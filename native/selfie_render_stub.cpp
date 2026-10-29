// selfie_render_stub.cpp - no-op selfie_render so SelfieStick.dll links before the real
// renderer (selfie_render.cpp, owned by the render agent) exists. build.cmd links this
// file only when selfie_render.cpp is absent. With the stub nothing is drawn and
// selfie::Tick() is never called; the core then advances from the camera hook.
#include <windows.h>
#include <d3d12.h>          // before selfie_shared.h: EchoQueue must return ::ID3D12CommandQueue* (see selfie_render.h)
#include "selfie_shared.h"

namespace selfie_render {
bool Install() { return false; }
void Uninstall() {}
const char* Status() { return "render=stub(not linked)"; }
ID3D12CommandQueue* EchoQueue() { return nullptr; }
}
