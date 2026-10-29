// capture_io.h - output side of the screenshot module (worker thread only):
//   WritePng  : opaque BGRA8 -> 24-bit PNG through WIC (caller has initialised COM)
//   PutDibOnClipboard : CF_DIB, OpenClipboard retried a few times with short sleeps
#pragma once
#include "capture_util.h"
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace capture {

inline bool WritePng(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height, const char** why) {
    using Microsoft::WRL::ComPtr;
    auto fail = [&](const char* r) { if (why) *why = r; return false; };
    if (!bgra || !width || !height) return fail("png-bad-image");
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return fail("wic-factory-failed");
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4, width * height * 4,
                                               const_cast<BYTE*>(bgra), &bitmap)))
        return fail("wic-bitmap-failed");
    ComPtr<IWICBitmapSource> bgr;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat24bppBGR, bitmap.Get(), &bgr))) return fail("wic-convert-failed");
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return fail("png-open-failed");
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)))
        return fail("png-encoder-failed");
    if (format != GUID_WICPixelFormat24bppBGR) return fail("png-format-rejected");
    if (FAILED(frame->WriteSource(bgr.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()) ||
        FAILED(stream->Commit(STGC_DEFAULT)))
        return fail("png-write-failed");
    if (why) *why = nullptr;
    return true;
}

// `owner` should be a window of the calling thread (the thread must pump messages), or nullptr.
inline bool PutDibOnClipboard(HWND owner, const std::vector<uint8_t>& dib, const char** why, int attempts = 10, DWORD sleepMs = 15) {
    auto fail = [&](const char* r) { if (why) *why = r; return false; };
    if (dib.size() <= sizeof(BITMAPINFOHEADER)) return fail("dib-empty");
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, dib.size());
    if (!memory) return fail("dib-alloc-failed");
    void* p = GlobalLock(memory);
    if (!p) { GlobalFree(memory); return fail("dib-lock-failed"); }
    std::memcpy(p, dib.data(), dib.size());
    GlobalUnlock(memory);
    bool open = false;
    for (int i = 0; i < attempts && !open; i++) {
        open = OpenClipboard(owner) != FALSE;
        if (!open) Sleep(sleepMs);
    }
    if (!open) { GlobalFree(memory); return fail("clipboard-busy"); }
    bool ok = EmptyClipboard() && SetClipboardData(CF_DIB, memory) != nullptr;
    CloseClipboard();
    if (!ok) { GlobalFree(memory); return fail("clipboard-set-failed"); }  // the system owns `memory` only on success
    if (why) *why = nullptr;
    return true;
}

}  // namespace capture
