// capture_tests.cpp - tests for the SelfieStick screenshot module (no headset, no Echo).
//   1. pixel conversion: every supported format, sRGB/typeless variants, row-pitch padding, opaque alpha
//   2. CF_DIB construction + a real SetClipboardData / GetClipboardData round trip
//   3. shot naming, folder creation and collision handling (_2, _3 ...)
//   4. PNG write + read back through WIC
//   5. request state machine
//   6. ReadbackCopier on a WARP device + direct queue against textures in PRESENT state
//      (per-format pixel check, readback realloc on resize, fence-gated Busy)
//   7. end to end through the real Present vtable hook on a WARP flip-model swap chain
//      (hidden window): queue-unknown abandon, in-flight rejection, PNG + clipboard contents,
//      other-swapchain and DXGI_PRESENT_TEST filtering, request timeout, ResizeBuffers.
// Built twice by build_capture.cmd; /DCAPTURE_ALT_MANGLING includes selfie_shared.h before <d3d12.h>
// proving EchoQueue() has a single symbol whatever the include order (the contract forward-declares
// ::ID3D12CommandQueue at global scope, so no linker fallback is needed).
#ifdef CAPTURE_ALT_MANGLING
#include "selfie_shared.h"
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "selfie_capture.h"
#include "capture_gpu.h"
#include "capture_io.h"
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

// ---- stub of the render module's queue getter (the DLL gets the real one from selfie_render.cpp) ----
static ::ID3D12CommandQueue* g_testQueue = nullptr;
namespace selfie_render {
::ID3D12CommandQueue* EchoQueue() { return g_testQueue; }
}

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring tempDir(const wchar_t* leaf) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring d = std::wstring(t) + L"selfie_capture_tests\\" + leaf;
    return d;
}
static void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + f.cFileName;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p); else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
static std::vector<std::wstring> listPng(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) { do out.push_back(dir + L"\\" + f.cFileName); while (FindNextFileW(h, &f)); FindClose(h); }
    return out;
}
static uint16_t floatToHalf(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7c00);
    return uint16_t(sign | (exp << 10) | ((x >> 13) & 0x3ff));
}

// WIC PNG -> BGRA8
static bool readPng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &converted)) ||
        FAILED(converted->GetSize(&w, &h)))
        return false;
    GUID container = {};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatPng) return false;
    bgra.resize(size_t(w) * h * 4);
    return SUCCEEDED(converted->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()));
}

