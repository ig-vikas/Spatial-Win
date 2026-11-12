#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace spatial {
constexpr int RATE = 48000, BLOCK = 512, SOURCES = 4, CHANNELS = 8, PARAMS = 384;
constexpr unsigned LIVE_QUEUE_BLOCKS = 10, LIVE_PRIME_BLOCKS = 3;
constexpr float PI = 3.14159265358979323846f;
inline float db(float v) { return std::pow(10.f, v / 20.f); }
inline float clamp(float v, float a, float b) { return std::max(a, std::min(b, v)); }

// Align the dominant ear peaks while keeping the frequency/level cues of each filter.
// A fixed anchor keeps common latency stable across ordinary head and source movement.
struct EarAlignment {
    static constexpr unsigned Capacity = 256, Mask = Capacity - 1, Anchor = 96;
    float history[2][Capacity]{};
    unsigned cursor = 0;
    unsigned tap[2]{};
    bool initialized = false;

    void apply(float (&samples)[2][BLOCK], const float (&delaySeconds)[2]) {
        unsigned peak[2]{};
        for (int c = 0; c < 2; ++c) {
            if (std::isfinite(delaySeconds[c]))
                peak[c] = static_cast<unsigned>(std::lround(clamp(delaySeconds[c] * RATE, 0.f, 4096.f)));
        }
        const unsigned anchor = std::max({Anchor, peak[0], peak[1]});
        const unsigned target[2]{std::min(anchor - peak[0], Mask), std::min(anchor - peak[1], Mask)};
        if (!initialized) { tap[0] = target[0]; tap[1] = target[1]; initialized = true; }
        for (unsigned i = 0; i < BLOCK; ++i) {
            const float fade = std::min(1.f, static_cast<float>(i + 1) / 96.f);
            for (int c = 0; c < 2; ++c) {
                history[c][cursor] = samples[c][i];
                const float previous = history[c][(cursor - tap[c]) & Mask];
                const float next = history[c][(cursor - target[c]) & Mask];
                samples[c][i] = previous + fade * (next - previous);
            }
            cursor = (cursor + 1) & Mask;
        }
        tap[0] = target[0]; tap[1] = target[1];
    }
};

// Two symmetric taps. Both ears get the same arrival times and crossfeed gains.
struct EarlyReflections {
    static constexpr unsigned Capacity = 2048, Mask = Capacity - 1;
    std::array<std::array<float,2>,Capacity> history{};
    unsigned cursor = 0;

    void process(float& left, float& right, float wet) {
        const auto early = history[(cursor - 576) & Mask];
        const auto late = history[(cursor - 1104) & Mask];
        history[cursor] = {left, right};
        cursor = (cursor + 1) & Mask;
        left += wet * (.12f * early[1] + .06f * late[0]);
        right += wet * (.12f * early[0] + .06f * late[1]);
    }
};

