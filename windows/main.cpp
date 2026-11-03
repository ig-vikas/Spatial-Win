#include <windows.h>
#include <windowsx.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <thread>
#include <string_view>
#include <unordered_map>
#include "../app/src/main/cpp/engine.cpp"

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace {
constexpr UINT AudioError=WM_APP+1, InputSignal=WM_APP+2;
constexpr wchar_t PcAudio[]=L"spatial:pc-audio";
constexpr COLORREF Background=RGB(8,9,11), PanelColor=RGB(22,24,29), Ink=RGB(239,241,248), Soft=RGB(183,187,200);
constexpr COLORREF Colors[]{RGB(230,232,241),RGB(241,201,149),RGB(198,184,245),RGB(159,205,245)};
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
};
struct Event {
    HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    ~Event(){if(handle)CloseHandle(handle);}
};
void check(HRESULT hr,const char* info){if(FAILED(hr))throw std::runtime_error(info);}
std::wstring widen(const char* value) {return std::wstring(value,value+strlen(value));}
WAVEFORMATEX stereoFormat(){return {WAVE_FORMAT_IEEE_FLOAT,2,RATE,RATE*8,8,32,0};}
ComPtr<IMMDeviceEnumerator> devices(){
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&result)),"Windows audio service is unavailable");
    return result;
}
ComPtr<IMMDevice> endpoint(const std::wstring& id){
    auto enumerator=devices();ComPtr<IMMDevice> result;
    if(id.empty())check(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&result),"Connect an audio output and press Play again");
    else check(enumerator->GetDevice(id.c_str(),&result),"The selected output disconnected");
    return result;
}
std::wstring endpointId(IMMDevice* device){
    LPWSTR raw=nullptr;check(device->GetId(&raw),"Cannot read audio endpoint ID");std::wstring result(raw);CoTaskMemFree(raw);return result;
}
std::wstring endpointName(IMMDevice* device){
    ComPtr<IPropertyStore> store;PROPVARIANT value;PropVariantInit(&value);std::wstring result=L"Audio output";
    if(SUCCEEDED(device->OpenPropertyStore(STGM_READ,&store)) && SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt==VT_LPWSTR)result=value.pwszVal;
    PropVariantClear(&value);return result;
}
struct SourceEntry {
    std::wstring file,name,endpoint;
    float angle=0,distance=1.5f,pathOrigin=0.f;
    bool muted=false,follow=false;
    std::vector<PathPoint> path;
    bool exists() const {return !file.empty()||!endpoint.empty();}
    bool external() const {return !endpoint.empty();}
    bool pcAudio() const {return endpoint==PcAudio;}
};

class PcmBlocks {
    std::array<float,BLOCK*2> block{};
    int used=0;
    int64_t position=0;
    bool wasAudible=false;
    int silentBlocks=0;
    float captureLimit=1.f;
    HWND window;
    Engine& engine;
    int slot, generation;
    std::stop_token stop;
    bool live;
    void flush() {
        if(!used)return;
        if(live) {
            float peak=0.f;
            for(int i=0;i<used*2;++i)peak=std::max(peak,std::abs(block[i]));
            if(peak>captureLimit){
                const float gain=captureLimit/peak;
                for(int i=0;i<used*2;++i)block[i]*=gain;
                peak=captureLimit;
            }
            const bool signal=peak>.0003f;
            silentBlocks=signal?0:silentBlocks+1;
            bool audible=signal||(wasAudible&&silentBlocks<94);
            if(audible)pushPcm(engine,slot,generation,block.data(),used,2,3,position,LIVE_QUEUE_BLOCKS);
            if(audible!=wasAudible){wasAudible=audible;PostMessageW(window,InputSignal,slot,audible);}
        } else {
            while(!stop.stop_requested() && !pushPcm(engine,slot,generation,block.data(),used,2,3,position))std::this_thread::sleep_for(12ms);
        }
        position+=used;used=0;
    }
public:
    PcmBlocks(HWND win,Engine& e,int s,int g,std::stop_token token,bool capture,int64_t start=0):position(start),window(win),engine(e),slot(s),generation(g),stop(token),live(capture){}
    void sample(float left,float right){
        block[used*2]=std::isfinite(left)?left:0;block[used*2+1]=std::isfinite(right)?right:0;
        if(++used==BLOCK)flush();
    }
    void finish(){if(!live)flush();}
    void limitCapture(float limit){captureLimit=limit;}
    void discontinuity(){
        if(!live)return;
        // Stamp a gap so the mixer ramps across the discontinuity instead of splicing abruptly.
        position+=used+BLOCK;
        used=0;
    }
    void idle(){
        if(!live)return;
        used=0;
        silentBlocks=0;
        if(wasAudible){wasAudible=false;PostMessageW(window,InputSignal,slot,FALSE);}
    }
};

// Maintains fractional phase across decoder packets and passes native 48 kHz through exactly.
class Converter {
    int rate;
    double inputIndex=0,next=0;
    float previous[2]{};
public:
    explicit Converter(int value):rate(value){}
    void sample(float left,float right,PcmBlocks& sink){
        if(rate==RATE){sink.sample(left,right);return;}
        if(inputIndex==0){previous[0]=left;previous[1]=right;}
        while(next<=inputIndex){
            const float x=std::clamp(static_cast<float>(next-inputIndex+1.),0.f,1.f);
            sink.sample(previous[0]+(left-previous[0])*x,previous[1]+(right-previous[1])*x);
            next+=static_cast<double>(rate)/RATE;
        }
        previous[0]=left;previous[1]=right;++inputIndex;
    }
};
void readFile(const std::wstring& path,std::stop_token stop,PcmBlocks& sink,int64_t seekFrames=0){
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"This file could not be opened by Windows Media Foundation");
    check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Cannot select audio stream");
    check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),"This file has no audio stream");
    ComPtr<IMFMediaType> request,format;
    check(MFCreateMediaType(&request),"Cannot prepare the audio decoder");
    request->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);request->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);
    check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,request.Get()),"Windows has no decoder for this file");
    check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,&format),"Cannot read decode format");
    UINT32 rate=0,channels=0,bits=0;
    format->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&rate);format->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&channels);format->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,&bits);
    if(rate<8000||rate>192000||channels<1||channels>2||(bits!=16&&bits!=24&&bits!=32))throw std::runtime_error("Choose a mono or stereo PCM, MP3 or AAC file");
    if(seekFrames>0) {
        PROPVARIANT position;InitPropVariantFromInt64(seekFrames*10'000'000/RATE,&position);
        check(reader->SetCurrentPosition(GUID_NULL,position),"This file cannot be resumed");PropVariantClear(&position);
    }
    const int stride=bits/8;
    auto amplitude=[bits](const BYTE* p){
        if(bits==16){int16_t n;memcpy(&n,p,2);return n/32768.f;}
        if(bits==24){int32_t n=(p[0]<<8)|(p[1]<<16)|(static_cast<unsigned>(p[2])<<24);return n/2147483648.f;}
        int32_t n;memcpy(&n,p,4);return n/2147483648.f;
    };
    Converter convert(rate);
    while(!stop.stop_requested()) {
        DWORD flags=0;ComPtr<IMFSample> sample;
        check(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,nullptr,&sample),"Playback stopped because the decoder failed");
        if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)throw std::runtime_error("Audio format changed during playback");
        if(sample){
            ComPtr<IMFMediaBuffer> buffer;check(sample->ConvertToContiguousBuffer(&buffer),"Cannot read decoded buffer");
            BYTE* bytes=nullptr;DWORD length=0;check(buffer->Lock(&bytes,nullptr,&length),"Cannot read decoded samples");
            for(DWORD i=0;i+stride*channels<=length && !stop.stop_requested();i+=stride*channels){
                const float left=amplitude(bytes+i);convert.sample(left,channels==2?amplitude(bytes+i+stride):left,sink);
            }
            buffer->Unlock();
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
    }
    sink.finish();
}

class LoopbackActivation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase,IActivateAudioInterfaceCompletionHandler> {
public:
    Event done;
    HRESULT result=E_PENDING;
    ComPtr<IAudioClient> client;
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT activationResult=E_FAIL;
        result=operation->GetActivateResult(&activationResult,&activated);
        if(SUCCEEDED(result))result=activationResult;
        if(SUCCEEDED(result)&&activated)result=activated.As(&client);
        SetEvent(done.handle);
        return S_OK;
    }
};