// ---------------------------------------------------------------------------------------------
static void testConversion() {
    std::printf("[conversion]\n");
    using namespace capture;
    // 3x2 image, row pitch 20 (12 used + 8 garbage padding bytes per row)
    const uint32_t w = 3, h = 2, pitch = 20;
    std::vector<uint8_t> rgba(pitch * h, 0xEE);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = &rgba[y * pitch + x * 4];
            p[0] = uint8_t(10 * x + 1); p[1] = uint8_t(20 * y + 2); p[2] = uint8_t(100 + x + y); p[3] = uint8_t(x * 7);  // alpha not 255
        }
    const DXGI_FORMAT rgbaFormats[] = {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    for (DXGI_FORMAT f : rgbaFormats) {
        std::vector<uint8_t> out;
        CHECK(ConvertToBgra(f, rgba.data(), rgba.size(), pitch, w, h, out));
        CHECK(out.size() == w * h * 4);
        bool ok = true;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* s = &rgba[y * pitch + x * 4];
                const uint8_t* d = &out[(y * w + x) * 4];
                ok = ok && d[0] == s[2] && d[1] == s[1] && d[2] == s[0] && d[3] == 255;
            }
        CHECK(ok);
    }
    const DXGI_FORMAT bgraFormats[] = {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                       DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB};
    for (DXGI_FORMAT f : bgraFormats) {
        std::vector<uint8_t> out;
        CHECK(ConvertToBgra(f, rgba.data(), rgba.size(), pitch, w, h, out));
        bool ok = true;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* s = &rgba[y * pitch + x * 4];
                const uint8_t* d = &out[(y * w + x) * 4];
                ok = ok && d[0] == s[0] && d[1] == s[1] && d[2] == s[2] && d[3] == 255;
            }
        CHECK(ok);
    }
    // R10G10B10A2: R=1023 G=512 B=4 A=0
    {
        const uint32_t v = 1023u | (512u << 10) | (4u << 20) | (0u << 30);
        uint8_t src[8] = {};
        std::memcpy(src, &v, 4);
        std::vector<uint8_t> out;
        CHECK(ConvertToBgra(DXGI_FORMAT_R10G10B10A2_UNORM, src, 4, 4, 1, 1, out));
        CHECK(out.size() == 4 && out[2] == 255 && out[1] == 128 && out[0] == 1 && out[3] == 255);
        CHECK(ConvertToBgra(DXGI_FORMAT_R10G10B10A2_TYPELESS, src, 4, 4, 1, 1, out));
    }
    // FP16 scRGB: 1.0 -> 255, 0.5 -> 188 (sRGB), -1 -> 0, 4.0 -> 255 (clamp), 0.0031308 -> ~10
    {
        const float vals[2][4] = {{1.f, 0.5f, -1.f, 0.f}, {4.f, 0.f, 0.0031308f, 0.25f}};
        uint16_t src[8];
        for (int p = 0; p < 2; p++) for (int c = 0; c < 4; c++) src[p * 4 + c] = floatToHalf(vals[p][c]);
        std::vector<uint8_t> out;
        CHECK(ConvertToBgra(DXGI_FORMAT_R16G16B16A16_FLOAT, reinterpret_cast<uint8_t*>(src), sizeof(src), 16, 2, 1, out));
        CHECK(out[2] == 255 && out[1] == 188 && out[0] == 0 && out[3] == 255);
        CHECK(out[6] == 255 && out[5] == 0 && out[4] >= 9 && out[4] <= 11 && out[7] == 255);
        CHECK(HalfToFloat(floatToHalf(0.5f)) == 0.5f && HalfToFloat(0x3c00) == 1.f && HalfToFloat(0x0001) > 0.f);
    }
    // rejections
    std::vector<uint8_t> out;
    CHECK(!ConvertToBgra(DXGI_FORMAT_R11G11B10_FLOAT, rgba.data(), rgba.size(), pitch, w, h, out));
    CHECK(!ConvertToBgra(DXGI_FORMAT_R8G8B8A8_UNORM, rgba.data(), rgba.size(), 8, w, h, out));            // pitch < row
    CHECK(!ConvertToBgra(DXGI_FORMAT_R8G8B8A8_UNORM, rgba.data(), pitch + 11, pitch, w, h, out));         // source too small
    CHECK(ConvertToBgra(DXGI_FORMAT_R8G8B8A8_UNORM, rgba.data(), pitch + 12, pitch, w, h, out));          // last row needs no padding
    CHECK(!ConvertToBgra(DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 100, pitch, w, h, out));
    CHECK(!ConvertToBgra(DXGI_FORMAT_R8G8B8A8_UNORM, rgba.data(), rgba.size(), pitch, 0, h, out));
}

