// selfie_capture_stub.cpp - no-op selfie_capture so SelfieStick.dll links before the real
// screenshot module (selfie_capture.cpp, owned by the capture agent) exists. build.cmd links
// this file only when selfie_capture.cpp is absent. With the stub the shutter / self-timer,
// flash and click still work, but no screenshot is taken.
#include "selfie_shared.h"

namespace selfie_capture {
bool Install() { return false; }
bool RequestShot() { return false; }
uint32_t ShotsTaken() { return 0; }
const char* Status() { return "capture=stub(not linked)"; }
}