ComPtr<IAudioClient> pcAudioClient(std::stop_token stop,DWORD process,bool include){
    auto callback=Microsoft::WRL::Make<LoopbackActivation>();
    if(!callback||!callback->done.handle)throw std::runtime_error("Could not prepare PC audio");
    AUDIOCLIENT_ACTIVATION_PARAMS options{};
    options.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    options.ProcessLoopbackParams.TargetProcessId=process;
    options.ProcessLoopbackParams.ProcessLoopbackMode=include
        ?PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE:PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT parameters;
    PropVariantInit(&parameters);
    parameters.vt=VT_BLOB;
    parameters.blob.cbSize=sizeof(options);
    parameters.blob.pBlobData=reinterpret_cast<BYTE*>(&options);
    ComPtr<IActivateAudioInterfaceAsyncOperation> activation;
    constexpr auto unsupported="PC audio is unavailable. Use a supported Windows 11 build or choose a routed input.";
    check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),
        &parameters,callback.Get(),&activation),unsupported);
    const auto deadline=std::chrono::steady_clock::now()+4s;
    while(!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline){
        if(WaitForSingleObject(callback->done.handle,50)==WAIT_OBJECT_0){
            check(callback->result,unsupported);
            if(!callback->client)throw std::runtime_error(unsupported);
            return callback->client;
        }
    }
    if(stop.stop_requested())return {};
    throw std::runtime_error("Windows did not start PC audio. Try reconnecting.");
}

// A processing relay can replay Spatial's output from a different process.
// FxSound captures the virtual output and opens a second WASAPI render session.
bool isFxSoundRelay(DWORD pid){
    if(!pid)return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return false;
    wchar_t image[512]{};
    DWORD size=static_cast<DWORD>(std::size(image));
    const bool read=QueryFullProcessImageNameW(process,0,image,&size)!=FALSE;
    CloseHandle(process);
    if(!read)return false;
    const auto* basename=wcsrchr(image,L'\\');
    return _wcsicmp(basename?basename+1:image,L"FxSound.exe")==0;
}

// Authorize overlay gain only from sessions that can provide independent audio.
class ExternalAudioMeter {
    struct Session {
        ComPtr<IAudioSessionControl2> control;
        ComPtr<IAudioMeterInformation> meter;
        ComPtr<ISimpleAudioVolume> volume;
        DWORD processId=0;
    };
    ComPtr<IMMDeviceEnumerator> enumerator=devices();
    std::vector<Session> sessions;
    std::unordered_map<DWORD,bool> relays;
    DWORD relay=0;
    using Clock=std::chrono::steady_clock;
    Clock::time_point nextRefresh{},nextPoll{},expires{};
    Clock::time_point nextReport{};
    float peak=0.f;
    bool trace;

    void refresh(){
        sessions.clear();
        relay=0;
        ComPtr<IMMDeviceCollection> outputs;
        UINT count=0;
        if(FAILED(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&outputs))||
            FAILED(outputs->GetCount(&count)))return;
        for(UINT i=0;i<count;++i){
            ComPtr<IMMDevice> device;
            ComPtr<IAudioSessionManager2> manager;
            ComPtr<IAudioSessionEnumerator> active;
            int total=0;
            if(FAILED(outputs->Item(i,&device))||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,
                    reinterpret_cast<void**>(manager.GetAddressOf())))||
                FAILED(manager->GetSessionEnumerator(&active))||FAILED(active->GetCount(&total)))continue;
            for(int j=0;j<total;++j){
                ComPtr<IAudioSessionControl> session;
                Session entry;
                DWORD pid=0;
                if(FAILED(active->GetSession(j,&session))||FAILED(session.As(&entry.control))||
                    FAILED(entry.control->GetProcessId(&pid))||pid==GetCurrentProcessId())continue;
                const auto [known,created]=relays.try_emplace(pid,false);
                if(created)known->second=isFxSoundRelay(pid);
                if(known->second){relay=pid;continue;}
                if(FAILED(session.As(&entry.meter)))continue;
                // Every Spatial render client uses a private process-local session.
                // Shared sessions and system sounds are still eligible here.
                session.As(&entry.volume);
                entry.processId=pid;
                sessions.push_back(std::move(entry));
            }
        }
    }
public:
    explicit ExternalAudioMeter(bool diagnostics):trace(diagnostics){
        refresh();
        nextRefresh=Clock::now()+200ms;
    }
    DWORD excludedProcess() const{return relay?relay:GetCurrentProcessId();}

    float poll(Clock::time_point now){
        if(now>=nextRefresh){refresh();nextRefresh=now+(peak>0.f?600ms:200ms);}
        if(now>=nextPoll){
            nextPoll=now+25ms;
            float observed=0.f;
            DWORD loudest=0;
            for(const auto& session:sessions){
                AudioSessionState state{};
                float level=0.f,volume=1.f;
                BOOL muted=FALSE;
                if(FAILED(session.control->GetState(&state))||state!=AudioSessionStateActive||
                    FAILED(session.meter->GetPeakValue(&level)))continue;
                if(session.volume){
                    session.volume->GetMute(&muted);
                    session.volume->GetMasterVolume(&volume);
                }
                if(!muted&&level*volume>observed){
                    observed=level*volume;
                    loudest=session.processId;
                }
            }
            if(trace&&now>=nextReport){
                std::fprintf(stderr,"External session: pid=%lu, peak=%.6f, sessions=%zu\n",loudest,observed,sessions.size());
                nextReport=now+1s;
            }
            if(observed>.00015f){
                peak=std::max(observed,peak*.90f);
                expires=now+180ms;
            }
            if(now>=expires)peak=0.f;
        }
        return peak;
    }
};

void loopback(const std::wstring& id,std::stop_token stop,PcmBlocks& sink,
    std::atomic<bool>* ready=nullptr,bool guardFeedback=true,bool diagnostics=false,DWORD testProcess=0){
    const bool allPc=id==PcAudio;
    std::optional<ExternalAudioMeter> external;
    if(allPc&&guardFeedback&&!testProcess)external.emplace(diagnostics);
    ComPtr<IAudioClient> client;
    if(allPc){
        const DWORD pid=testProcess?testProcess:external?external->excludedProcess():GetCurrentProcessId();
        client=pcAudioClient(stop,pid,testProcess!=0);
        if(!client)return;
    }
    else {
        auto input=endpoint(id);
        check(input->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"Cannot connect the capture endpoint");
    }
    Event event;
    WAVEFORMATEX* raw=nullptr;
    // Process loopback supports shared PCM conversion but doesn't expose an endpoint mix format.
    const WAVEFORMATEX processFormat{WAVE_FORMAT_PCM,2,44100,176400,4,16,0};
    if(!allPc)check(client->GetMixFormat(&raw),"Cannot read capture endpoint format");
    const std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)> endpointFormat(raw,&CoTaskMemFree);
    const auto* format=allPc?&processFormat:endpointFormat.get();
    const auto channels=format->nChannels;
    const auto bits=format->wBitsPerSample;
    const auto stride=format->nBlockAlign;
    const bool extensible=format->wFormatTag==WAVE_FORMAT_EXTENSIBLE && format->cbSize>=22;
    const auto* extended=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    const bool floating=format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT || (extensible&&extended->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const bool pcm=format->wFormatTag==WAVE_FORMAT_PCM || (extensible&&extended->SubFormat==KSDATAFORMAT_SUBTYPE_PCM);
    if(!channels||format->nSamplesPerSec<8000||format->nSamplesPerSec>192000||stride<channels*bits/8||
        !(floating&&bits==32)&&!(pcm&&(bits==16||bits==24||bits==32)))throw std::runtime_error("Unsupported capture endpoint format");
    const auto read=[&](const BYTE* p) {
        if(floating){float v;memcpy(&v,p,4);return v;}
        if(bits==16){int16_t v;memcpy(&v,p,2);return v/32768.f;}
        if(bits==24){uint32_t v=(p[0]<<8)|(p[1]<<16)|(static_cast<unsigned>(p[2])<<24);return static_cast<int32_t>(v)/2147483648.f;}
        int32_t v;memcpy(&v,p,4);return v/2147483648.f;
    };
    Converter convert(format->nSamplesPerSec);
    const DWORD flags=AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK|
        (allPc?AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM:0);
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,0,0,format,nullptr),"Windows could not start the PC audio capture stream");
    check(client->SetEventHandle(event.handle),"Cannot monitor the capture endpoint");
    ComPtr<IAudioCaptureClient> capture;check(client->GetService(IID_PPV_ARGS(&capture)),"Cannot open loopback stream");
    check(client->Start(),"Could not start desktop audio");
    if(ready)ready->store(true);
    bool wasAllowed=false;
    auto lastPacket=std::chrono::steady_clock::now();
    while(!stop.stop_requested()) {
        if(WaitForSingleObject(event.handle,100)==WAIT_FAILED)throw std::runtime_error("Audio capture event failed");
        const auto now=std::chrono::steady_clock::now();
        const float peak=external?external->poll(now):1.f;
        const bool allowed=peak>0.f;
        if(allowed!=wasAllowed){
            sink.discontinuity();
            convert=Converter(format->nSamplesPerSec);
            if(!allowed)sink.idle();
            wasAllowed=allowed;
        }
        if(external)sink.limitCapture(std::min(1.f,peak*2.f));
        UINT32 packet=0;
        while(!stop.stop_requested()) {
            check(capture->GetNextPacketSize(&packet),"Capture endpoint disconnected");if(!packet)break;
            lastPacket=std::chrono::steady_clock::now();
            BYTE* bytes=nullptr;UINT32 frames=0;DWORD status=0;
            check(capture->GetBuffer(&bytes,&frames,&status,nullptr,nullptr),"Could not read desktop audio");
            if(!allowed){
                check(capture->ReleaseBuffer(frames),"Capture endpoint disconnected");
                continue;
            }
            if(status&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY){sink.discontinuity();convert=Converter(format->nSamplesPerSec);}
            const bool silent=(status&AUDCLNT_BUFFERFLAGS_SILENT)!=0;
            for(UINT32 i=0;i<frames;++i){
                const auto* frame=silent?nullptr:bytes+i*stride;
                const float left=frame?read(frame):0.f;
                const float right=frame&&channels>1?read(frame+bits/8):left;
                convert.sample(left,right,sink);
            }
            check(capture->ReleaseBuffer(frames),"Capture endpoint disconnected");
        }
        if(std::chrono::steady_clock::now()-lastPacket>=1s)sink.idle();
    }
    client->Stop();
}