// ---------------------------------------------------------------------------------------------
static void testDibAndClipboard() {
    std::printf("[dib + clipboard]\n");
    using namespace capture;
    const uint32_t w = 5, h = 3;   // 5*3 = 15 bytes -> stride 16 (padding)
    std::vector<uint8_t> bgra(w * h * 4);
    for (uint32_t i = 0; i < w * h; i++) { bgra[i * 4] = uint8_t(i); bgra[i * 4 + 1] = uint8_t(100 + i); bgra[i * 4 + 2] = uint8_t(200 + i); bgra[i * 4 + 3] = 255; }
    std::vector<uint8_t> dib;
    CHECK(BuildDib24(bgra.data(), w, h, dib));
    CHECK(DibStride24(5) == 16 && DibStride24(4) == 12 && DibStride24(1) == 4);
    CHECK(dib.size() == sizeof(BITMAPINFOHEADER) + 16 * 3);
    BITMAPINFOHEADER hd;
    std::memcpy(&hd, dib.data(), sizeof(hd));
    CHECK(hd.biSize == 40 && hd.biWidth == 5 && hd.biHeight == 3 && hd.biPlanes == 1 && hd.biBitCount == 24 &&
          hd.biCompression == BI_RGB && hd.biSizeImage == 48);
    // bottom-up: DIB row 0 = image row 2
    const uint8_t* bits = dib.data() + 40;
    bool ok = true;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* d = bits + y * 16 + x * 3;
            const uint8_t* s = &bgra[((h - 1 - y) * w + x) * 4];
            ok = ok && d[0] == s[0] && d[1] == s[1] && d[2] == s[2];
        }
    CHECK(ok);
    CHECK(bits[15] == 0 && bits[31] == 0 && bits[47] == 0);   // padding zeroed
    CHECK(!BuildDib24(nullptr, w, h, dib));
    CHECK(!BuildDib24(bgra.data(), 0, h, dib));

    // Real clipboard round trip (the user's text clipboard is saved and restored).
    std::wstring savedText;
    bool hadText = false;
    if (OpenClipboard(nullptr)) {
        if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) if (auto p = static_cast<const wchar_t*>(GlobalLock(t))) { savedText = p; hadText = true; GlobalUnlock(t); }
        CloseClipboard();
    }
    std::vector<uint8_t> dib2;
    CHECK(BuildDib24(bgra.data(), w, h, dib2));
    const char* why = nullptr;
    CHECK(PutDibOnClipboard(nullptr, dib2, &why));
    bool readOk = false;
    if (OpenClipboard(nullptr)) {
        CHECK(IsClipboardFormatAvailable(CF_DIB) != FALSE);
        if (HANDLE d = GetClipboardData(CF_DIB)) {
            const SIZE_T size = GlobalSize(d);
            if (auto p = static_cast<const uint8_t*>(GlobalLock(d))) {
                readOk = size >= dib2.size() && std::memcmp(p, dib2.data(), dib2.size()) == 0;
                GlobalUnlock(d);
            }
        }
        CloseClipboard();
    }
    CHECK(readOk);
    // Clipboard held open by another window -> retries then reports busy (never blocks long)
    HWND holder = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CHECK(holder != nullptr);
    if (holder && OpenClipboard(holder)) {
        HANDLE blocker = CreateThread(nullptr, 0, [](void* arg) -> DWORD {
            const char* w2 = nullptr;
            auto* d = static_cast<std::vector<uint8_t>*>(arg);
            return PutDibOnClipboard(nullptr, *d, &w2, 3, 5) ? 1 : 0;
        }, &dib2, 0, nullptr);
        DWORD code = 99;
        if (blocker) { WaitForSingleObject(blocker, 5000); GetExitCodeThread(blocker, &code); CloseHandle(blocker); }
        CHECK(code == 0);
        CloseClipboard();
    }
    if (holder) DestroyWindow(holder);
    if (hadText && OpenClipboard(nullptr)) {
        EmptyClipboard();
        const size_t bytes = (savedText.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            std::memcpy(GlobalLock(g), savedText.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }
}

// ---------------------------------------------------------------------------------------------
static void testNamingAndPng() {
    std::printf("[naming + png]\n");
    using namespace capture;
    SYSTEMTIME t = {};
    t.wYear = 2026; t.wMonth = 10; t.wDay = 29; t.wHour = 9; t.wMinute = 5; t.wSecond = 7;
    CHECK(ShotBaseName(t) == L"EchoSelfie_20261029_090507");
    // default folder = the Pictures KNOWN folder (OneDrive-redirected or not) + \Echo Selfies
    {
        PWSTR known = nullptr;
        std::wstring expected;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &known)) && known) expected = known;
        CoTaskMemFree(known);
        CHECK(!expected.empty());
        CHECK(PicturesFolder() == expected);
        CHECK(DefaultShotDir() == expected + L"\\Echo Selfies");
        std::printf("  default shot folder: %ls\n", DefaultShotDir().c_str());
    }

    const std::wstring root = tempDir(L"naming");
    removeTree(root);
    const std::wstring dir = root + L"\\Pictures\\Echo Selfies";
    CHECK(EnsureDirectory(dir));
    CHECK(EnsureDirectory(dir));   // idempotent
    std::wstring a, b, c;
    CHECK(ReserveShotPath(dir, t, a));
    CHECK(ReserveShotPath(dir, t, b));
    CHECK(ReserveShotPath(dir, t, c));
    CHECK(a == dir + L"\\EchoSelfie_20261029_090507.png");
    CHECK(b == dir + L"\\EchoSelfie_20261029_090507_2.png");
    CHECK(c == dir + L"\\EchoSelfie_20261029_090507_3.png");
    // an existing file is never overwritten
    {
        HANDLE f = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD wrote = 0;
        WriteFile(f, "keep", 4, &wrote, nullptr);
        CloseHandle(f);
        std::wstring d;
        CHECK(ReserveShotPath(dir, t, d) && d == dir + L"\\EchoSelfie_20261029_090507_4.png");
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        CHECK(GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 4);
    }
    CHECK(!EnsureDirectory(a));   // a file is not a directory

    // PNG round trip (odd width)
    const uint32_t w = 7, h = 5;
    std::vector<uint8_t> img(w * h * 4);
    for (uint32_t i = 0; i < w * h; i++) { img[i * 4] = uint8_t(i * 3); img[i * 4 + 1] = uint8_t(255 - i); img[i * 4 + 2] = uint8_t(i * 7); img[i * 4 + 3] = 255; }
    const char* why = nullptr;
    CHECK(WritePng(b, img.data(), w, h, &why));
    if (why) std::printf("  png why=%s\n", why);
    std::vector<uint8_t> back;
    UINT rw = 0, rh = 0;
    CHECK(readPng(b, back, rw, rh));
    CHECK(rw == w && rh == h && back == img);
    CHECK(!WritePng(root + L"\\no\\such\\dir\\x.png", img.data(), w, h, &why) && why);
    removeTree(root);
}

