#pragma once
// Selfie Stick control for the separate SelfieStick.dll (SelfieCam/DESIGN.md section 5,
// contract v2 in SelfieCam/native/selfie_shared.h).
// Platform-independent: runtime.cpp supplies a host that loads the DLL and
// makes fault-guarded calls; tests.cpp drives every state with a fake host.
//
// Host interface:
//   selfie_switch::Load load(Api&)        load DLL once and resolve the exports
//   bool set(const Api&,int on)           SelfieStick_SetEnabled; false on fault
//   bool query(const Api&,int& on)        SelfieStick_IsEnabled;  false on fault
//   const char* status(const Api&)        SelfieStick_Status, or nullptr on fault
//   bool getFloat(float(*)(),float&)      v2 GetFov/GetLength; false on fault
//   bool stepFloat(float(*)(int),int,float&)  v2 StepFov/StepLength; false on fault
//   bool getInt(int(*)(),int&)            v2 ShotsTaken; false on fault
//   void log(const char*)
//
// A v1 DLL (SetEnabled/IsEnabled/Status only) keeps the toggle working; the
// FOV/LENGTH rows then read N/A and their -/+ controls stay hidden.
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace selfie_switch {

struct Api {
    void (*setEnabled)(int)=nullptr;
    int (*isEnabled)()=nullptr;
    const char* (*status)()=nullptr;
    // ---- contract v2 (each optional; resolved independently) ----
    float (*getFov)()=nullptr;
    float (*stepFov)(int)=nullptr;
    float (*getLength)()=nullptr;
    float (*stepLength)(int)=nullptr;
    int (*shotsTaken)()=nullptr;
    bool complete() const { return setEnabled && isEnabled && status; }
    bool fov() const { return getFov && stepFov; }
    bool length() const { return getLength && stepLength; }
    bool shots() const { return shotsTaken!=nullptr; }
};
enum class Load { Ok, NoFile, NoExports };
enum class State { Unloaded, Ready, Missing, Error };
enum class Value { Fov, Length };

// Plausibility bounds for values read back from the DLL (contract: FOV 60..130,
// length 0.5..2.0). Anything non-finite or far outside is shown as N/A.
inline bool fovValid(float v) { return std::isfinite(v) && v>=1.f && v<=179.f; }
inline bool lengthValid(float v) { return std::isfinite(v) && v>0.f && v<=10.f; }

struct Switch {
    State state=State::Unloaded;
    Api api{};
    bool enabled=false;      // last value reported by SelfieStick_IsEnabled
    bool retryLoad=false;    // DLL absent/unloadable: a later press may retry
    bool noExports=false;    // DLL present but lacks the v1 exports
    uint64_t nextPoll=0;
    // Last values read from the v2 getters (meaningful only when *Known).
    float fov=0,length=0;
    int shots=0;
    bool fovKnown=false,lengthKnown=false,shotsKnown=false;
    bool reportedNoV2=false;

    // Default OFF every launch: nothing is loaded until the Selfie page opens.
    const char* label() const {
        switch(state) {
        case State::Ready: return enabled?"SELFIE STICK: ON":"SELFIE STICK: OFF";
        case State::Missing: return "SELFIE STICK: MISSING";
        case State::Error: return "SELFIE STICK: ERROR";
        default: return "SELFIE STICK: OFF";
        }
    }
    bool ready() const { return state==State::Ready; }
    bool canStep(Value which) const { return ready() && (which==Value::Fov?api.fov():api.length()); }

    // Loads the DLL once (or retries after an absent file). True when Ready.
    template<class Host> bool ensureLoaded(Host& host) {
        if(state==State::Ready) return true;
        if(state==State::Error) return false;
        if(state==State::Missing && !retryLoad) return false;
        Api loaded{};
        auto result=host.load(loaded);
        if(result!=Load::Ok || !loaded.complete()) {
            state=State::Missing;enabled=false;api={};clearValues();
            // Exports missing: the module stays loaded (never freed while it
            // may own hooks), so retrying cannot help. Absent file: retry later.
            retryLoad=result==Load::NoFile;noExports=!retryLoad;
            host.log(result==Load::NoFile?"Selfie Stick MISSING: SelfieStick.dll not found or not loadable"
                                         :"Selfie Stick MISSING: SelfieStick.dll lacks SelfieStick_SetEnabled/IsEnabled/Status");
            return false;
        }
        api=loaded;state=State::Ready;retryLoad=false;noExports=false;
        int on=0;
        if(!host.query(api,on)) {faulted(host,"IsEnabled");return false;}
        enabled=on!=0;
        char line[160];
        snprintf(line,sizeof(line),"SelfieStick.dll loaded (contract %s: fov=%d length=%d shots=%d)",
                 api.fov()&&api.length()&&api.shots()?"v2":"v1",int(api.fov()),int(api.length()),int(api.shots()));
        host.log(line);
        return true;
    }

    // Selfie page opened: load if needed and show fresh values immediately.
    template<class Host> void open(Host& host,uint64_t now) {
        if(!ensureLoaded(host)) return;
        refresh(host,now,true);
    }

    // The ON/OFF toggle (Selfie page first row; also the Tools-row fallback).
    template<class Host> void press(Host& host,uint64_t now) {
        if(state==State::Error) {host.log("Selfie Stick press ignored after an earlier fault");return;}
        if(!ensureLoaded(host)) return;
        int desired=enabled?0:1, on=0;
        if(!host.set(api,desired)) {faulted(host,"SetEnabled");return;}
        if(!host.query(api,on)) {faulted(host,"IsEnabled");return;}
        enabled=on!=0;
        const char* detail=host.status(api);
        char line[200];
        snprintf(line,sizeof(line),"Selfie Stick %s (requested %s) status=%s",enabled?"ON":"OFF",desired?"ON":"OFF",detail?detail:"(unavailable)");
        host.log(line);
        if(refreshValues(host)) nextPoll=now+1000;
    }

    // FOV / LENGTH -/+ (dir -1/+1). Re-reads every displayed value afterwards.
    template<class Host> void step(Host& host,Value which,int dir,uint64_t now) {
        if(state!=State::Ready) {host.log("Selfie Stick step ignored: SelfieStick.dll not ready");return;}
        dir=dir<0?-1:1;
        const bool fovStep=which==Value::Fov;
        if(!canStep(which)) {
            if(!reportedNoV2) {reportedNoV2=true;host.log("Selfie Stick FOV/LENGTH unavailable: SelfieStick.dll lacks the v2 Get/Step exports");}
            return;
        }
        float result=0;
        if(!host.stepFloat(fovStep?api.stepFov:api.stepLength,dir,result)) {faulted(host,fovStep?"StepFov":"StepLength");return;}
        if(!refresh(host,now,true)) return;
        char line[120];
        if(fovStep) snprintf(line,sizeof(line),"Selfie Stick FOV %s -> %.0f (getter %.0f)",dir<0?"-":"+",double(result),double(fov));
        else snprintf(line,sizeof(line),"Selfie Stick LENGTH %s -> %.2f m (getter %.2f m)",dir<0?"-":"+",double(result),double(length));
        host.log(line);
    }

    // Keeps the display truthful if the DLL changes state on its own; at most
    // once a second. IsEnabled is polled whenever loaded; the v2 getters only
    // while the Selfie page is open.
    template<class Host> void poll(Host& host,uint64_t now,bool pageOpen=false) {
        if(state!=State::Ready || now<nextPoll) return;
        bool was=enabled;
        if(!refresh(host,now,pageOpen)) return;
        if(was!=enabled) host.log(enabled?"Selfie Stick reported ON":"Selfie Stick reported OFF");
    }

private:
    void clearValues() {fovKnown=lengthKnown=shotsKnown=false;fov=length=0;shots=0;}
    template<class Host> bool refresh(Host& host,uint64_t now,bool values) {
        nextPoll=now+1000;
        int on=0;
        if(!host.query(api,on)) {faulted(host,"IsEnabled");return false;}
        enabled=on!=0;
        return values?refreshValues(host):true;
    }
    template<class Host> bool refreshValues(Host& host) {
        if(state!=State::Ready) return false;
        float v=0;int n=0;
        if(api.fov()) {
            if(!host.getFloat(api.getFov,v)) {faulted(host,"GetFov");return false;}
            fovKnown=fovValid(v);fov=fovKnown?v:0;
        } else fovKnown=false;
        if(api.length()) {
            if(!host.getFloat(api.getLength,v)) {faulted(host,"GetLength");return false;}
            lengthKnown=lengthValid(v);length=lengthKnown?v:0;
        } else lengthKnown=false;
        if(api.shots()) {
            if(!host.getInt(api.shotsTaken,n)) {faulted(host,"ShotsTaken");return false;}
            shotsKnown=n>=0;shots=shotsKnown?n:0;
        } else shotsKnown=false;
        return true;
    }
    template<class Host> void faulted(Host& host,const char* call) {
        state=State::Error;enabled=false;clearValues();
        char line[120];snprintf(line,sizeof(line),"Selfie Stick ERROR: SelfieStick_%s faulted; switch disabled for this session",call);
        host.log(line);
    }
};

// ---- Selfie page: navigation and text, independent of the native canvas. ----
namespace page {
// Control order matches SELFIE_KEYS generated by TabletMinesweeper/build_tablet.py.
enum Control { Toggle=0, FovMinus, FovPlus, LengthMinus, LengthPlus, Back, Count };
enum class Where { Other, Tools, Selfie };

// Tools' SELFIE STICK row opens the Selfie page and loads the DLL. If the
// Selfie page canvas is not loaded it falls back to the 0.6.12 direct toggle.
template<class Host> Where openFromTools(Switch& s,Host& host,bool pageLoaded,uint64_t now) {
    if(!pageLoaded) {host.log("Selfie page canvas unavailable; Tools row toggles directly");s.press(host,now);return Where::Tools;}
    host.log("Selfie page opened");
    s.open(host,now);
    return Where::Selfie;
}

// A Selfie page control. Returns the page to show afterwards.
template<class Host> Where press(Switch& s,Host& host,int control,uint64_t now) {
    switch(control) {
    case Toggle: s.press(host,now);break;
    case FovMinus: s.step(host,Value::Fov,-1,now);break;
    case FovPlus: s.step(host,Value::Fov,1,now);break;
    case LengthMinus: s.step(host,Value::Length,-1,now);break;
    case LengthPlus: s.step(host,Value::Length,1,now);break;
    case Back: host.log("Selfie page closed");return Where::Tools;
    default: break;
    }
    return Where::Selfie;
}

// Whether a control is shown/touchable on the open page.
inline bool visible(const Switch& s,int control) {
    switch(control) {
    case FovMinus: case FovPlus: return s.canStep(Value::Fov);
    case LengthMinus: case LengthPlus: return s.canStep(Value::Length);
    case Toggle: case Back: return true;
    default: return false;
    }
}

// Every string fits the authored 64-byte label capacity (snprintf truncates).
struct Text { char toggle[64], fov[64], length[64], status[64], tools[64]; };
inline void text(const Switch& s,Text& t) {
    snprintf(t.toggle,sizeof(t.toggle),"%s",s.label());
    // Tools row: current state plus a chevron meaning "opens a page".
    snprintf(t.tools,sizeof(t.tools),"%s  >",s.label());
    // Canvas text is ASCII-only in every stock canvas, so no degree glyph.
    if(s.ready() && s.api.fov() && s.fovKnown) snprintf(t.fov,sizeof(t.fov),"%.0f DEG",double(s.fov));
    else snprintf(t.fov,sizeof(t.fov),"N/A");
    if(s.ready() && s.api.length() && s.lengthKnown) snprintf(t.length,sizeof(t.length),"%.2f M",double(s.length));
    else snprintf(t.length,sizeof(t.length),"N/A");
    switch(s.state) {
    case State::Unloaded: snprintf(t.status,sizeof(t.status),"SELFIESTICK.DLL NOT LOADED YET");break;
    case State::Missing:
        snprintf(t.status,sizeof(t.status),"%s",s.noExports?"SELFIESTICK.DLL INCOMPATIBLE | SEE TRAINER.LOG"
                                                           :"SELFIESTICK.DLL NOT FOUND | TAP TOGGLE TO RETRY");break;
    case State::Error: snprintf(t.status,sizeof(t.status),"SELFIESTICK.DLL FAULTED | SEE TRAINER.LOG");break;
    case State::Ready:
        if(!s.api.fov() || !s.api.length()) snprintf(t.status,sizeof(t.status),"OLDER SELFIESTICK.DLL | FOV/LENGTH N/A");
        else if(s.shotsKnown) snprintf(t.status,sizeof(t.status),"SHOTS TAKEN: %d",s.shots);
        else snprintf(t.status,sizeof(t.status),"READY");
        break;
    }
}
} // namespace page

} // namespace selfie_switch