void outputAudio(Engine& engine,const std::wstring& id,std::stop_token stop){
    auto output=endpoint(id);
    ComPtr<IAudioClient> client;check(output->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"Cannot activate the playback device");
    Event event;auto format=stereoFormat();
    constexpr DWORD flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    // The absence of CROSSPROCESS keeps this session scoped to the renderer
    // process while the stable identifier preserves its volume mixer setting.
    constexpr GUID session{0x71e75f8a,0x9621,0x4058,{0xb6,0x9e,0x8e,0xb2,0x2a,0x2c,0x63,0x7f}};
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,400'000,0,&format,&session),"This output cannot play the stereo stream");
    check(client->SetEventHandle(event.handle),"Cannot monitor playback timing");
    UINT32 capacity=0;check(client->GetBufferSize(&capacity),"Cannot read audio buffer size");
    ComPtr<IAudioRenderClient> render;check(client->GetService(IID_PPV_ARGS(&render)),"Cannot open the playback stream");
    DWORD task=0;const HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Audio",&task);
    struct Restore {HANDLE value;~Restore(){if(value)AvRevertMmThreadCharacteristics(value);}} restore{mmcss};
    std::array<float,BLOCK*2> scratch{};unsigned at=BLOCK;
    auto fill=[&](UINT32 frames){
        if(!frames)return;
        BYTE* bytes=nullptr;check(render->GetBuffer(frames,&bytes),"Playback device disconnected");
        auto* samples=reinterpret_cast<float*>(bytes);
        for(UINT32 i=0;i<frames;){
            if(at==BLOCK){engine.render(scratch.data(),true);at=0;}
            const auto n=std::min<unsigned>(frames-i,BLOCK-at);
            memcpy(samples+i*2,scratch.data()+at*2,n*8);at+=n;i+=n;
        }
        check(render->ReleaseBuffer(frames,0),"Playback device disconnected");
    };
    // Let the endpoint set the render quantum. A fixed 1,152-frame target ran
    // dry on outputs with a larger servicing period.
    BYTE* initial=nullptr;
    check(render->GetBuffer(capacity,&initial),"Cannot prime the playback buffer");
    check(render->ReleaseBuffer(capacity,AUDCLNT_BUFFERFLAGS_SILENT),"Cannot prime the playback buffer");
    check(client->Start(),"Cannot start audio output");
    while(!stop.stop_requested()) {
        if(WaitForSingleObject(event.handle,50)==WAIT_FAILED)throw std::runtime_error("Audio output event failed");
        if(stop.stop_requested())break;
        UINT32 padding=0;check(client->GetCurrentPadding(&padding),"Playback device disconnected");
        if(padding<capacity)fill(capacity-padding);
    }
    client->Stop();
}

enum Control { Add=100, Play, Stop, DeleteSource, Save, Load, Capture, Reset, MuteSource,
    Sources, Angle, Metres, Room, QueueAdd, QueueDelete, Recents, QueueList, EditName, PathAdd, PathToggle, PathClear, SpatialToggle,
    OpenWindow, Quit };