// ---------------------------------------------------------------------------------------------
static void testStateMachine() {
    std::printf("[state machine]\n");
    capture::ShotState s;
    CHECK(s.Get() == capture::ShotState::Idle);
    CHECK(!s.BeginRecord() && !s.Submitted() && !s.BeginProcess());
    CHECK(s.Request(1000));
    CHECK(!s.Request(1001));                     // one in flight
    CHECK(!s.Expire(3999, 3000));                // not yet
    CHECK(s.BeginRecord());
    CHECK(!s.Expire(99999, 3000));               // a recording shot never expires
    CHECK(!s.BeginRecord());
    CHECK(s.Submitted());
    CHECK(!s.Request(2000));
    CHECK(s.BeginProcess());
    CHECK(!s.AbortInFlight());
    s.Finish();
    CHECK(s.Get() == capture::ShotState::Idle);
    CHECK(s.Request(5000) && s.BeginRecord() && s.AbortRecord() && s.Get() == capture::ShotState::Idle);
    CHECK(s.Request(6000) && s.BeginRecord() && s.Submitted() && s.AbortInFlight() && s.Get() == capture::ShotState::Idle);
    CHECK(s.Request(7000) && s.Expire(10000, 3000) && s.Get() == capture::ShotState::Idle);
    CHECK(!s.BeginRecord());                     // the expired request cannot be picked up afterwards
}

// ---------------------------------------------------------------------------------------------
struct Gpu {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    UINT64 serial = 0;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool init() {
        ComPtr<ID3D12Debug> debug;
        if (!GetEnvironmentVariableW(L"CAPTURE_NO_DEBUG", nullptr, 0) && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    void begin() { alloc->Reset(); list->Reset(alloc.Get(), nullptr); }
    void flush() {
        queue->Signal(fence.Get(), ++serial);
        fence->SetEventOnCompletion(serial, event);
        WaitForSingleObject(event, 10000);
    }
    void submitAndWait() {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        queue->Signal(fence.Get(), ++serial);
        fence->SetEventOnCompletion(serial, event);
        WaitForSingleObject(event, 10000);
    }
    UINT64 debugErrors() {
        if (!info) return 0;
        UINT64 errors = 0;
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; i++) {
            SIZE_T len = 0;
            info->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                errors++;
                std::printf("  d3d12 debug: %s\n", m->pDescription);
            }
        }
        info->ClearStoredMessages();
        return errors;
    }
};

