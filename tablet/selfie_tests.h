#pragma once
// Selfie Stick: switch label states, lazy single load, missing DLL or exports,
// faults (v1), plus the 0.6.13 Selfie page: navigation, v2 FOV/LENGTH values,
// N/A for v1 DLLs, refresh after presses and at most once a second, guarded calls.
#include "selfie_switch.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace selfie_fake {
static int enabled=0, setCalls=0, queryCalls=0;
static bool ignoreSet=false;
static void setEnabled(int on) {++setCalls;if(!ignoreSet)enabled=on?1:0;}
static int isEnabled() {++queryCalls;return enabled;}
static const char* status() {return enabled?"stick visible":"stick hidden";}
// v2: mirrors the contract (FOV 60..130 step 10, length 0.5..2.0 step 0.25).
static float fov=112, length=1;
static int shots=0, getCalls=0, stepCalls=0;
static float getFov() {++getCalls;return fov;}
static float getLength() {++getCalls;return length;}
static float stepFov(int dir) {++stepCalls;fov=std::fmin(130.f,std::fmax(60.f,fov+10.f*dir));return fov;}
static float stepLength(int dir) {++stepCalls;length=std::fmin(2.f,std::fmax(.5f,length+.25f*dir));return length;}
static int shotsTaken() {++getCalls;return shots;}
}

struct FakeSelfieHost {
    selfie_switch::Load result=selfie_switch::Load::Ok;
    bool exports=true, v2=false, faultSet=false, faultQuery=false, faultGet=false, faultStep=false;
    int loads=0;
    std::vector<std::string> lines;
    selfie_switch::Load load(selfie_switch::Api& api) {
        ++loads;
        if(result==selfie_switch::Load::NoFile) return result;
        if(exports) api={selfie_fake::setEnabled,selfie_fake::isEnabled,selfie_fake::status};
        else api.setEnabled=selfie_fake::setEnabled; // IsEnabled/Status absent
        if(v2) {
            api.getFov=selfie_fake::getFov;api.stepFov=selfie_fake::stepFov;
            api.getLength=selfie_fake::getLength;api.stepLength=selfie_fake::stepLength;
            api.shotsTaken=selfie_fake::shotsTaken;
        }
        return exports?selfie_switch::Load::Ok:selfie_switch::Load::NoExports;
    }
    bool set(const selfie_switch::Api& api,int on) {if(faultSet)return false;api.setEnabled(on);return true;}
    bool query(const selfie_switch::Api& api,int& on) {if(faultQuery)return false;on=api.isEnabled();return true;}
    const char* status(const selfie_switch::Api& api) {return api.status();}
    // Same shape as runtime.cpp's guarded calls: a null pointer is never called.
    bool getFloat(float(*call)(),float& out) {if(!call || faultGet)return false;out=call();return true;}
    bool stepFloat(float(*call)(int),int dir,float& out) {if(!call || faultStep)return false;out=call(dir);return true;}
    bool getInt(int(*call)(),int& out) {if(!call || faultGet)return false;out=call();return true;}
    void log(const char* line) {lines.push_back(line);}
    bool logged(const char* text) const {for(auto& l:lines)if(l.find(text)!=std::string::npos)return true;return false;}
    int count(const char* text) const {int n=0;for(auto& l:lines)n+=l.find(text)!=std::string::npos;return n;}
};

inline void resetSelfieFake() {
    selfie_fake::enabled=0;selfie_fake::setCalls=0;selfie_fake::queryCalls=0;selfie_fake::ignoreSet=false;
    selfie_fake::fov=112;selfie_fake::length=1;selfie_fake::shots=0;selfie_fake::getCalls=0;selfie_fake::stepCalls=0;
}