constexpr UINT TrayEvent=WM_APP+3;
class Desktop {
    HWND win;
    std::unique_ptr<Engine> engine;
    std::array<SourceEntry,SOURCES> sources{};
    std::array<std::jthread,SOURCES> workers{};
    std::jthread renderer;
    std::array<bool,SOURCES> active{};
    std::array<int64_t,SOURCES> resumeAt{};
    std::vector<std::wstring> recent;
    std::deque<std::wstring> queue;
    std::wstring routeId;
    std::wstring status=L"Idle · battery saving";
    float room=.24f;
    int selected=0;
    bool running=false, live=false, spatialOn=true, grabbing=false;
    HFONT body=nullptr,heading=nullptr;
    HBRUSH background=CreateSolidBrush(Background),panel=CreateSolidBrush(PanelColor);
    NOTIFYICONDATAW trayData{};
    bool trayReady=false;
    UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    RECT stage{};
public:
    explicit Desktop(HWND hwnd):win(hwnd){}
    ~Desktop(){closeRenderer();for(int i=0;i<SOURCES;++i)stopSlot(i);Shell_NotifyIconW(NIM_DELETE,&trayData);DeleteObject(background);DeleteObject(panel);DeleteObject(body);DeleteObject(heading);}
    HWND control(int id) const{return GetDlgItem(win,id);}
    bool restoreTray(UINT message){
        if(!taskbarCreated||message!=taskbarCreated)return false;
        trayReady=Shell_NotifyIconW(NIM_ADD,&trayData)!=FALSE;
        if(!trayReady)ShowWindow(win,SW_RESTORE);
        return true;
    }
    void closeWindow(){if(trayReady)ShowWindow(win,SW_HIDE);else DestroyWindow(win);}
    std::wstring label(const SourceEntry& src) const {
        if(!src.name.empty())return src.name;
        if(src.pcAudio())return L"This PC audio";
        if(src.external())return L"Routed audio";
        if(src.file.empty())return L"Empty slot";
        return std::filesystem::path(src.file).stem().wstring();
    }
    void create(){
        body=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        heading=CreateFontW(-27,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto child=[&](const wchar_t* cls,const wchar_t* text,int id,DWORD style){
            HWND item=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,0,0,win,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
            SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(body),TRUE);return item;
        };
        auto button=[&](const wchar_t* text,int id){child(L"BUTTON",text,id,BS_OWNERDRAW);};
        button(L"Add music",Add);button(L"Play",Play);button(L"Stop",Stop);button(L"Save space",Save);button(L"Open space",Load);
        button(L"Device audio",Capture);button(L"Recenter",Reset);button(L"Mute",MuteSource);button(L"Delete",DeleteSource);button(L"Rename",EditName);
        button(L"Queue file",QueueAdd);button(L"Remove",QueueDelete);button(L"Add point",PathAdd);button(L"Follow path",PathToggle);
        button(L"Clear path",PathClear);button(L"Spatial on",SpatialToggle);
        child(L"LISTBOX",L"",Sources,LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL);
        child(L"LISTBOX",L"",Recents,LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL);
        child(L"LISTBOX",L"",QueueList,LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL);
        auto slider=[&](int id,int max){auto item=child(TRACKBAR_CLASSW,L"",id,TBS_NOTICKS);SendMessageW(item,TBM_SETRANGE,TRUE,MAKELPARAM(0,max));};
        slider(Angle,360);slider(Metres,77);
        auto list=child(L"COMBOBOX",L"",Room,CBS_DROPDOWNLIST);
        for(auto name:{L"Dry",L"Studio",L"Open room"})SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        SendMessageW(list,CB_SETCURSEL,1,0);
        trayData.cbSize=sizeof(trayData);trayData.hWnd=win;trayData.uID=1;trayData.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
        trayData.uCallbackMessage=TrayEvent;trayData.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
        wcscpy_s(trayData.szTip,L"Spatial · idle");trayReady=Shell_NotifyIconW(NIM_ADD,&trayData)!=FALSE;
        refresh();
    }
    void layout(int w,int h){
        const int split=std::max(450,w-330),right=split+16,width=w-right-18;
        stage={20,100,split-10,std::max(360,h-195)};
        auto move=[&](int id,int x,int y,int ww,int hh){MoveWindow(control(id),x,y,ww,hh,TRUE);};
        int x=20;
        for(int id:{Add,Play,Stop,Save,Load,Capture,SpatialToggle}){const int bw=id==Capture?108:91;move(id,x,56,bw,32);x+=bw+6;}
        move(Sources,right,129,width,113);
        const int cell=std::max(48,(width-18)/4);
        int pos=right;for(int id:{EditName,MuteSource,Reset,DeleteSource}){move(id,pos,251,cell,28);pos+=cell+6;}
        move(Angle,right,310,width,24);move(Metres,right,365,width,24);
        move(Room,right,411,width,130);
        const int pcell=(width-12)/3;
        move(PathAdd,right,458,pcell,28);move(PathToggle,right+pcell+6,458,pcell,28);move(PathClear,right+(pcell+6)*2,458,pcell,28);
        move(Recents,20,h-142,(split-52)/2,98);move(QueueList,32+(split-52)/2,h-142,(split-52)/2,98);
        move(QueueAdd,right,h-135,110,30);move(QueueDelete,right+118,h-135,90,30);
        InvalidateRect(win,nullptr,TRUE);
    }
    void paint(HDC dc){
        RECT bounds;GetClientRect(win,&bounds);FillRect(dc,&bounds,background);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Ink);SelectObject(dc,heading);
        const std::wstring title=L"Spatial";TextOutW(dc,20,15,title.c_str(),static_cast<int>(title.size()));
        SelectObject(dc,body);SetTextColor(dc,Soft);
        text(dc,142,23,status);text(dc,stage.right+25,104,L"Multi-hop · up to 4 sources");
        const auto& chosen=sources[selected];
        text(dc,stage.right+25,289,L"Direction   "+std::to_wstring(static_cast<int>(chosen.angle))+L"°");
        wchar_t distance[32];swprintf_s(distance,L"Distance   %.1f m",chosen.distance);text(dc,stage.right+25,344,distance);
        text(dc,stage.right+25,391,L"Room");
        text(dc,stage.right+25,436,L"Movement path   ·   "+std::to_wstring(chosen.path.size())+L" points");
        text(dc,20,bounds.bottom-166,L"Recent tracks · double-click to play");
        text(dc,32+(stage.right-42)/2,bounds.bottom-166,L"Queue");
        text(dc,20,bounds.bottom-32,L"Drag a source to move it. Right-click a source for actions.");
        FillRect(dc,&stage,panel);drawStage(dc);
    }
    void ownerDraw(DRAWITEMSTRUCT* item){
        if(item->CtlType!=ODT_BUTTON)return;
        const auto dis=(item->itemState&ODS_DISABLED)!=0;
        HBRUSH bg=CreateSolidBrush((item->itemState&ODS_SELECTED)?RGB(65,67,78):PanelColor);
        FillRect(item->hDC,&item->rcItem,bg);DeleteObject(bg);
        SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,dis?RGB(111,113,124):Ink);SelectObject(item->hDC,body);
        wchar_t label[80]{};GetWindowTextW(item->hwndItem,label,80);
        DrawTextW(item->hDC,label,-1,&item->rcItem,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
        if(item->itemState&ODS_FOCUS)DrawFocusRect(item->hDC,&item->rcItem);
    }
    HBRUSH color(HDC dc){SetBkColor(dc,PanelColor);SetTextColor(dc,Ink);return panel;}
    void menu(unsigned action){
        switch(action){
            case OpenWindow: ShowWindow(win,SW_RESTORE);SetForegroundWindow(win);break;
            case Quit: DestroyWindow(win);break;
            case Add: if(auto file=pick(false,false);!file.empty())addFile(file,freeSlot());break;
            case QueueAdd: if(auto file=pick(false,false);!file.empty()){queue.push_back(file);refresh();}break;
            case QueueDelete: {const auto i=static_cast<int>(SendMessageW(control(QueueList),LB_GETCURSEL,0,0));if(i>=0&&i<static_cast<int>(queue.size())){queue.erase(queue.begin()+i);refresh();}break;}
            case Play: if(running)pause();else play();break;
            case Stop: stop();break;
            case Save: if(auto file=pick(true,true);!file.empty())save(file);break;
            case Load: if(auto file=pick(false,true);!file.empty())load(file);break;
            case Capture: chooseCapture();break;
            case DeleteSource: clearSelected();break;
            case Reset: sources[selected].angle=0;sources[selected].distance=1.5f;sources[selected].follow=false;refresh();break;
            case MuteSource: sources[selected].muted=!sources[selected].muted;refresh();break;
            case EditName: rename();break;
            case SpatialToggle:
                spatialOn=!spatialOn;
                controls();
                if(hasPcAudio()&&live&&spatialOn)startRenderer();
                timer();refresh();break;
            case PathAdd: {
                auto& src=sources[selected];if(src.exists()&&src.path.size()<8){
                    const auto position=sourcePosition(selected);
                    const float seconds=src.path.empty()?0.f:src.path.back().time+5.f;
                    src.path.push_back({seconds,std::remainder(position.azimuth,360.f),0.f,position.distance});
                    refresh();
                }break;
            }
            case PathToggle: {
                auto& source=sources[selected];
                if(source.path.size()<2)break;
                if(source.follow)holdPath(selected);
                else {source.pathOrigin=pathClock(selected);source.follow=true;}
                refresh();break;
            }
            case PathClear: holdPath(selected);sources[selected].path.clear();refresh();break;
        }
    }
    void selection(int id,int code){
        if(id==Sources&&code==LBN_SELCHANGE) {
            const auto index=static_cast<int>(SendMessageW(control(Sources),LB_GETCURSEL,0,0));
            if(index>=0){selected=index;refresh();}
        } else if(id==Recents&&code==LBN_DBLCLK) {
            const auto index=static_cast<int>(SendMessageW(control(Recents),LB_GETCURSEL,0,0));
            if(index>=0&&index<static_cast<int>(recent.size()))addFile(recent[index],primarySlot());
        } else if(id==Room&&code==CBN_SELCHANGE) {
            const auto index=static_cast<int>(SendMessageW(control(Room),CB_GETCURSEL,0,0));
            if(index>=0&&index<=2){room=std::array<float,3>{0,.24f,.68f}[index];controls();}
        }
    }
    void slider(HWND item){
        if(!sources[selected].exists())return;
        holdPath(selected);
        auto& src=sources[selected];
        const int value=static_cast<int>(SendMessageW(item,TBM_GETPOS,0,0));
        if(item==control(Angle)){const int raw=value-180,detent=static_cast<int>(std::lround(raw/45.f))*45;src.angle=static_cast<float>(std::abs(raw-detent)<=4?detent:raw);}
        if(item==control(Metres))src.distance=(value+3)/10.f;
        controls();InvalidateRect(win,nullptr,FALSE);
    }
    void onClick(int x,int y,bool right){
        POINT cursor{x,y};if(!PtInRect(&stage,cursor))return;
        for(int i=0;i<SOURCES;++i){
            if(!sources[i].exists())continue;
            const auto position=sourcePosition(i);
            const auto p=point(position.azimuth,position.distance);
            if(std::hypot(static_cast<float>(x-p.x),static_cast<float>(y-p.y))>23)continue;
            selected=i;refresh();
            if(right){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,EditName,L"Rename");AppendMenuW(menu,MF_STRING,Reset,L"Recenter");AppendMenuW(menu,MF_STRING,DeleteSource,L"Delete source");ClientToScreen(win,&cursor);TrackPopupMenu(menu,TPM_RIGHTBUTTON,cursor.x,cursor.y,0,win,nullptr);DestroyMenu(menu);}
            else {holdPath(i);grabbing=true;SetCapture(win);refresh();}
            return;
        }
    }
    void onMove(int x,int y){
        if(!grabbing||!sources[selected].exists())return;
        const auto center=stageCenter();const float r=radius(),dx=static_cast<float>(x-center.x),dy=static_cast<float>(center.y-y);
        auto& src=sources[selected];src.angle=std::atan2(dx,dy)*180/PI;src.distance=std::clamp((dx*dx+dy*dy)/(r*r)*8,.3f,8.f);src.follow=false;
        SendMessageW(control(Angle),TBM_SETPOS,TRUE,static_cast<int>(src.angle)+180);
        SendMessageW(control(Metres),TBM_SETPOS,TRUE,static_cast<int>(std::lround(src.distance*10))-3);
        controls();InvalidateRect(win,&stage,FALSE);
    }
    void onUp(){if(grabbing){grabbing=false;ReleaseCapture();}}
    void timer(){
        if(!engine)return;
        bool local=false;
        for(int i=0;i<SOURCES;++i){
            if(!active[i]||sources[i].external())continue;
            auto& stream=engine->streams[i];
            if(stream.ended.load()==stream.generation.load()&&stream.read.load()==stream.write.load()){
                active[i]=false;resumeAt[i]=0;
                if(i==primarySlot()&&!queue.empty()){const auto file=queue.front();queue.pop_front();addFile(file,i);return;}
            } else local=true;
        }
        if(!local&&!renderLive()&&renderer.joinable()){
            closeRenderer();
            running=std::any_of(active.begin(),active.end(),[](bool value){return value;});
            status=waitingStatus();refresh();
        }
        if(renderer.joinable()){if(IsWindowVisible(win))InvalidateRect(win,&stage,FALSE);}
        else KillTimer(win,1);
    }
    void captureSignal(int slot,bool audible){
        if(slot<0||slot>=SOURCES||!active[slot]||!sources[slot].external())return;
        live=audible;controls();
        if(renderLive())startRenderer();
        else if(std::none_of(sources.begin(),sources.end(),[&](const auto& src){return src.exists()&&!src.external()&&active[&src-sources.data()];})){
            closeRenderer();status=waitingStatus();refresh();
        }
    }
    void error(WPARAM slot,std::wstring error){
        if(slot>=SOURCES)stop();else {active[slot]=false;if(sources[slot].external())live=false;controls();}
        status=std::move(error);refresh();
    }
    void tray(){
        POINT p;GetCursorPos(&p);SetForegroundWindow(win);
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,OpenWindow,L"Open Spatial");
        AppendMenuW(menu,MF_STRING,Play,running?L"Pause":L"Play");
        AppendMenuW(menu,MF_STRING,Stop,L"Stop Spatial");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,Quit,L"Quit Spatial");
        TrackPopupMenu(menu,TPM_RIGHTBUTTON,p.x,p.y,0,win,nullptr);DestroyMenu(menu);PostMessageW(win,WM_NULL,0,0);
    }