// Creates a texture in PRESENT (COMMON) state holding `data` (tightly packed rows of `bpp` bytes).
static ComPtr<ID3D12Resource> makeTexture(Gpu& g, DXGI_FORMAT format, UINT w, UINT h, const std::vector<uint8_t>& data, UINT bpp) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> tex, upload;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return nullptr;
    uint8_t* p = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&p));
    std::memset(p, 0xCD, size_t(total));   // garbage in the row padding
    for (UINT y = 0; y < h; y++) std::memcpy(p + fp.Offset + size_t(fp.Footprint.RowPitch) * y, &data[size_t(y) * w * bpp], size_t(w) * bpp);
    upload->Unmap(0, nullptr);
    g.begin();
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT};
    g.list->ResourceBarrier(1, &b);
    g.submitAndWait();
    return tex;
}

static bool waitCopier(capture::ReadbackCopier& c, HANDLE ev) {
    if (c.Idle()) return true;
    c.Fence()->SetEventOnCompletion(c.Serial(), ev);
    return WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0 && c.Idle();
}

static void testCopier(Gpu& g) {
    std::printf("[readback copier on WARP]\n");
    struct Case { DXGI_FORMAT f; UINT bpp; UINT w, h; };
    const Case cases[] = {
        {DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 37, 19},     // Echo's observed desktop mirror format; odd width -> pitch padding
        {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 64, 8},
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 3, 3},
        {DXGI_FORMAT_B8G8R8A8_UNORM, 4, 50, 30},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4, 17, 2},
        {DXGI_FORMAT_B8G8R8A8_TYPELESS, 4, 9, 9},
        {DXGI_FORMAT_R10G10B10A2_UNORM, 4, 21, 6},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 13, 7},
    };
    capture::ReadbackCopier copier;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int allocations = 0;
    for (const Case& k : cases) {
        std::vector<uint8_t> data(size_t(k.w) * k.h * k.bpp);
        for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t((i * 37 + k.w) & 0xff);
        if (k.f == DXGI_FORMAT_R16G16B16A16_FLOAT)
            for (size_t i = 0; i < data.size() / 2; i++) { const uint16_t hv = floatToHalf(float(i % 11) / 10.f); std::memcpy(&data[i * 2], &hv, 2); }
        ComPtr<ID3D12Resource> tex = makeTexture(g, k.f, k.w, k.h, data, k.bpp);
        CHECK(tex != nullptr);
        if (!tex) continue;
        const auto r = copier.Record(g.queue.Get(), tex.Get(), D3D12_RESOURCE_STATE_PRESENT);
        CHECK(r == capture::ReadbackCopier::Submitted);
        if (r != capture::ReadbackCopier::Submitted) { std::printf("  format %d: %s\n", int(k.f), copier.error ? copier.error : "?"); continue; }
        CHECK(waitCopier(copier, ev));
        std::vector<uint8_t> got, want;
        uint32_t w = 0, h = 0;
        CHECK(copier.ReadPixels(got, w, h));
        CHECK(w == k.w && h == k.h);
        CHECK(capture::ConvertToBgra(k.f, data.data(), data.size(), k.w * k.bpp, k.w, k.h, want));
        CHECK(got == want);
        if (got != want) std::printf("  pixel mismatch for format %d\n", int(k.f));
        // the texture is back in PRESENT: a PRESENT -> COPY_SOURCE barrier of ours again is valid (debug layer checks it)
        CHECK(copier.Record(g.queue.Get(), tex.Get()) == capture::ReadbackCopier::Submitted);
        CHECK(waitCopier(copier, ev));
        allocations = copier.BufferAllocations();
    }
    CHECK(allocations >= 2 && allocations < int(sizeof(cases) / sizeof(cases[0])) * 2);   // lazily (re)sized, not per shot
    // grow: a much larger texture forces a bigger buffer
    {
        const UINT64 before = copier.BufferBytes();
        std::vector<uint8_t> data(300 * 200 * 4, 0x5A);
        ComPtr<ID3D12Resource> big = makeTexture(g, DXGI_FORMAT_B8G8R8A8_UNORM, 300, 200, data, 4);
        CHECK(copier.Record(g.queue.Get(), big.Get()) == capture::ReadbackCopier::Submitted);
        CHECK(copier.BufferBytes() > before && copier.BufferBytes() >= 300ull * 4 * 200);
        CHECK(waitCopier(copier, ev));
        std::vector<uint8_t> got; uint32_t w, h;
        CHECK(copier.ReadPixels(got, w, h) && w == 300 && h == 200 && got[0] == 0x5A && got[3] == 255);
    }
    // fence-gated: while our previous copy has not retired, Record refuses (Busy) and ReadPixels refuses
    {
        ComPtr<ID3D12Fence> gate;
        g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
        std::vector<uint8_t> data(8 * 8 * 4, 1);
        ComPtr<ID3D12Resource> tex = makeTexture(g, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, data, 4);
        g.queue->Wait(gate.Get(), 1);   // stall the queue
        CHECK(copier.Record(g.queue.Get(), tex.Get()) == capture::ReadbackCopier::Submitted);
        CHECK(!copier.Idle());
        CHECK(copier.Record(g.queue.Get(), tex.Get()) == capture::ReadbackCopier::Busy);
        std::vector<uint8_t> got; uint32_t w, h;
        CHECK(!copier.ReadPixels(got, w, h));
        gate->Signal(1);                 // release
        CHECK(waitCopier(copier, ev));
        CHECK(copier.ReadPixels(got, w, h) && w == 8);
    }
    // unsupported source formats / shapes are refused without submitting
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R11G11B10_FLOAT; td.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> odd;
        g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&odd));
        const UINT64 serial = copier.Serial();
        CHECK(odd && copier.Record(g.queue.Get(), odd.Get()) == capture::ReadbackCopier::Unsupported && copier.Serial() == serial);
        CHECK(copier.Record(nullptr, odd.Get()) == capture::ReadbackCopier::Unsupported);
    }
    CloseHandle(ev);
    CHECK(g.debugErrors() == 0);
}