inline void stereoOverlay(float& left,float& right,float pan) {
    // The original stream already carries the center. Replaying it from capture
    // introduces a second arrival. Only enhance stereo differences, with both
    // channels calculated from the same sample and no extra room/ear delays.
    const float side=.24f*(left-right)*.5f;
    left=side*(1.f-.50f*pan);
    right=-side*(1.f+.50f*pan);
}
struct Vec3 { float x, y, z; };
inline Vec3 direction(float azimuth, float elevation) {
    float a = azimuth * PI / 180.f, e = elevation * PI / 180.f;
    return {std::sin(a)*std::cos(e), std::sin(e), -std::cos(a)*std::cos(e)};
}
inline Vec3 viewpoint(Vec3 v, float yaw, float pitch, float roll) {
    float a = -yaw*PI/180, b = -pitch*PI/180, c = -roll*PI/180;
    Vec3 y{std::cos(a)*v.x-std::sin(a)*v.z, v.y, std::sin(a)*v.x+std::cos(a)*v.z};
    Vec3 p{y.x, std::cos(b)*y.y-std::sin(b)*y.z, std::sin(b)*y.y+std::cos(b)*y.z};
    return {std::cos(c)*p.x-std::sin(c)*p.y, std::sin(c)*p.x+std::cos(c)*p.y, p.z};
}
struct Coeff { double b0=1,b1=0,b2=0,a1=0,a2=0; };
// W3C/RBJ cookbook: peak, low shelf, high shelf, low pass, high pass, notch.
inline Coeff filter(int type, float hz, float gain, float q, int rate=RATE) {
    double w=2*PI*clamp(hz,20.f,rate*.45f)/rate, cs=cos(w), sn=sin(w);
    double A=pow(10.,clamp(gain,-24.f,24.f)/40.), alpha=sn/(2*clamp(q,.1f,12.f));
    double b0,b1,b2,a0,a1,a2;
    if(type==1 || type==2) {
        double al=sn/2*sqrt(2.), s=2*sqrt(A)*al;
        if(type==1) { b0=A*((A+1)-(A-1)*cs+s); b1=2*A*((A-1)-(A+1)*cs); b2=A*((A+1)-(A-1)*cs-s); a0=(A+1)+(A-1)*cs+s; a1=-2*((A-1)+(A+1)*cs); a2=(A+1)+(A-1)*cs-s; }
        else { b0=A*((A+1)+(A-1)*cs+s); b1=-2*A*((A-1)+(A+1)*cs); b2=A*((A+1)+(A-1)*cs-s); a0=(A+1)-(A-1)*cs+s; a1=2*((A-1)-(A+1)*cs); a2=(A+1)-(A-1)*cs-s; }
    } else if(type==3) { b0=(1-cs)/2; b1=1-cs; b2=b0; a0=1+alpha;a1=-2*cs;a2=1-alpha; }
    else if(type==4) { b0=(1+cs)/2;b1=-(1+cs);b2=b0;a0=1+alpha;a1=-2*cs;a2=1-alpha; }
    else if(type==5) { b0=1;b1=-2*cs;b2=1;a0=1+alpha;a1=-2*cs;a2=1-alpha; }
    else { b0=1+alpha*A;b1=-2*cs;b2=1-alpha*A;a0=1+alpha/A;a1=-2*cs;a2=1-alpha/A; }
    return {b0/a0,b1/a0,b2/a0,a1/a0,a2/a0};
}
struct Biquad {
    Coeff c, target; double z1=0,z2=0;
    void set(Coeff n) { target=n; }
    float tick(float x) {
        // Smooth stable coefficients over ~5 ms; controls never reset delay state.
        constexpr double k=.004;
        c.b0+=k*(target.b0-c.b0);c.b1+=k*(target.b1-c.b1);c.b2+=k*(target.b2-c.b2);c.a1+=k*(target.a1-c.a1);c.a2+=k*(target.a2-c.a2);
        double y=c.b0*x+z1; z1=c.b1*x-c.a1*y+z2; z2=c.b2*x-c.a2*y;
        if(!std::isfinite(y)) {z1=z2=0;return 0;}
        return float(y);
    }
};
struct Limiter {
    static constexpr int LOOK=128;
    std::array<std::array<float,2>,LOOK> delay{}; int pos=0; float gain=1,reduction=0;
    void process(float& l,float& r) {
        float peak=std::max(std::abs(l),std::abs(r));
        float wanted=peak>.94f?.94f/peak:1.f;
        if(wanted<gain)gain=wanted;else gain+=.00015f*(wanted-gain);
        auto old=delay[pos]; delay[pos]={l,r}; pos=(pos+1)%LOOK;
        l=clamp(old[0]*gain,-.98f,.98f);r=clamp(old[1]*gain,-.98f,.98f);
        reduction=-20*std::log10(std::max(.00001f,gain));
    }
};
struct Compressor {
    float envelope=0,gain=1;
    void process(float& l,float& r, bool enabled) {
        float peak=std::max(std::abs(l),std::abs(r));
        envelope+=(peak>envelope?.004f:.0001f)*(peak-envelope);
        float level=20*log10(std::max(envelope,1e-6f));
        float wanted=enabled?db(-std::max(0.f,level+24.f)*.75f+3.f):1.f;
        gain+=.002f*(wanted-gain);l*=gain;r*=gain;
    }
};
struct PathPoint {float time,azimuth,elevation,distance;};
inline PathPoint pathAt(const PathPoint* p,int n,float t,bool loop,float duration,bool roundTrip=false) {
    if(n<=0)return {0,0,0,1.5f};
    if(loop && duration>0){
        const float period=roundTrip?duration*2:duration;
        t=std::fmod(std::max(0.f,t),period);
        if(roundTrip)t=std::min(t,period-t);
    }
    if(t<=p[0].time)return p[0];
    for(int i=1;i<n;i++)if(t<p[i].time){
        float f=clamp((t-p[i-1].time)/std::max(.001f,p[i].time-p[i-1].time),0,1);f=f*f*(3-2*f);
        float delta=std::remainder(p[i].azimuth-p[i-1].azimuth,360.f);
        return {t,p[i-1].azimuth+delta*f,p[i-1].elevation+(p[i].elevation-p[i-1].elevation)*f,p[i-1].distance+(p[i].distance-p[i-1].distance)*f};
    }
    return p[n-1];
}
// Explicit WAVE speaker-bit order, never infer a surround map from channel count alone.
inline bool verifiedMask(int ch,uint32_t mask) { return (ch==1 && (mask==0||mask==4)) || (ch==2 && (mask==0||mask==3)) || (ch==6 && (mask==0x3f||mask==0x60f)) || (ch==8 && mask==0x63f); }
inline float channelAngle(int ch,int index,uint32_t mask) {
    if(ch==1)return 0;
    if(ch==2)return index==0?-30.f:30.f;
    constexpr float seven[]{-30,30,0,0,-150,150,-90,90};
    if(ch==6 && mask==0x60f && index>=4)return index==4?-90.f:90.f;
    return seven[index];
}
}