private:
    static void text(HDC dc,int x,int y,const std::wstring& value){TextOutW(dc,x,y,value.c_str(),static_cast<int>(value.size()));}
    POINT stageCenter() const {return {stage.left+(stage.right-stage.left)/2,stage.top+(stage.bottom-stage.top)/2};}
    float radius() const {return std::max(1.f,std::min(stage.right-stage.left,stage.bottom-stage.top)*.40f);}
    POINT point(float angle,float distance)const{
        const auto c=stageCenter();const float r=std::sqrt(std::clamp(distance,.3f,8.f)/8.f)*radius(),a=angle*PI/180;
        return {c.x+static_cast<LONG>(std::sin(a)*r),c.y-static_cast<LONG>(std::cos(a)*r)};
    }
    float pathClock(int slot)const{
        if(!engine)return 0.f;
        const auto frames=sources[slot].external()?engine->streams[slot].livePathFrames.load():engine->clock.load();
        return static_cast<float>(frames)/RATE;
    }
    PathPoint sourcePosition(int slot)const{
        const auto& source=sources[slot];
        if(!source.follow||source.path.size()<2)return {0.f,source.angle,0.f,source.distance};
        return pathAt(source.path.data(),static_cast<int>(source.path.size()),
            std::max(0.f,pathClock(slot)-source.pathOrigin),true,source.path.back().time,source.external());
    }
    void holdPath(int slot){
        auto& source=sources[slot];
        if(!source.follow)return;
        const auto position=sourcePosition(slot);
        source.angle=std::remainder(position.azimuth,360.f);
        source.distance=position.distance;
        source.follow=false;
    }
    void drawStage(HDC dc){
        const auto c=stageCenter();const int r=static_cast<int>(radius());
        const auto oldPen=SelectObject(dc,GetStockObject(DC_PEN));const auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc,RGB(68,71,83));
        for(int n:{1,2,4,8}){const int rr=static_cast<int>(r*std::sqrt(n/8.f));Ellipse(dc,c.x-rr,c.y-rr,c.x+rr,c.y+rr);}
        MoveToEx(dc,c.x-r,c.y,nullptr);LineTo(dc,c.x+r,c.y);MoveToEx(dc,c.x,c.y-r,nullptr);LineTo(dc,c.x,c.y+r);
        text(dc,c.x-23,c.y-r-24,L"FRONT");
        SelectObject(dc,GetStockObject(DC_BRUSH));SetDCBrushColor(dc,Ink);Ellipse(dc,c.x-6,c.y-6,c.x+6,c.y+6);
        for(int i=0;i<SOURCES;++i){
            const auto& src=sources[i];if(!src.exists())continue;
            const auto position=sourcePosition(i);
            const auto p=point(position.azimuth,position.distance);
            SetDCPenColor(dc,RGB(94,97,110));MoveToEx(dc,c.x,c.y,nullptr);LineTo(dc,p.x,p.y);
            if(src.path.size()>1){for(size_t j=1;j<src.path.size();++j){const auto a=point(src.path[j-1].azimuth,src.path[j-1].distance),b=point(src.path[j].azimuth,src.path[j].distance);MoveToEx(dc,a.x,a.y,nullptr);LineTo(dc,b.x,b.y);}}
            SetDCPenColor(dc,i==selected?Ink:Background);SetDCBrushColor(dc,src.muted?RGB(99,101,110):Colors[i]);
            Ellipse(dc,p.x-17,p.y-17,p.x+17,p.y+17);SetTextColor(dc,Background);text(dc,p.x-5,p.y-11,std::to_wstring(i+1));SetTextColor(dc,Soft);
        }
        SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
    }
    void refresh(){
        auto writeList=[&](HWND h,const auto& items,auto labeler){
            SendMessageW(h,WM_SETREDRAW,FALSE,0);SendMessageW(h,LB_RESETCONTENT,0,0);
            for(const auto& item:items){const auto row=labeler(item);SendMessageW(h,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str()));}
            SendMessageW(h,WM_SETREDRAW,TRUE,0);InvalidateRect(h,nullptr,TRUE);
        };
        for(int i=0;i<SOURCES;++i){const auto row=std::to_wstring(i+1)+L"   "+label(sources[i]);if(i==0)SendMessageW(control(Sources),LB_RESETCONTENT,0,0);SendMessageW(control(Sources),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str()));}
        SendMessageW(control(Sources),LB_SETCURSEL,selected,0);
        const auto listLabel=[](const std::wstring& p){return std::filesystem::path(p).filename().wstring();};
        writeList(control(Recents),recent,listLabel);writeList(control(QueueList),queue,listLabel);
        const auto& src=sources[selected];
        SendMessageW(control(Angle),TBM_SETPOS,TRUE,static_cast<int>(src.angle)+180);
        SendMessageW(control(Metres),TBM_SETPOS,TRUE,static_cast<int>(std::lround(src.distance*10))-3);
        SetWindowTextW(control(MuteSource),src.muted?L"Unmute":L"Mute");SetWindowTextW(control(PathToggle),src.follow?L"Hold path":L"Follow path");
        SetWindowTextW(control(Play),running?L"Pause":L"Play");SetWindowTextW(control(SpatialToggle),spatialOn?L"Spatial on":L"Spatial off");
        for(int id:{MuteSource,DeleteSource,EditName})EnableWindow(control(id),src.exists());
        for(int id:{Reset,Angle,Metres})EnableWindow(control(id),src.exists());
        EnableWindow(control(PathAdd),src.exists()&&src.path.size()<8);
        EnableWindow(control(PathToggle),src.path.size()>1);EnableWindow(control(PathClear),!src.path.empty());
        EnableWindow(control(Add),freeSlot()>=0);
        const auto tip=L"Spatial · "+status;
        wcsncpy_s(trayData.szTip,tip.c_str(),_TRUNCATE);if(trayReady)Shell_NotifyIconW(NIM_MODIFY,&trayData);
        controls();InvalidateRect(win,nullptr,TRUE);
    }
    void controls(){
        if(!engine)return;
        float p[PARAMS]{};p[0]=spatialOn?1.f:0.f;p[2]=-3.f;p[11]=room;p[16]=1.f;p[17]=1.f;p[18]=1.f;
        for(int i=0;i<SOURCES;++i){
            const auto& src=sources[i];const int o=128+i*64;
            p[o]=active[i]&&(!src.external()||renderLive())?1.f:0.f;p[o+1]=src.angle;
            p[o+3]=src.distance;
            p[o+5]=src.muted?1.f:0.f;p[o+7]=src.pcAudio()?100.f:60.f;
            p[o+12]=src.external()?1.f:0.f;
            p[o+13]=src.pcAudio()?2.f:0.f;
            p[o+14]=src.pathOrigin;
            p[o+8]=src.follow?1.f:0.f;p[o+11]=1.f;p[o+16]=static_cast<float>(src.path.size());
            if(!src.path.empty())p[o+10]=src.path.back().time;
            for(size_t j=0;j<src.path.size();++j){const auto& k=src.path[j];p[o+17+j*4]=k.time;p[o+18+j*4]=k.azimuth;p[o+20+j*4]=k.distance;}
        }
        sendControls(*engine,p);
    }
    std::wstring pick(bool save,bool scene){
        wchar_t path[32768]{};OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=win;ofn.lpstrFile=path;ofn.nMaxFile=32768;
        ofn.lpstrFilter=scene?L"Spatial scene\0*.spatial\0\0":L"Audio files\0*.wav;*.mp3;*.m4a;*.aac;*.flac;*.wma\0All files\0*.*\0\0";
        ofn.lpstrDefExt=scene?L"spatial":nullptr;ofn.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        return (save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))?path:L"";
    }
    int freeSlot()const{for(int i=0;i<SOURCES;++i)if(!sources[i].exists())return i;return -1;}
    int primarySlot()const{for(int i=0;i<SOURCES;++i)if(sources[i].exists()&&!sources[i].external())return i;return freeSlot();}
    std::optional<int> externalSlot()const{for(int i=0;i<SOURCES;++i)if(sources[i].external())return i;return {};}
    bool hasPcAudio()const{const auto slot=externalSlot();return slot&&sources[*slot].pcAudio();}
    bool renderLive()const{return live&&(!hasPcAudio()||spatialOn);}
    const wchar_t* waitingStatus()const{
        if(!running)return L"Idle · battery saving";
        if(hasPcAudio())return spatialOn?L"PC audio ready · waiting for sound":L"PC spatial effect off";
        return L"Waiting for routed audio";
    }
    void ensure(){if(!engine){engine=std::make_unique<Engine>();if(!engine->valid){engine.reset();throw std::runtime_error("Steam Audio could not initialize");}}}
    void postError(int slot,const char* reason){auto msg=std::make_unique<std::wstring>(widen(reason));if(PostMessageW(win,AudioError,slot,reinterpret_cast<LPARAM>(msg.get())))msg.release();}
    void startSlot(int i){
        const auto source=sources[i];if(!source.exists())return;
        if(workers[i].joinable()){workers[i].request_stop();workers[i].join();}
        const auto position=resumeAt[i];const int generation=beginStream(*engine,i,position);
        active[i]=true;controls();
        workers[i]=std::jthread([this,source,i,generation,position](std::stop_token token){
            ComScope com;
            PcmBlocks sink(win,*engine,i,generation,token,source.external(),position);
            try { if(source.external())loopback(source.endpoint,token,sink);else readFile(source.file,token,sink,position); }
            catch(const std::exception& e){if(!token.stop_requested())postError(i,e.what());}
            if(!token.stop_requested())engine->streams[i].ended.store(generation);
        });
    }
    void startRenderer(){
        if(renderer.joinable())return;
        const auto destination=routeId;
        renderer=std::jthread([this,destination](std::stop_token token){
            ComScope com;
            try {outputAudio(*engine,destination,token);}catch(const std::exception& e){if(!token.stop_requested())postError(SOURCES,e.what());}
        });
        SetTimer(win,1,150,nullptr);status=L"Live · spatial playback";refresh();
    }
    void closeRenderer(){if(renderer.joinable()){renderer.request_stop();renderer.join();}}
    void addFile(const std::wstring& file,int i){
        if(i<0||sources[i].external()){status=L"All four slots are in use";refresh();return;}
        stopSlot(i);selected=i;sources[i].file=file;sources[i].name.clear();resumeAt[i]=0;
        recent.erase(std::remove(recent.begin(),recent.end(),file),recent.end());recent.insert(recent.begin(),file);if(recent.size()>30)recent.pop_back();
        try {ensure();startSlot(i);running=true;startRenderer();}catch(const std::exception& e){status=widen(e.what());}
        refresh();
    }
    void play(){
        if(std::none_of(sources.begin(),sources.end(),[](const auto& x){return x.exists();})&&!queue.empty()){const auto next=queue.front();queue.pop_front();addFile(next,0);return;}
        if(std::none_of(sources.begin(),sources.end(),[](const auto& x){return x.exists();}))return;
        try {
            if(const auto slot=externalSlot();slot&&!sources[*slot].pcAudio()){
                const auto outputId=endpointId(endpoint(L"").Get());
                if(outputId==sources[*slot].endpoint)throw std::runtime_error("Choose a different Windows output before playing this routed space");
                routeId=outputId;
            }else routeId.clear();
            ensure();running=true;bool local=false;
            for(int i=0;i<SOURCES;++i)if(sources[i].exists()){
                if(!active[i])startSlot(i);
                local|=!sources[i].external();
            }
            if(local||renderLive())startRenderer();else status=waitingStatus();
        } catch(const std::exception& e){status=widen(e.what());}
        refresh();
    }
    void stopSlot(int i){
        active[i]=false;if(workers[i].joinable()){workers[i].request_stop();workers[i].join();}
        if(engine)beginStream(*engine,i,0);
    }
    void pause(){
        closeRenderer();
        for(int i=0;i<SOURCES;++i){if(engine&&active[i])resumeAt[i]=engine->streams[i].position.load();stopSlot(i);}
        live=false;running=false;KillTimer(win,1);status=L"Idle · battery saving";refresh();
    }
    void stop(){
        pause();resumeAt.fill(0);
        for(int slot=0;slot<SOURCES;++slot)sources[slot].pathOrigin=pathClock(slot);
        refresh();
    }
    void clearSelected(){stopSlot(selected);if(sources[selected].external())live=false;sources[selected]={};resumeAt[selected]=0;timer();refresh();}
    void chooseCapture(){
        if(const auto slot=externalSlot()){selected=*slot;clearSelected();status=L"Device audio disconnected";refresh();return;}
        const int slot=freeSlot();if(slot<0){status=L"Free one source slot first";refresh();return;}
        try {
            auto enumerator=devices();ComPtr<IMMDeviceCollection> collection;
            check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection),"Cannot find audio endpoints");
            const auto dest=endpoint(L"");const auto destId=endpointId(dest.Get());
            UINT count=0;collection->GetCount(&count);std::vector<std::wstring> choices{PcAudio};
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING,1,L"This PC audio");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            for(UINT i=0;i<count;++i){ComPtr<IMMDevice> item;if(FAILED(collection->Item(i,&item)))continue;const auto id=endpointId(item.Get());if(id==destId)continue;
                choices.push_back(id);const auto label=L"Route from "+endpointName(item.Get());AppendMenuW(menu,MF_STRING,static_cast<UINT>(choices.size()),label.c_str());}
            POINT p;GetCursorPos(&p);const auto choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,win,nullptr);DestroyMenu(menu);
            if(choice==0)return;
            const bool allPc=choice==1;
            const wchar_t* message=allPc
                ? L"Spatial will move the stereo ambience of eligible PC sound. Add points in Movement path to automate direction and distance. An independent routed input spatializes the entire signal."
                : L"Use Windows Volume mixer to send the source app to the selected endpoint. Spatial plays through the current default output.";
            if(MessageBoxW(win,message,L"Connect device audio",MB_OKCANCEL)!=IDOK)return;
            closeRenderer();routeId=allPc?L"":destId;sources[slot].endpoint=choices[choice-1];selected=slot;
            if(allPc){spatialOn=true;sources[slot].distance=1.f;}
            play();
        }catch(const std::exception& e){status=widen(e.what());refresh();}
    }
    void rename(){
        auto& src=sources[selected];if(!src.exists())return;
        // Use an inline edit field which commits on Enter and cancels on Escape or focus loss.
        RECT field;GetWindowRect(control(Sources),&field);MapWindowPoints(HWND_DESKTOP,win,reinterpret_cast<POINT*>(&field),2);
        HWND edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",label(src).c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,field.left,field.top,field.right-field.left,29,win,nullptr,nullptr,nullptr);
        SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(body),TRUE);SendMessageW(edit,EM_SETLIMITTEXT,60,0);SendMessageW(edit,EM_SETSEL,0,-1);
        SetWindowSubclass(edit,[](HWND box,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR ref)->LRESULT {
            auto* ui=reinterpret_cast<Desktop*>(ref);
            if(msg==WM_KEYDOWN&&(w==VK_RETURN||w==VK_ESCAPE)){
                if(w==VK_RETURN){wchar_t name[61]{};GetWindowTextW(box,name,61);ui->sources[ui->selected].name=name;}
                DestroyWindow(box);ui->refresh();return 0;
            }
            if(msg==WM_KILLFOCUS){DestroyWindow(box);return 0;}
            return DefSubclassProc(box,msg,w,l);
        },1,reinterpret_cast<DWORD_PTR>(this));SetFocus(edit);
    }
    void save(const std::wstring& file){
        try {
            const auto target=std::filesystem::path(file);auto tmp=target;tmp+=L".tmp";
            std::wofstream f(tmp);f.imbue(std::locale(".UTF-8"));f<<L"Spatial 2\n"<<room<<L' '<<spatialOn<<L'\n';
            for(const auto& src:sources){
                f<<std::quoted(src.file)<<L' '<<std::quoted(src.name)<<L' '<<std::quoted(src.endpoint)<<L' '
                  <<src.angle<<L' '<<src.distance<<L' '<<src.muted<<L' '<<src.follow<<L' '<<src.path.size()<<L'\n';
                for(const auto& k:src.path)f<<k.time<<L' '<<k.azimuth<<L' '<<k.distance<<L'\n';
            }
            f.close();if(!f||!MoveFileExW(tmp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not save space");
            status=L"Space saved";
        }catch(const std::exception& e){status=widen(e.what());}
        refresh();
    }
    void load(const std::wstring& file){
        try {
            std::wifstream f{std::filesystem::path(file)};f.imbue(std::locale(".UTF-8"));std::wstring header;
            std::getline(f,header);if(header!=L"Spatial 1"&&header!=L"Spatial 2")throw std::runtime_error("Invalid Spatial scene");
            std::array<SourceEntry,SOURCES> parsed{};float newRoom=0;bool newOn=false;f>>newRoom>>newOn;
            if(!std::isfinite(newRoom)||newRoom<0||newRoom>1)throw std::runtime_error("Invalid room preset");
            bool foundExternal=false;
            for(auto& src:parsed){
                f>>std::quoted(src.file)>>std::quoted(src.name);
                if(header==L"Spatial 2")f>>std::quoted(src.endpoint);
                if(src.external()){
                    if(foundExternal||!src.file.empty())throw std::runtime_error("A space supports one external audio source");
                    foundExternal=true;
                }
                size_t count=0;f>>src.angle>>src.distance>>src.muted>>src.follow>>count;
                if(!f||count>8||!std::isfinite(src.angle)||!std::isfinite(src.distance)||src.angle < -180||src.angle>180||src.distance<.3||src.distance>8)throw std::runtime_error("Invalid source in scene");
                float previous=-1;
                for(size_t i=0;i<count;++i){PathPoint k{};f>>k.time>>k.azimuth>>k.distance;if(!f||!(k.time>previous)||k.time>3600||!std::isfinite(k.azimuth)||!(k.distance>=.3f&&k.distance<=8.f))throw std::runtime_error("Invalid movement path");src.path.push_back(k);previous=k.time;}
            }
            stop();sources=std::move(parsed);
            for(int slot=0;slot<SOURCES;++slot)sources[slot].pathOrigin=pathClock(slot);
            routeId.clear();room=newRoom;spatialOn=newOn;SendMessageW(control(Room),CB_SETCURSEL,room<.1f?0:room<.4f?1:2,0);status=L"Space loaded · press Play";
        }catch(const std::exception& e){status=widen(e.what());}
        refresh();
    }
};
LRESULT CALLBACK windowProc(HWND win,UINT msg,WPARAM w,LPARAM l){
    auto* ui=reinterpret_cast<Desktop*>(GetWindowLongPtrW(win,GWLP_USERDATA));
    if(msg==WM_CREATE){ui=new Desktop(win);SetWindowLongPtrW(win,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(ui));ui->create();return 0;}
    if(!ui)return DefWindowProcW(win,msg,w,l);
    if(ui->restoreTray(msg))return 0;
    switch(msg){
        case WM_CLOSE:ui->closeWindow();return 0;
        case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={830,690};return 0;
        case WM_SIZE:if(w!=SIZE_MINIMIZED)ui->layout(LOWORD(l),HIWORD(l));return 0;
        case WM_COMMAND:if(LOWORD(w)==Sources||LOWORD(w)==Recents||LOWORD(w)==Room)ui->selection(LOWORD(w),HIWORD(w));else ui->menu(LOWORD(w));return 0;
        case WM_HSCROLL:ui->slider(reinterpret_cast<HWND>(l));return 0;
        case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:ui->onClick(GET_X_LPARAM(l),GET_Y_LPARAM(l),msg==WM_RBUTTONDOWN);return 0;
        case WM_MOUSEMOVE:if(w&MK_LBUTTON)ui->onMove(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:ui->onUp();return 0;
        case WM_DRAWITEM:ui->ownerDraw(reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
        case WM_CTLCOLORLISTBOX:case WM_CTLCOLORSTATIC:return reinterpret_cast<LRESULT>(ui->color(reinterpret_cast<HDC>(w)));
        case WM_TIMER:ui->timer();return 0;
        case AudioError:{std::unique_ptr<std::wstring> reason(reinterpret_cast<std::wstring*>(l));ui->error(w,*reason);return 0;}
        case InputSignal:ui->captureSignal(static_cast<int>(w),l!=0);return 0;
        case TrayEvent:if(l==WM_RBUTTONUP)ui->tray();if(l==WM_LBUTTONUP){ShowWindow(win,SW_RESTORE);SetForegroundWindow(win);}return 0;
        case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(win,&ps);ui->paint(dc);EndPaint(win,&ps);return 0;}
        case WM_DESTROY:SetWindowLongPtrW(win,GWLP_USERDATA,0);delete ui;PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(win,msg,w,l);
}
}

int verifyDecodedAudio(const std::wstring& path) {
    auto engine=std::make_unique<Engine>();
    if(!engine->valid)return 4;
    float control[PARAMS]{};control[0]=1;control[16]=1;control[17]=1;control[18]=1;
    control[128]=1;control[129]=55;control[131]=1.5f;control[135]=60;
    sendControls(*engine,control);
    const int gen=beginStream(*engine,0,0);
    std::atomic<bool> decoded=false,failed=false;
    std::jthread input([&](std::stop_token token){
        ComScope com;
        try{PcmBlocks sink(nullptr,*engine,0,gen,token,false);readFile(path,token,sink);}catch(...){failed=true;}
        engine->streams[0].ended.store(gen);decoded=true;
    });
    std::array<float,BLOCK*2> out{};double energy=0;int blocks=0;
    const auto until=std::chrono::steady_clock::now()+10s;
    while(std::chrono::steady_clock::now()<until){
        auto& stream=engine->streams[0];
        if(stream.read.load()==stream.write.load()){if(decoded)break;std::this_thread::sleep_for(1ms);continue;}
        engine->render(out.data(),true);++blocks;
        for(float v:out){if(!std::isfinite(v)||std::abs(v)>1.f)return 5;energy+=v*v;}
    }
    input.request_stop();input.join();
    return !failed&&decoded&&blocks>10&&energy>1 ? 0:6;
}

int verifyPcLoopback() {
    auto engine=std::make_unique<Engine>();
    if(!engine->valid)return 7;
    const int generation=beginStream(*engine,0,0);
    std::atomic<bool> ready=false,failed=false;
    std::jthread worker([&](std::stop_token stop){
        ComScope com;
        PcmBlocks sink(nullptr,*engine,0,generation,stop,true);
        try {loopback(PcAudio,stop,sink,&ready);}catch(...){failed=true;}
    });
    const auto until=std::chrono::steady_clock::now()+8s;
    while(!failed&&!engine->streams[0].write.load()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(50ms);
    worker.request_stop();worker.join();
    if(failed||!ready)return 8;
    return engine->streams[0].write.load()>0 ? 0:9;
}

// Exercise the same fixed-field capture/render chain on a real WASAPI output.
// A separate test process provides a short recording.
int verifyPcFieldDecay(bool centerOnly,DWORD testProcess) {
    auto engine=std::make_unique<Engine>();
    if(!engine->valid)return 15;
    float p[PARAMS]{};
    p[0]=1.f;p[2]=-3.f;p[11]=.68f;p[16]=1.f;p[18]=1.f;
    p[128]=1.f;p[131]=1.f;p[140]=1.f;p[141]=1.f;
    sendControls(*engine,p);
    const int generation=beginStream(*engine,0,0);
    std::atomic<bool> ready=false,failed=false;
    std::jthread input([&](std::stop_token stop){
        ComScope com;
        PcmBlocks sink(nullptr,*engine,0,generation,stop,true);
        try{loopback(PcAudio,stop,sink,&ready,true,true,testProcess);}catch(...){failed=true;}
    });
    std::jthread output([&](std::stop_token stop){
        ComScope com;
        try{outputAudio(*engine,L"",stop);}catch(...){failed=true;}
    });
    const auto start=std::chrono::steady_clock::now();
    float peak=0,tail=0;
    while(!failed&&std::chrono::steady_clock::now()-start<7s){
        const float value=engine->meterPeak.load();
        peak=std::max(peak,value);
        if(std::chrono::steady_clock::now()-start>6s)tail=std::max(tail,value);
        std::this_thread::sleep_for(20ms);
    }
    input.request_stop();output.request_stop();input.join();output.join();
    const auto frames=engine->streams[0].position.load();
    std::fprintf(stderr,"WASAPI fixed field: captured=%lld frames, peak=%.6f, tail=%.6f\n",
        static_cast<long long>(frames),peak,tail);
    const bool received=frames>RATE/4;
    const bool valid=centerOnly?received&&peak<.00005f:peak>.0001f&&tail<peak*.03f;
    return ready&&!failed&&valid ? 0:16;
}

int verifyAudioOutput(const std::wstring& path) {
    auto engine=std::make_unique<Engine>();
    if(!engine->valid)return 10;
    float p[PARAMS]{};
    p[2]=-20.f;p[16]=1.f;p[18]=1.f;p[128]=1.f;p[131]=1.f;
    sendControls(*engine,p);
    const int generation=beginStream(*engine,0,0);
    std::atomic<bool> failed=false;
    std::jthread input([&](std::stop_token stop){
        ComScope com;
        PcmBlocks sink(nullptr,*engine,0,generation,stop,false);
        try {readFile(path,stop,sink);}catch(...){failed=true;}
        engine->streams[0].ended.store(generation);
    });
    std::jthread output([&](std::stop_token stop){
        ComScope com;
        try {outputAudio(*engine,L"",stop);}catch(...){failed=true;}
    });
    const auto deadline=std::chrono::steady_clock::now()+6s;
    while(!failed&&std::chrono::steady_clock::now()<deadline) {
        auto& source=engine->streams[0];
        if(source.ended.load()==generation&&source.read.load()==source.write.load())break;
        std::this_thread::sleep_for(50ms);
    }
    input.request_stop();output.request_stop();input.join();output.join();
    return !failed&&engine->streams[0].position.load()>RATE ? 0:11;
}

int verifyOwnOutputIsExcluded(const std::wstring& path) {
    auto engine=std::make_unique<Engine>();
    if(!engine->valid)return 12;
    const int generation=beginStream(*engine,0,0);
    std::atomic<bool> ready=false,failed=false;
    std::jthread capture([&](std::stop_token stop){
        ComScope com;
        PcmBlocks sink(nullptr,*engine,0,generation,stop,true);
        try {loopback(PcAudio,stop,sink,&ready,false);}catch(...){failed=true;}
    });
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!ready&&!failed&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(20ms);
    if(!ready)return 13;
    std::atomic<int> playback=-1;
    std::jthread output([&]{ComScope com;playback=verifyAudioOutput(path);});
    unsigned captured=0,matched=0;
    const auto checkUntil=std::chrono::steady_clock::now()+7s;
    auto& queue=engine->streams[0];
    while(!failed&&std::chrono::steady_clock::now()<checkUntil) {
        auto read=queue.read.load(std::memory_order_relaxed);
        const auto write=queue.write.load(std::memory_order_acquire);
        for(;read!=write;++read) {
            const auto& block=queue.queue[read%QUEUE];
            double sine=0,cosine=0,power=0;
            for(int i=0;i<block.frames;++i) {
                const double s=block.samples[i*2];
                const double phase=2.*PI*997.*i/RATE;
                sine+=s*std::sin(phase);cosine+=s*std::cos(phase);power+=s*s;
            }
            ++captured;
            // Ignore unrelated desktop sounds. The generated fixture is a 997 Hz sine.
            if(power>.000001&&2*(sine*sine+cosine*cosine)>.70*block.frames*power)++matched;
        }
        queue.read.store(read,std::memory_order_release);
        if(playback>=0)break;
        std::this_thread::sleep_for(20ms);
    }
    output.join();
    capture.request_stop();capture.join();
    std::fprintf(stderr,"PC feedback probe: playback=%d, non-silent blocks=%u, fixture-frequency blocks=%u\n",playback.load(),captured,matched);
    return playback==0&&!failed&&matched==0 ? 0:14;
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    ComScope com;
    if(FAILED(MFStartup(MF_VERSION)))return 1;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv&&argc==3&&std::wstring_view(argv[1])==L"--verify-audio") {
        const int result=verifyDecodedAudio(argv[2]);LocalFree(argv);MFShutdown();return result;
    }
    if(argv&&argc==3&&std::wstring_view(argv[1])==L"--verify-output") {
        const int result=verifyAudioOutput(argv[2]);LocalFree(argv);MFShutdown();return result;
    }
    if(argv&&argc==2&&std::wstring_view(argv[1])==L"--verify-pc-loopback") {
        const int result=verifyPcLoopback();LocalFree(argv);MFShutdown();return result;
    }
    if(argv&&(argc==2||argc==3)&&(std::wstring_view(argv[1])==L"--verify-pc-decay"||std::wstring_view(argv[1])==L"--verify-pc-center")) {
        const int result=verifyPcFieldDecay(std::wstring_view(argv[1])==L"--verify-pc-center",argc==3?wcstoul(argv[2],nullptr,10):0);
        LocalFree(argv);MFShutdown();return result;
    }
    if(argv&&argc==3&&std::wstring_view(argv[1])==L"--verify-own-output-excluded") {
        const int result=verifyOwnOutputIsExcluded(argv[2]);LocalFree(argv);MFShutdown();return result;
    }
    if(argv)LocalFree(argv);
    // Two copies would each exclude only their own stream, creating a loop through
    // the other copy. Also detect the window class used by older installed copies.
    HANDLE instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\SpatialDesktop.AudioInstance");
    const bool duplicate=instanceMutex&&GetLastError()==ERROR_ALREADY_EXISTS;
    struct MutexHandle {HANDLE value;~MutexHandle(){if(value)CloseHandle(value);}} instanceGuard{instanceMutex};
    const HWND runningWindow=FindWindowW(L"SpatialDesktop",nullptr);
    if(duplicate||runningWindow) {
        if(runningWindow)PostMessageW(runningWindow,WM_COMMAND,OpenWindow,0);
        MFShutdown();return 0;
    }
    if(!instanceMutex){MFShutdown();return 3;}
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=instance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"SpatialDesktop";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    HWND win=CreateWindowExW(0,wc.lpszClassName,L"Spatial",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1030,805,nullptr,nullptr,instance,nullptr);
    if(!win){MFShutdown();return 2;}
    const BOOL dark=TRUE;DwmSetWindowAttribute(win,20,&dark,sizeof(dark));ShowWindow(win,show);UpdateWindow(win);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(win,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    MFShutdown();return static_cast<int>(message.wParam);
}