// ---------------------------------------------------------------------------------------------
struct TestSwap {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12DescriptorHeap> rtv;
    bool create(Gpu& g, UINT w, UINT h, DXGI_FORMAT format) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SelfieCaptureTestWindow";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"capture test", WS_OVERLAPPEDWINDOW, 0, 0, int(w) + 16, int(h) + 39, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w; sd.Height = h; sd.Format = format; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> c1;
        if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &sd, nullptr, nullptr, &c1)) || FAILED(c1.As(&chain))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        return SUCCEEDED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)));
    }
    // Echo-like frame: PRESENT -> RT, clear, RT -> PRESENT on the swap chain's queue.
    void clear(Gpu& g, const float color[4]) {
        ComPtr<ID3D12Resource> bb;
        chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        g.device->CreateRenderTargetView(bb.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
        g.begin();
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {bb.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
        g.list->ResourceBarrier(1, &b);
        g.list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g.list->ResourceBarrier(1, &b);
        g.submitAndWait();
    }
    ~TestSwap() { chain.Reset(); if (hwnd) DestroyWindow(hwnd); }
};

static bool clipboardPixel(uint32_t x, uint32_t y, uint8_t bgr[3], LONG* w = nullptr, LONG* h = nullptr) {
    bool ok = false;
    for (int i = 0; i < 20 && !OpenClipboard(nullptr); i++) Sleep(10);
    if (HANDLE d = GetClipboardData(CF_DIB)) {
        if (auto p = static_cast<const uint8_t*>(GlobalLock(d))) {
            BITMAPINFOHEADER hd; std::memcpy(&hd, p, sizeof(hd));
            if (w) *w = hd.biWidth;
            if (h) *h = hd.biHeight;
            if (hd.biBitCount == 24 && LONG(x) < hd.biWidth && LONG(y) < hd.biHeight) {
                const uint8_t* px = p + hd.biSize + capture::DibStride24(hd.biWidth) * (hd.biHeight - 1 - y) + x * 3;
                bgr[0] = px[0]; bgr[1] = px[1]; bgr[2] = px[2];
                ok = true;
            }
            GlobalUnlock(d);
        }
    }
    CloseClipboard();
    return ok;
}

static void testEndToEnd(Gpu& g) {
    std::printf("[end to end: Present vtable hook on a WARP swap chain]\n");
    CHECK(!selfie_capture::RequestShot());     // not installed
    TestSwap a, other;
    if (!a.create(g, 64, 48, DXGI_FORMAT_B8G8R8A8_UNORM) || !other.create(g, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        CHECK(!"could not create a WARP swap chain on a hidden window");
        return;
    }
    // DXGI does not expose the presenting queue: GetDevice on a D3D12 swap chain returns the device only.
    {
        ComPtr<ID3D12CommandQueue> q;
        ComPtr<ID3D12Device> d;
        CHECK(FAILED(a.chain->GetDevice(IID_PPV_ARGS(&q))) && SUCCEEDED(a.chain->GetDevice(IID_PPV_ARGS(&d))));
    }
    const std::wstring out = tempDir(L"shots");
    removeTree(out);
    selfie_capture::testing::SetOutputDir(out.c_str());
    selfie_capture::testing::SetRequestTimeoutMs(400);
    CHECK(selfie_capture::testing::InstallOn(a.chain.Get()));
    CHECK(selfie_capture::testing::InstallOn(a.chain.Get()));     // idempotent (same vtable is not re-patched)
    const float red[4] = {1.f, 0.f, 0.f, 0.f}, teal[4] = {0.2f, 0.4f, 0.6f, 0.f};

    // 1) Echo's queue not known yet -> the shot is abandoned, the game frame still presents, nothing disabled
    g_testQueue = nullptr;
    a.clear(g, red);
    CHECK(selfie_capture::RequestShot());
    CHECK(SUCCEEDED(a.chain->Present(0, 0)));
    CHECK(selfie_capture::testing::WaitIdle(2000));
    CHECK(std::strstr(selfie_capture::Status(), "echo-queue-unknown") != nullptr);
    CHECK(selfie_capture::ShotsTaken() == 0 && !selfie_capture::testing::Disabled() && selfie_capture::testing::GpuFailures() == 0);

    // 2) a real shot
    g_testQueue = g.queue.Get();
    a.clear(g, teal);
    CHECK(selfie_capture::RequestShot());
    CHECK(!selfie_capture::RequestShot());     // already pending
    CHECK(SUCCEEDED(a.chain->Present(0, 0)));
    CHECK(selfie_capture::testing::WaitIdle(10000));
    CHECK(selfie_capture::ShotsTaken() == 1);
    std::printf("  %s\n", selfie_capture::Status());
    std::vector<std::wstring> files = listPng(out);
    CHECK(files.size() == 1);
    if (files.size() == 1) {
        std::vector<uint8_t> px; UINT w = 0, h = 0;
        CHECK(readPng(files[0], px, w, h));
        CHECK(w == 64 && h == 48);
        bool ok = !px.empty();
        for (size_t i = 0; i + 3 < px.size(); i += 4) ok = ok && px[i] == 153 && px[i + 1] == 102 && px[i + 2] == 51 && px[i + 3] == 255;
        CHECK(ok);
        CHECK(files[0].find(L"\\EchoSelfie_") != std::wstring::npos);
    }
    uint8_t bgr[3] = {};
    LONG cw = 0, ch = 0;
    CHECK(clipboardPixel(10, 40, bgr, &cw, &ch) && bgr[0] == 153 && bgr[1] == 102 && bgr[2] == 51 && cw == 64 && ch == 48);

    std::printf("  shot 1 checked (png + clipboard)\n");
    // 3) filtering: another swap chain (same patched vtable) and DXGI_PRESENT_TEST never capture
    CHECK(selfie_capture::RequestShot());
    const float black[4] = {0, 0, 0, 1};
    other.clear(g, black);
    other.chain->Present(0, 0);
    a.chain->Present(0, DXGI_PRESENT_TEST);
    Sleep(50);
    CHECK(selfie_capture::ShotsTaken() == 1);
    // 4) ...and with no desktop Present the request is dropped after the timeout (core can retry)
    CHECK(selfie_capture::testing::WaitIdle(3000));
    CHECK(std::strstr(selfie_capture::Status(), "no-desktop-present") != nullptr);
    std::printf("  filtering + timeout checked\n");
    CHECK(selfie_capture::RequestShot());     // accepted again
    a.clear(g, red);
    CHECK(SUCCEEDED(a.chain->Present(0, 0)));
    CHECK(selfie_capture::testing::WaitIdle(10000));
    CHECK(selfie_capture::ShotsTaken() == 2);
    CHECK(clipboardPixel(0, 0, bgr) && bgr[0] == 0 && bgr[1] == 0 && bgr[2] == 255);

    // 5) ResizeBuffers (backbuffer size change): the readback buffer follows
    g.flush();   // like any app, idle the queue before ResizeBuffers (queued Presents still reference the buffers)
    CHECK(SUCCEEDED(a.chain->ResizeBuffers(0, 160, 90, DXGI_FORMAT_UNKNOWN, 0)));
    a.clear(g, teal);
    CHECK(selfie_capture::RequestShot());
    CHECK(SUCCEEDED(a.chain->Present(0, 0)));
    CHECK(selfie_capture::testing::WaitIdle(10000));
    CHECK(selfie_capture::ShotsTaken() == 3);
    CHECK(clipboardPixel(159, 89, bgr, &cw, &ch) && cw == 160 && ch == 90 && bgr[0] == 153 && bgr[2] == 51);
    files = listPng(out);
    CHECK(files.size() == 3);   // never overwritten, even within the same second
    // 6) many presents with no request do nothing and stay fast
    for (int i = 0; i < 20; i++) a.chain->Present(0, 0);
    CHECK(selfie_capture::ShotsTaken() == 3 && selfie_capture::testing::GpuFailures() == 0 && !selfie_capture::testing::Disabled());
    // 7) EchoQueue() on a different device than the backbuffer: refused, the frame presents, capture stays enabled.
    //    (D3D12 keeps one device per adapter, so the other device is a hardware adapter's; skipped if there is none.)
    {
        ComPtr<IDXGIAdapter1> hw;
        ComPtr<ID3D12Device> dev2;
        ComPtr<ID3D12CommandQueue> q2;
        for (UINT i = 0; g.factory->EnumAdapters1(i, &hw) != DXGI_ERROR_NOT_FOUND; i++, hw.Reset()) {
            DXGI_ADAPTER_DESC1 ad;
            hw->GetDesc1(&ad);
            if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(hw.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev2)))) break;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (dev2 && dev2.Get() != g.device.Get() && SUCCEEDED(dev2->CreateCommandQueue(&qd, IID_PPV_ARGS(&q2)))) {
            g_testQueue = q2.Get();
            a.clear(g, red);
            CHECK(selfie_capture::RequestShot());
            CHECK(SUCCEEDED(a.chain->Present(0, 0)));
            CHECK(selfie_capture::testing::WaitIdle(2000));
            CHECK(selfie_capture::ShotsTaken() == 3 && !selfie_capture::testing::Disabled() && selfie_capture::testing::GpuFailures() == 0);
            CHECK(std::strstr(selfie_capture::Status(), "device") != nullptr);
            std::printf("  other-device queue -> %s\n", selfie_capture::Status());
        } else {
            std::printf("  (other-device case skipped: no hardware D3D12 adapter)\n");
        }
        g_testQueue = g.queue.Get();
    }
    std::printf("  %s\n", selfie_capture::Status());
    g.flush();
    CHECK(g.debugErrors() == 0);
    removeTree(out);
    removeTree(tempDir(L""));
}

static Gpu* g_gpu = nullptr;
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* e) {
    if (g_gpu) g_gpu->debugErrors();
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(e->ExceptionRecord->ExceptionAddress), &m);
    wchar_t name[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, name, MAX_PATH);
    std::printf("CRASH: code 0x%08lx at %ls+0x%llx\n", e->ExceptionRecord->ExceptionCode, name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(m)));
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashFilter);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    testConversion();
    testDibAndClipboard();
    testNamingAndPng();
    testStateMachine();
    Gpu g;
    g_gpu = &g;
    if (!g.init()) { std::printf("FAIL: no WARP D3D12 device\n"); return 1; }
    std::printf("(d3d12 debug layer: %s)\n", g.info ? "on" : "unavailable");
    testCopier(g);
    testEndToEnd(g);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