inline void testSelfie() {
    using selfie_switch::Switch;using selfie_switch::State;using selfie_switch::Load;
    // Default OFF every launch, nothing loaded before the first press.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;
        assert(!strcmp(s.label(),"SELFIE STICK: OFF") && s.state==State::Unloaded);
        s.poll(host,0);assert(host.loads==0 && selfie_fake::queryCalls==0);
        s.press(host,100);
        assert(host.loads==1 && s.state==State::Ready && selfie_fake::enabled==1);
        assert(!strcmp(s.label(),"SELFIE STICK: ON") && host.logged("Selfie Stick ON") && host.logged("stick visible"));
        s.press(host,200);
        assert(host.loads==1 && selfie_fake::enabled==0 && !strcmp(s.label(),"SELFIE STICK: OFF"));
        for(int i=0;i<10;++i)s.press(host,300+i);
        assert(host.loads==1 && selfie_fake::setCalls==12 && !strcmp(s.label(),"SELFIE STICK: OFF"));
        // The label follows SelfieStick_IsEnabled, not the requested value.
        selfie_fake::ignoreSet=true;s.press(host,400);
        assert(!strcmp(s.label(),"SELFIE STICK: OFF") && host.logged("requested ON"));
        selfie_fake::ignoreSet=false;
        // The DLL turning itself off is reflected within a second.
        s.press(host,500);assert(!strcmp(s.label(),"SELFIE STICK: ON"));
        selfie_fake::enabled=0;
        unsigned queries=selfie_fake::queryCalls;
        s.poll(host,900);assert(selfie_fake::queryCalls==queries && !strcmp(s.label(),"SELFIE STICK: ON"));
        s.poll(host,1500);assert(selfie_fake::queryCalls==queries+1 && !strcmp(s.label(),"SELFIE STICK: OFF"));
        s.poll(host,1600);assert(selfie_fake::queryCalls==queries+1);
    }
    // Missing DLL: MISSING label, logged, no calls, and a later press may retry.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.result=Load::NoFile;
        s.press(host,0);
        assert(s.state==State::Missing && !strcmp(s.label(),"SELFIE STICK: MISSING"));
        assert(host.logged("MISSING") && selfie_fake::setCalls==0);
        s.poll(host,5000);assert(selfie_fake::queryCalls==0);
        s.press(host,6000);assert(host.loads==2 && s.state==State::Missing);
        host.result=Load::Ok;s.press(host,7000);
        assert(host.loads==3 && s.state==State::Ready && !strcmp(s.label(),"SELFIE STICK: ON"));
    }
    // Exports missing: MISSING permanently; the module is not reloaded or called.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.exports=false;
        s.press(host,0);s.press(host,10);s.press(host,20);
        assert(host.loads==1 && s.state==State::Missing && !strcmp(s.label(),"SELFIE STICK: MISSING"));
        assert(selfie_fake::setCalls==0 && host.logged("lacks"));
    }
    // A fault inside SetEnabled or IsEnabled disables the switch for the session.
    for(int which=0;which<2;++which) {
        resetSelfieFake();Switch s;FakeSelfieHost host;
        s.press(host,0);assert(s.state==State::Ready);
        if(which)host.faultQuery=true;else host.faultSet=true;
        s.press(host,10);
        assert(s.state==State::Error && !strcmp(s.label(),"SELFIE STICK: ERROR") && host.logged("faulted"));
        unsigned calls=selfie_fake::setCalls;
        s.press(host,20);s.poll(host,5000);
        assert(selfie_fake::setCalls==calls && host.loads==1);
    }
    // Every label fits the authored text capacity (64 bytes, see build_tablet.py).
    for(auto label:{"SELFIE STICK: OFF","SELFIE STICK: ON","SELFIE STICK: MISSING","SELFIE STICK: ERROR"})assert(strlen(label)<64);
    std::puts("PASS: Selfie Stick default OFF, lazy single load, ON/OFF from IsEnabled, self-disable poll, MISSING DLL/exports with retry rules, fault isolation");
}

