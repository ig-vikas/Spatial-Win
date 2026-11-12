#include "dsp.hpp"
#include "phonon.h"
#include "phonon_version.h"
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <complex>
#include <vector>

using namespace spatial;
namespace {
constexpr int QUEUE=64, FFT=1024, PARTS=16;
using Complex=std::complex<float>;
void fft(Complex* a,bool inverse) {
    for(int i=1,j=0;i<FFT;i++){int bit=FFT>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
    for(int len=2;len<=FFT;len<<=1){float angle=2*PI/len*(inverse?1:-1);Complex root(cos(angle),sin(angle));for(int i=0;i<FFT;i+=len){Complex w(1,0);for(int j=0;j<len/2;j++){auto u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=root;}}}
    if(inverse)for(int i=0;i<FFT;i++)a[i]/=FFT;
}
struct Convolver {
    std::array<std::array<Complex,FFT>,PARTS> kernel{}, history{};
    std::array<Complex,FFT> work{};
    std::array<float,BLOCK> overlap{};
    int parts=0,cursor=0;
    void prepare(const float* ir,int size){parts=std::min(PARTS,(size+BLOCK-1)/BLOCK);for(int p=0;p<parts;p++){for(int i=0;i<BLOCK;i++)if(p*BLOCK+i<size)kernel[p][i]=ir[p*BLOCK+i];fft(kernel[p].data(),false);}}
    void apply(float* data){
        if(parts==0)return;
        auto& current=history[cursor];
        for(int i=0;i<BLOCK;i++){current[i]=overlap[i];current[i+BLOCK]=data[i];overlap[i]=data[i];}
        fft(current.data(),false);work.fill(Complex{});
        for(int p=0;p<parts;p++){auto& prev=history[(cursor-p+PARTS)%PARTS];for(int i=0;i<FFT;i++)work[i]+=prev[i]*kernel[p][i];}
        fft(work.data(),true);for(int i=0;i<BLOCK;i++)data[i]=work[i+BLOCK].real();cursor=(cursor+1)%PARTS;
    }
};
struct Bank {
    IPLHRTF hrtf=nullptr;
    IPLBinauralEffect effects[SOURCES][CHANNELS]{};
    EarAlignment alignment[SOURCES][CHANNELS];
    Convolver room[2]; bool valid=false,customRoom=false;
    Bank(IPLContext context,const char* path,const float* ir,int count,int channels) {
        IPLAudioSettings audio{RATE,BLOCK};IPLHRTFSettings settings{};
        settings.type=(path && *path)?IPL_HRTFTYPE_SOFA:IPL_HRTFTYPE_DEFAULT;
        settings.sofaFileName=path;settings.volume=1;settings.normType=IPL_HRTFNORMTYPE_RMS;
        if(iplHRTFCreate(context,&audio,&settings,&hrtf)!=IPL_STATUS_SUCCESS)return;
        IPLBinauralEffectSettings e{hrtf};
        for(auto& source:effects)for(auto& fx:source)if(iplBinauralEffectCreate(context,&audio,&e,&fx)!=IPL_STATUS_SUCCESS)return;
        if(ir && count>0){std::vector<float> mono(count);for(int c=0;c<2;c++){for(int i=0;i<count;i++)mono[i]=ir[i*channels+std::min(c,channels-1)];room[c].prepare(mono.data(),count);}customRoom=true;}
        valid=true;
    }
    ~Bank(){for(auto& s:effects)for(auto& e:s)if(e)iplBinauralEffectRelease(&e);if(hrtf)iplHRTFRelease(&hrtf);}
    void resetSource(int slot) {
        for (int channel=0;channel<CHANNELS;++channel) {
            iplBinauralEffectReset(effects[slot][channel]);
            alignment[slot][channel]={};
        }
    }
};
struct BlockData {int generation=0,channels=0,frames=0;uint32_t mask=0;int64_t position=0;float samples[BLOCK*CHANNELS]{};};
struct Stream {
    std::array<BlockData,QUEUE> queue;
    std::atomic<uint32_t> read{0},write{0};
    std::atomic<int> generation{0},ended{0};
    std::atomic<int64_t> position{0};
    std::atomic<int64_t> livePathFrames{0};
    std::atomic<uint32_t> underruns{0};
    std::atomic<float> loudnessGain{1};
    int renderedGeneration=-1;bool started=false; // Owned only by the render thread.
    float az=0,el=0,dist=1.5,gain=0,width=60;Biquad lfe;
    float overlayPan=0.f;
    bool livePrimed=false;
    float liveOffset=0,liveRatio=1;
    int64_t lastLiveInput=-1;
    std::array<float,2> lastLive{},liveFadeStart{};
    int liveFadeLeft=0;

    void resetLive() {
        livePrimed=false;liveOffset=0;liveRatio=1;lastLiveInput=-1;
        lastLive={};liveFadeStart={};liveFadeLeft=0;
    }
};

// A bounded live queue absorbs capture burstiness. Follow slow clock drift without
// dropping whole audio blocks, and fade marked discontinuities with both ears together.
void readLive(Stream& stream,uint32_t& read,uint32_t write,float (&output)[BLOCK][2]) {
    constexpr int FadeFrames=96;
    const auto queued=static_cast<int>((write-read)*BLOCK);
    if(!stream.livePrimed && queued>=static_cast<int>(LIVE_PRIME_BLOCKS)*BLOCK) {
        stream.livePrimed=true;stream.liveRatio=1.f;
        stream.liveFadeStart={};stream.liveFadeLeft=FadeFrames;
    }
    const float error=(queued-stream.liveOffset)/BLOCK-LIVE_PRIME_BLOCKS;
    const float targetRatio=1.f+clamp(error*.003f,-.009f,.009f);
    stream.liveRatio+=.12f*(targetRatio-stream.liveRatio);
    const auto needed=static_cast<int>(stream.liveOffset+(BLOCK-1)*stream.liveRatio)+1;
    if(!stream.livePrimed || queued<needed) {
        if(stream.livePrimed) {
            stream.underruns.fetch_add(1,std::memory_order_relaxed);
            const auto tail=stream.lastLive;
            for(int i=0;i<FadeFrames;++i)for(int c=0;c<2;++c)
                output[i][c]=tail[c]*(1.f-static_cast<float>(i+1)/FadeFrames);
            read=write;
            stream.resetLive();
        }
        return;
    }

    float offset=stream.liveOffset;
    for(int i=0;i<BLOCK;++i) {
        const int a=static_cast<int>(offset),b=std::min(a+1,queued-1);
        const auto& current=stream.queue[(read+a/BLOCK)%QUEUE];
        const auto& next=stream.queue[(read+b/BLOCK)%QUEUE];
        const auto frame=current.position+a%BLOCK;
        if(stream.lastLiveInput>=0 && frame>stream.lastLiveInput+3) {
            stream.liveFadeStart=stream.lastLive;
            stream.liveFadeLeft=FadeFrames;
        }
        const bool contiguous=next.position+b%BLOCK==frame+1;
        const float fraction=contiguous?offset-a:0.f;
        const float gain=static_cast<float>(FadeFrames-stream.liveFadeLeft)/FadeFrames;
        for(int c=0;c<2;++c) {
            const float first=current.samples[(a%BLOCK)*2+c];
            const float second=next.samples[(b%BLOCK)*2+c];
            const float sample=first+fraction*(second-first);
            output[i][c]=stream.liveFadeLeft>0 ? stream.liveFadeStart[c]+gain*(sample-stream.liveFadeStart[c]) : sample;
            stream.lastLive[c]=output[i][c];
        }
        if(stream.liveFadeLeft>0)--stream.liveFadeLeft;
        stream.lastLiveInput=frame;
        offset+=stream.liveRatio;
    }
    const auto consumed=static_cast<unsigned>(offset/BLOCK);
    stream.liveOffset=offset-consumed*BLOCK;
    read+=consumed;
    stream.position.store(stream.lastLiveInput+1,std::memory_order_relaxed);
}
struct Engine {
    IPLContext context=nullptr;Bank* current=nullptr;Bank* old=nullptr;
    std::atomic<Bank*> pending{nullptr},retired{nullptr};int fade=0;
    std::array<Stream,SOURCES> streams;
    std::atomic<float> mailbox[PARAMS];std::atomic<unsigned> version{0};
    std::array<float,PARAMS> params{},candidate{},previous{};
    float inputs[SOURCES][CHANNELS][BLOCK]{};
    float dry[2][BLOCK]{},mix[2][BLOCK]{},oldMix[2][BLOCK]{},overlayMix[2][BLOCK]{},mono[BLOCK]{},ear[2][BLOCK]{};
    float* monoPtr[1]{mono};float* earPtr[2]{ear[0],ear[1]};
    Biquad eq[2][16];Limiter limiter;Compressor compressor;
    std::array<std::array<float,2>,4096> delay{};int delayPos=0;
    EarlyReflections room;
    float preamp=1,roomWet=0,spatialWet=0,peak=0,physicalWet=0;
    float balance=0,monoMix=0,leftGain=1,rightGain=1,leftDelay=0,rightDelay=0;
    float yaw=0,pitch=0,roll=0;
    std::atomic<float> meterPeak{0},meterReduction{0},headroom{0};
    std::atomic<int64_t> clock{0},seekClock{-1};
    int channelCounts[SOURCES]{};uint32_t masks[SOURCES]{};bool valid=false;
    Engine(){
        for(auto& f:mailbox)f.store(0);previous.fill(-999);
        IPLContextSettings settings{};settings.version=STEAMAUDIO_VERSION;
        if(iplContextCreate(&settings,&context)!=IPL_STATUS_SUCCESS)return;
        current=new Bank(context,nullptr,nullptr,0,1);valid=current->valid;
        for(auto& s:streams)s.lfe.c=s.lfe.target=filter(3,120,0,.707);
    }
    ~Engine(){delete current;delete old;delete pending.exchange(nullptr);delete retired.exchange(nullptr);if(context)iplContextRelease(&context);}
    void snapshot(){unsigned v=version.load(std::memory_order_acquire);if(v&1)return;for(int i=0;i<PARAMS;i++)candidate[i]=mailbox[i].load(std::memory_order_relaxed);if(v==version.load(std::memory_order_acquire))params=candidate;}
    void updateEq(){
        bool changed=params[16]!=previous[16]||params[25]!=previous[25];
        for(int i=3;i<=6;i++)changed|=params[i]!=previous[i];
        for(int i=64;i<124;i++)changed|=params[i]!=previous[i];
        if(!changed)return;
        for(int c=0;c<2;c++){
            const float hz[4]{100,350,2800,9000};
            for(int b=0;b<4;b++)eq[c][b].set(params[16]>.5?Coeff{}:filter(b==0?1:b==3?2:0,hz[b],params[3+b]+(b==0?params[25]:0),.707));
            for(int b=0;b<12;b++){int o=64+b*5;eq[c][b+4].set(params[16]>.5||params[o+4]<.5?Coeff{}:filter(int(params[o+3]),params[o],params[o+1],params[o+2]));}
        }previous=params;
    }
    void renderBank(Bank* bank,float dest[2][BLOCK]){
        std::memset(dest,0,sizeof(float)*2*BLOCK);
        IPLAudioBuffer in{1,BLOCK,monoPtr},out{2,BLOCK,earPtr};
        for(int s=0;s<SOURCES;s++){
            auto& stream=streams[s];int ch=channelCounts[s];if(ch==0)continue;
            for(int c=0;c<ch;c++){
                float angle=stream.az+(ch==2?(c==0?-stream.width/2:stream.width/2):ch>2?channelAngle(ch,c,masks[s]):0);
                auto d=viewpoint(direction(angle,stream.el),yaw,pitch,roll);
                float peaks[2]{};
                IPLBinauralEffectParams p{};p.direction={d.x,d.y,d.z};p.interpolation=IPL_HRTFINTERPOLATION_BILINEAR;p.spatialBlend=1;p.hrtf=bank->hrtf;p.peakDelays=peaks;
                std::memcpy(mono,inputs[s][c],sizeof(mono));
                iplBinauralEffectApply(bank->effects[s][c],&p,&in,&out);
                bank->alignment[s][c].apply(ear,peaks);
                for(int i=0;i<BLOCK;i++){dest[0][i]+=ear[0][i];dest[1][i]+=ear[1][i];}
            }
        }
        if(bank->customRoom){
            float wet[2][BLOCK];std::memcpy(wet,dest,sizeof(wet));
            bank->room[0].apply(wet[0]);bank->room[1].apply(wet[1]);
            for(int c=0;c<2;c++)for(int i=0;i<BLOCK;i++)dest[c][i]+=wet[c][i]*roomWet;
        }
    }
    void render(float* output,bool playing){
        snapshot();updateEq();
        yaw+=.2f*std::remainder(params[13]-yaw,360.f);
        pitch+=.2f*std::remainder(params[14]-pitch,360.f);
        roll+=.2f*std::remainder(params[15]-roll,360.f);
        if(!old && !retired.load(std::memory_order_acquire)){auto* next=pending.exchange(nullptr,std::memory_order_acq_rel);if(next){old=current;current=next;fade=0;}}
        int64_t requested=seekClock.exchange(-1);if(requested>=0)clock.store(requested);
        std::memset(inputs,0,sizeof(inputs));std::memset(dry,0,sizeof(dry));std::memset(overlayMix,0,sizeof(overlayMix));
        bool anySolo=false;int count=0;for(int s=0;s<SOURCES;s++){auto* p=&params[128+s*64];anySolo|=p[0]>.5&&p[6]>.5;if(p[0]>.5&&p[5]<.5)count++;}
        for(int s=0;s<SOURCES;s++){
            auto& stream=streams[s];auto* p=&params[128+s*64];
            auto r=stream.read.load(std::memory_order_relaxed),w=stream.write.load(std::memory_order_acquire);int gen=stream.generation.load(std::memory_order_acquire);
            if(gen!=stream.renderedGeneration){
                if(stream.started||stream.livePrimed){current->resetSource(s);if(old)old->resetSource(s);}
                stream.renderedGeneration=gen;stream.started=false;stream.resetLive();
            }
            // Drain stale generations even while paused. Valid PCM is never consumed while paused.
            int drained=0;while(r!=w && stream.queue[r%QUEUE].generation!=gen && drained++<QUEUE){r++;}stream.read.store(r,std::memory_order_release);
            channelCounts[s]=0;if(!playing || p[0]<.5)continue;
            const bool live=p[12]>.5f;
            const bool overlay=live&&p[13]>.5f;
            if(!live&&r==w){if(stream.started&&stream.ended.load()!=gen)stream.underruns.fetch_add(1);continue;}
            const BlockData* block=live?nullptr:&stream.queue[r%QUEUE];
            const int ch=live?2:block->channels;
            channelCounts[s]=ch;masks[s]=live?3:block->mask;
            float captured[BLOCK][2]{};
            if(live)readLive(stream,r,w,captured);
            else stream.started=true;
            float az=p[1],el=p[2],distance=p[3];
            if(p[8]>.5 && p[9]<.5){
                PathPoint points[8];
                const int count=std::clamp(int(p[16]),0,8);
                for(int i=0;i<count;i++)points[i]={p[17+i*4],p[18+i*4],p[19+i*4],p[20+i*4]};
                const auto frames=live?stream.livePathFrames.load(std::memory_order_relaxed):clock.load(std::memory_order_relaxed);
                const auto target=pathAt(points,count,std::max(0.f,float(frames)/RATE-p[14]),p[11]>.5,p[10],live);
                az=target.azimuth;el=target.elevation;distance=target.distance;
            }
            if(live&&stream.livePrimed)stream.livePathFrames.fetch_add(BLOCK,std::memory_order_relaxed);
            stream.az+=.25f*std::remainder(az-stream.az,360.f);stream.el+=.25f*(el-stream.el);stream.dist+=.2f*(clamp(distance,.3f,8.f)-stream.dist);stream.width+=.25f*(p[7]-stream.width);
            float targetGain=(p[5]>.5||(anySolo&&p[6]<.5))?0:db(p[4]);
            targetGain*=params[10]>.5?stream.loudnessGain.load():1;
            float atten=1/std::max(1.f,stream.dist),centerGain=db(params[12]);
            if(overlay) {
                channelCounts[s]=0;
                // 1 = fixed Windows PC field. 2 = movable Android capture.
                const bool moving=p[13]>1.5f;
                const float pan=moving?viewpoint(direction(stream.az,stream.el),yaw,pitch,roll).x:0.f;
                const float gain=std::min(targetGain,1.f)*(moving?std::min(1.f,1.5f*atten):1.f);
                for(int i=0;i<BLOCK;++i){
                    stream.gain+=.004f*(gain-stream.gain);
                    stream.overlayPan+=.004f*(pan-stream.overlayPan);
                    float left=captured[i][0],right=captured[i][1];
                    stereoOverlay(left,right,stream.overlayPan);
                    overlayMix[0][i]+=left*stream.gain;
                    overlayMix[1][i]+=right*stream.gain;
                }
                stream.read.store(r,std::memory_order_release);
                continue;
            }
            for(int i=0;i<BLOCK;i++){
                stream.gain+=.004f*(targetGain-stream.gain);
                for(int c=0;c<ch;c++){
                    float x=(live?captured[i][c]:i<block->frames?block->samples[i*ch+c]:0.f)*stream.gain;
                    if(ch>2&&c==3)x=stream.lfe.tick(x)*.5f;
                    if(ch>2&&c==2)x*=centerGain;
                    inputs[s][c][i]=x*atten;
                    if(ch==1){dry[0][i]+=x*.7071f;dry[1][i]+=x*.7071f;}
                    else if(ch==2)dry[c][i]+=x;
                    else {float a=channelAngle(ch,c,masks[s])*PI/180;float pan=std::sin(a);dry[0][i]+=x*std::sqrt((1-pan)/2.f)*.5f;dry[1][i]+=x*std::sqrt((1+pan)/2.f)*.5f;}
                }
                if(ch==2){float mid=(inputs[s][0][i]+inputs[s][1][i])*.5f/atten;float lift=centerGain-1;dry[0][i]+=mid*lift;dry[1][i]+=mid*lift;float shared=(inputs[s][0][i]+inputs[s][1][i])*.5f*lift;inputs[s][0][i]+=shared;inputs[s][1][i]+=shared;}
            }
            if(!live){stream.position.store(block->position+block->frames);++r;}
            stream.read.store(r,std::memory_order_release);
        }
        if(!playing){std::memset(output,0,BLOCK*2*sizeof(float));return;}
        roomWet+=.15f*(params[11]-roomWet);
        if((params[0]>.5 && params[1]<.5)||spatialWet>.0001f||old)renderBank(current,mix);
        else std::memset(mix,0,sizeof(mix));
        if(old){renderBank(old,oldMix);float f=std::min(1.f,float(++fade)/12);for(int c=0;c<2;c++)for(int i=0;i<BLOCK;i++)mix[c][i]=mix[c][i]*f+oldMix[c][i]*(1-f);if(f>=1){retired.store(old,std::memory_order_release);old=nullptr;}}
        float boost=0;if(params[16]<.5){for(int b=3;b<=6;b++)boost+=std::max(0.f,params[b]);for(int b=0;b<12;b++)if(params[64+b*5+4]>.5)boost+=std::max(0.f,params[64+b*5+1]);}
        float safety=params[17]>.5?boost+20*log10(std::max(1,count)):0;headroom.store(safety);
        float targetPre=db(params[2]-safety+params[26])*clamp(params[18],0,1);
        float wetTarget=params[0]>.5 && params[1]<.5?1:0;
        int ld=int(std::max(0.f,params[20]-params[19])/343.f*RATE),rd=int(std::max(0.f,params[19]-params[20])/343.f*RATE);
        ld=std::clamp(ld,0,4095);rd=std::clamp(rd,0,4095);
        float lg=db(params[21]),rg=db(params[22]);peak=0;
        for(int i=0;i<BLOCK;i++){
            preamp+=.002f*(targetPre-preamp);spatialWet+=.003f*(wetTarget-spatialWet);physicalWet+=.003f*(params[1]-physicalWet);
            float l=dry[0][i]+spatialWet*(mix[0][i]-dry[0][i]),r=dry[1][i]+spatialWet*(mix[1][i]-dry[1][i]);
            if(!current->customRoom)room.process(l,r,roomWet*spatialWet);
            l+=overlayMix[0][i]*spatialWet;r+=overlayMix[1][i]*spatialWet;
            l*=preamp;r*=preamp;
            if(params[16]<.5)for(int b=0;b<16;b++){l=eq[0][b].tick(l);r=eq[1][b].tick(r);}
            if(params[9]>.5)compressor.process(l,r,true);
            monoMix+=.003f*(params[8]-monoMix);balance+=.003f*(params[7]-balance);
            float m=(l+r)*.5f;l+=monoMix*(m-l);r+=monoMix*(m-r);
            l*=1-std::max(0.f,balance);r*=1+std::min(0.f,balance);
            leftGain+=.003f*(lg-leftGain);rightGain+=.003f*(rg-rightGain);
            leftDelay+=.001f*(ld-leftDelay);rightDelay+=.001f*(rd-rightDelay);
            delay[delayPos]={l,r};
            auto delayed=[&](float d,int ch){int a=int(d);float f=d-a;return delay[(delayPos+4096-a)%4096][ch]*(1-f)+delay[(delayPos+4095-a)%4096][ch]*f;};
            float pl=delayed(leftDelay,0)*leftGain,pr=delayed(rightDelay,1)*rightGain;delayPos=(delayPos+1)%4096;
            l+=physicalWet*(pl-l);r+=physicalWet*(pr-r);
            limiter.process(l,r);output[i*2]=l;output[i*2+1]=r;peak=std::max({peak,std::abs(l),std::abs(r)});
        }
        meterPeak.store(peak);meterReduction.store(limiter.reduction);clock.fetch_add(BLOCK);
    }
};
int beginStream(Engine& engine, int slot, int64_t position) {
    auto& stream=engine.streams[slot];stream.position.store(position);stream.ended.store(-1);stream.loudnessGain.store(1);
    return stream.generation.fetch_add(1)+1;
}
void sendControls(Engine& engine, const float* values) {
    engine.version.fetch_add(1,std::memory_order_acq_rel);
    for(int i=0;i<PARAMS;i++)engine.mailbox[i].store(std::isfinite(values[i])?values[i]:0,std::memory_order_relaxed);
    engine.version.fetch_add(1,std::memory_order_release);
}
bool pushPcm(Engine& engine,int slot,int generation,const float* data,int frames,int channels,int mask,int64_t position,unsigned limit=QUEUE) {
    if(slot<0||slot>=SOURCES||frames<1||frames>BLOCK||!verifiedMask(channels,mask)||!data)return false;
    auto& stream=engine.streams[slot];if(generation!=stream.generation.load())return false;
    auto write=stream.write.load(std::memory_order_relaxed),read=stream.read.load(std::memory_order_acquire);
    if(write-read>=limit)return false;
    auto& block=stream.queue[write%QUEUE];block.generation=generation;block.channels=channels;block.mask=mask;block.frames=frames;block.position=position;
    std::memcpy(block.samples,data,frames*channels*sizeof(float));stream.write.store(write+1,std::memory_order_release);return true;
}
#ifdef __ANDROID__
Engine* engine(jlong h){return reinterpret_cast<Engine*>(h);}
#endif
}
#ifdef __ANDROID__
extern "C" {
JNIEXPORT jlong JNICALL Java_com_spatial_app_audio_NativeEngine_create(JNIEnv*,jobject){auto* e=new Engine;if(!e->valid){delete e;return 0;}return reinterpret_cast<jlong>(e);}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_destroy(JNIEnv*,jobject,jlong h){delete engine(h);}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_parameters(JNIEnv* env,jobject,jlong h,jfloatArray a){if(env->GetArrayLength(a)!=PARAMS)return;float v[PARAMS];env->GetFloatArrayRegion(a,0,PARAMS,v);sendControls(*engine(h),v);}
JNIEXPORT jint JNICALL Java_com_spatial_app_audio_NativeEngine_begin(JNIEnv*,jobject,jlong h,jint slot,jlong position){return slot<0||slot>=SOURCES?-1:beginStream(*engine(h),slot,position);}
JNIEXPORT jboolean JNICALL Java_com_spatial_app_audio_NativeEngine_push(JNIEnv* env,jobject,jlong h,jint slot,jint gen,jobject buffer,jint frames,jint ch,jint mask,jlong position){
    if(frames<1||frames>BLOCK||ch<1||ch>CHANNELS||env->GetDirectBufferCapacity(buffer)<frames*ch*4)return false;
    return pushPcm(*engine(h),slot,gen,static_cast<float*>(env->GetDirectBufferAddress(buffer)),frames,ch,mask,position);
}
JNIEXPORT jboolean JNICALL Java_com_spatial_app_audio_NativeEngine_pushLive(JNIEnv* env,jobject,jlong h,jint slot,jint gen,jobject buffer,jlong position){
    if(env->GetDirectBufferCapacity(buffer)<BLOCK*8)return false;
    return pushPcm(*engine(h),slot,gen,static_cast<float*>(env->GetDirectBufferAddress(buffer)),BLOCK,2,3,position,LIVE_QUEUE_BLOCKS);
}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_end(JNIEnv*,jobject,jlong h,jint slot,jint gen){if(slot>=0&&slot<SOURCES)engine(h)->streams[slot].ended.store(gen);}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_level(JNIEnv*,jobject,jlong h,jint slot,jfloat gain){if(slot>=0&&slot<SOURCES)engine(h)->streams[slot].loudnessGain.store(clamp(gain,.1f,4.f));}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_render(JNIEnv* env,jobject,jlong h,jobject buffer,jboolean playing){auto* data=static_cast<float*>(env->GetDirectBufferAddress(buffer));if(data&&env->GetDirectBufferCapacity(buffer)>=BLOCK*8)engine(h)->render(data,playing);}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_seekClock(JNIEnv*,jobject,jlong h,jlong frames){engine(h)->seekClock.store(frames);}
JNIEXPORT jfloatArray JNICALL Java_com_spatial_app_audio_NativeEngine_metrics(JNIEnv* env,jobject,jlong h){auto* e=engine(h);float v[24]{};v[0]=e->meterPeak.load();v[1]=e->meterReduction.load();v[2]=e->headroom.load();v[3]=float(e->clock.load())/RATE;for(int i=0;i<SOURCES;i++){auto& s=e->streams[i];v[4+i]=float(s.position.load())/RATE;v[8+i]=float(s.underruns.load());v[12+i]=float(s.write.load()-s.read.load());v[16+i]=s.ended.load()==s.generation.load()?1:0;}auto result=env->NewFloatArray(24);env->SetFloatArrayRegion(result,0,24,v);return result;}
// Called only by the serialized preparation worker. Bank retirement/deletion never occurs on render.
JNIEXPORT jboolean JNICALL Java_com_spatial_app_audio_NativeEngine_prepare(JNIEnv* env,jobject,jlong h,jstring path,jfloatArray impulse,jint channels){auto* e=engine(h);delete e->retired.exchange(nullptr);if(e->pending.load())return false;const char* name=path?env->GetStringUTFChars(path,nullptr):nullptr;int count=impulse?env->GetArrayLength(impulse):0;if(channels<1||channels>2||count>8192*channels){if(name)env->ReleaseStringUTFChars(path,name);return false;}std::vector<float> ir(count);if(count)env->GetFloatArrayRegion(impulse,0,count,ir.data());auto* bank=new Bank(e->context,name,count?ir.data():nullptr,count/channels,channels);if(name)env->ReleaseStringUTFChars(path,name);if(!bank->valid){delete bank;return false;}e->pending.store(bank,std::memory_order_release);return true;}
JNIEXPORT void JNICALL Java_com_spatial_app_audio_NativeEngine_collect(JNIEnv*,jobject,jlong h){delete engine(h)->retired.exchange(nullptr);}
}


#endif