inline void testSelfiePage() {
    using selfie_switch::Switch;using selfie_switch::State;using selfie_switch::Load;using selfie_switch::Value;
    namespace pg=selfie_switch::page;using pg::Where;
    // Navigation: Tools row opens the page and loads once WITHOUT toggling; Back returns.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=true;
        pg::Text t;pg::text(s,t);
        assert(!strcmp(t.tools,"SELFIE STICK: OFF  >") && !strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A"));
        assert(pg::openFromTools(s,host,true,1000)==Where::Selfie);
        assert(host.loads==1 && s.ready() && selfie_fake::setCalls==0 && !s.enabled && host.logged("contract v2"));
        // Values come from the getters immediately on open.
        pg::text(s,t);
        assert(!strcmp(t.toggle,"SELFIE STICK: OFF") && !strcmp(t.fov,"112 DEG") && !strcmp(t.length,"1.00 M"));
        assert(!strcmp(t.status,"SHOTS TAKEN: 0"));
        for(int c=0;c<pg::Count;++c)assert(pg::visible(s,c));
        assert(!pg::visible(s,pg::Count) && !pg::visible(s,-1));
        // Toggle stays on the page; Back goes to Tools; reopening never reloads.
        assert(pg::press(s,host,pg::Toggle,1100)==Where::Selfie && selfie_fake::enabled==1);
        pg::text(s,t);assert(!strcmp(t.toggle,"SELFIE STICK: ON") && !strcmp(t.tools,"SELFIE STICK: ON  >"));
        for(int c:{int(pg::FovMinus),int(pg::FovPlus),int(pg::LengthMinus),int(pg::LengthPlus)})
            assert(pg::press(s,host,c,1200)==Where::Selfie);
        assert(pg::press(s,host,pg::Back,1300)==Where::Tools && host.logged("Selfie page closed"));
        assert(pg::press(s,host,99,1300)==Where::Selfie); // unknown control: no-op
        assert(pg::openFromTools(s,host,true,1400)==Where::Selfie && host.loads==1 && selfie_fake::enabled==1);
        // Without the page canvas, the Tools row keeps the 0.6.12 direct toggle.
        assert(pg::openFromTools(s,host,false,1500)==Where::Tools && selfie_fake::enabled==0 && host.loads==1);
    }
    // FOV / LENGTH steps clamp in the DLL; the display shows what the getters report.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=true;pg::Text t;
        pg::openFromTools(s,host,true,0);
        pg::press(s,host,pg::FovPlus,10);pg::text(s,t);assert(!strcmp(t.fov,"122 DEG"));
        for(int i=0;i<5;++i)pg::press(s,host,pg::FovPlus,20+i);
        pg::text(s,t);assert(!strcmp(t.fov,"130 DEG") && host.logged("Selfie Stick FOV +"));
        for(int i=0;i<20;++i)pg::press(s,host,pg::FovMinus,40+i);
        pg::text(s,t);assert(!strcmp(t.fov,"60 DEG"));
        pg::press(s,host,pg::LengthPlus,70);pg::text(s,t);assert(!strcmp(t.length,"1.25 M"));
        for(int i=0;i<10;++i)pg::press(s,host,pg::LengthPlus,80+i);
        pg::text(s,t);assert(!strcmp(t.length,"2.00 M"));
        for(int i=0;i<10;++i)pg::press(s,host,pg::LengthMinus,100+i);
        pg::text(s,t);assert(!strcmp(t.length,"0.50 M") && host.logged("LENGTH -"));
        // Every press re-reads the getters (2 values + shots), so changes the
        // DLL made meanwhile appear too.
        selfie_fake::shots=4;selfie_fake::length=1.5f;
        int gets=selfie_fake::getCalls;
        pg::press(s,host,pg::FovPlus,200);
        assert(selfie_fake::getCalls==gets+3);
        pg::text(s,t);assert(!strcmp(t.fov,"70 DEG") && !strcmp(t.length,"1.50 M") && !strcmp(t.status,"SHOTS TAKEN: 4"));
        // The toggle also refreshes values.
        selfie_fake::shots=5;pg::press(s,host,pg::Toggle,300);pg::text(s,t);assert(!strcmp(t.status,"SHOTS TAKEN: 5"));
    }
    // Periodic refresh: at most once a second, and the getters only while the page is open.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=true;pg::Text t;
        pg::openFromTools(s,host,true,1000);
        int gets=selfie_fake::getCalls,queries=selfie_fake::queryCalls;
        selfie_fake::fov=90;selfie_fake::shots=2;selfie_fake::enabled=1;
        for(uint64_t now=1000;now<2000;now+=16)s.poll(host,now,true);
        assert(selfie_fake::getCalls==gets && selfie_fake::queryCalls==queries);
        pg::text(s,t);assert(!strcmp(t.fov,"112 DEG"));
        s.poll(host,2000,true);
        assert(selfie_fake::getCalls==gets+3 && selfie_fake::queryCalls==queries+1);
        pg::text(s,t);assert(!strcmp(t.fov,"90 DEG") && !strcmp(t.status,"SHOTS TAKEN: 2") && !strcmp(t.toggle,"SELFIE STICK: ON"));
        for(uint64_t now=2001;now<3000;now+=16)s.poll(host,now,true);
        assert(selfie_fake::getCalls==gets+3);
        // Page closed: IsEnabled still polled once a second, getters are not.
        s.poll(host,3000,false);s.poll(host,4000,false);
        assert(selfie_fake::getCalls==gets+3 && selfie_fake::queryCalls==queries+3);
        // A press restarts the one-second window.
        pg::press(s,host,pg::FovPlus,4500);int after=selfie_fake::getCalls;
        s.poll(host,5000,true);assert(selfie_fake::getCalls==after);
        s.poll(host,5500,true);assert(selfie_fake::getCalls==after+3);
    }
    // v1 DLL (no v2 exports): toggle works, FOV/LENGTH read N/A, -/+ hidden and never called.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=false;pg::Text t;
        assert(pg::openFromTools(s,host,true,0)==Where::Selfie && s.ready() && host.logged("contract v1"));
        pg::text(s,t);
        assert(!strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A") && !strcmp(t.status,"OLDER SELFIESTICK.DLL | FOV/LENGTH N/A"));
        assert(pg::visible(s,pg::Toggle) && pg::visible(s,pg::Back));
        for(int c:{int(pg::FovMinus),int(pg::FovPlus),int(pg::LengthMinus),int(pg::LengthPlus)})assert(!pg::visible(s,c));
        for(int i=0;i<3;++i){pg::press(s,host,pg::FovPlus,10+i);pg::press(s,host,pg::LengthMinus,20+i);}
        assert(selfie_fake::stepCalls==0 && selfie_fake::getCalls==0 && host.count("lacks the v2")==1 && s.ready());
        pg::press(s,host,pg::Toggle,30);
        pg::text(s,t);assert(!strcmp(t.toggle,"SELFIE STICK: ON") && !strcmp(t.fov,"N/A") && selfie_fake::getCalls==0);
        s.poll(host,5000,true);assert(selfie_fake::getCalls==0 && s.ready());
        // Partial v2 (FOV only): each feature is gated independently.
        Switch p;FakeSelfieHost partial;partial.v2=true;resetSelfieFake();
        p.ensureLoaded(partial);p.api.stepLength=nullptr;
        assert(p.canStep(Value::Fov) && !p.canStep(Value::Length));
        p.step(partial,Value::Length,1,10);assert(selfie_fake::stepCalls==0 && p.ready());
        p.step(partial,Value::Fov,1,20);pg::text(p,t);
        assert(!strcmp(t.fov,"122 DEG") && !strcmp(t.length,"N/A") && !strcmp(t.status,"OLDER SELFIESTICK.DLL | FOV/LENGTH N/A"));
    }
    // MISSING / incompatible / ERROR pages all show N/A and hide -/+.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.result=Load::NoFile;pg::Text t;
        assert(pg::openFromTools(s,host,true,0)==Where::Selfie && s.state==State::Missing);
        pg::text(s,t);
        assert(!strcmp(t.toggle,"SELFIE STICK: MISSING") && !strcmp(t.tools,"SELFIE STICK: MISSING  >"));
        assert(!strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A") && !strcmp(t.status,"SELFIESTICK.DLL NOT FOUND | TAP TOGGLE TO RETRY"));
        assert(!pg::visible(s,pg::FovPlus) && pg::visible(s,pg::Toggle));
        pg::press(s,host,pg::FovPlus,10);assert(host.loads==1 && host.logged("not ready"));
        // Reopening the page retries an absent file (like the toggle does).
        host.result=Load::Ok;host.v2=true;pg::openFromTools(s,host,true,20);
        assert(host.loads==2 && s.ready() && selfie_fake::setCalls==0);
        pg::text(s,t);assert(!strcmp(t.fov,"112 DEG"));
    }
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.exports=false;pg::Text t;
        pg::openFromTools(s,host,true,0);pg::openFromTools(s,host,true,10);pg::press(s,host,pg::Toggle,20);
        pg::text(s,t);
        assert(host.loads==1 && !strcmp(t.toggle,"SELFIE STICK: MISSING") && !strcmp(t.status,"SELFIESTICK.DLL INCOMPATIBLE | SEE TRAINER.LOG"));
        assert(!strcmp(t.fov,"N/A") && selfie_fake::setCalls==0);
    }
    // Guarded v2 calls: a fault in any getter/step marks ERROR, clears values, stops all calls.
    for(int which=0;which<3;++which) {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=true;pg::Text t;
        pg::openFromTools(s,host,true,0);assert(s.ready());
        if(which==0){host.faultStep=true;pg::press(s,host,pg::LengthPlus,10);assert(host.logged("SelfieStick_StepLength faulted"));}
        if(which==1){host.faultGet=true;pg::press(s,host,pg::FovMinus,10);assert(host.logged("SelfieStick_GetFov faulted"));}
        if(which==2){host.faultGet=true;s.poll(host,1000,true);assert(host.logged("SelfieStick_GetFov faulted"));}
        assert(s.state==State::Error);
        pg::text(s,t);
        assert(!strcmp(t.toggle,"SELFIE STICK: ERROR") && !strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A"));
        assert(!strcmp(t.status,"SELFIESTICK.DLL FAULTED | SEE TRAINER.LOG"));
        for(int c=pg::FovMinus;c<=pg::LengthPlus;++c)assert(!pg::visible(s,c));
        host.faultGet=host.faultStep=false;
        int steps=selfie_fake::stepCalls,gets=selfie_fake::getCalls,sets=selfie_fake::setCalls;
        pg::press(s,host,pg::FovPlus,20);pg::press(s,host,pg::Toggle,30);s.poll(host,9000,true);
        pg::openFromTools(s,host,true,9100);
        assert(selfie_fake::stepCalls==steps && selfie_fake::getCalls==gets && selfie_fake::setCalls==sets && host.loads==1);
    }
    // Implausible values from the DLL are shown as N/A, not as garbage, and are not faults.
    {
        resetSelfieFake();Switch s;FakeSelfieHost host;host.v2=true;pg::Text t;
        selfie_fake::fov=NAN;selfie_fake::length=INFINITY;selfie_fake::shots=-1;
        pg::openFromTools(s,host,true,0);pg::text(s,t);
        assert(s.ready() && !strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A") && !strcmp(t.status,"READY"));
        selfie_fake::fov=1e30f;selfie_fake::length=-1;s.poll(host,1000,true);pg::text(s,t);
        assert(!strcmp(t.fov,"N/A") && !strcmp(t.length,"N/A"));
        selfie_fake::fov=95;selfie_fake::length=.75f;selfie_fake::shots=12345;s.poll(host,2000,true);pg::text(s,t);
        assert(!strcmp(t.fov,"95 DEG") && !strcmp(t.length,"0.75 M") && !strcmp(t.status,"SHOTS TAKEN: 12345"));
    }
    // Every page string fits the 64-byte authored label capacity.
    {
        pg::Text t;Switch s;
        for(auto st:{State::Unloaded,State::Ready,State::Missing,State::Error}) {
            s.state=st;pg::text(s,t);
            for(const char* x:{t.toggle,t.fov,t.length,t.status,t.tools})assert(strlen(x)<64);
        }
    }
    std::puts("PASS: Selfie page open/back navigation, load-on-open without toggling, v2 FOV/LENGTH steps and clamps, refresh after presses and <=1/s while open, v1 N/A, MISSING/ERROR states, guarded v2 faults, implausible values");
}
