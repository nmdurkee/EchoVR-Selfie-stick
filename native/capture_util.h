// capture_util.h - pure helpers for the SelfieStick screenshot module (no GPU, no COM):
//   pixel conversion (desktop backbuffer formats -> opaque BGRA8 top-down),
//   CF_DIB construction, shot file naming, and the shot request state machine.
// Header-only; included by selfie_capture.cpp and capture_tests.cpp.
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <dxgiformat.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace capture {

// ---- pixel formats -----------------------------------------------------------
enum class PixelKind { Unsupported, RGBA8, BGRA8, BGRX8, RGB10A2, RGBA16F };

inline PixelKind Classify(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return PixelKind::RGBA8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return PixelKind::BGRA8;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return PixelKind::BGRX8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return PixelKind::RGB10A2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return PixelKind::RGBA16F;   // TYPELESS assumed FLOAT (the only 64-bit swapchain format)
    default:
        return PixelKind::Unsupported;
    }
}

inline uint32_t BytesPerPixel(PixelKind k) {
    switch (k) {
    case PixelKind::RGBA8: case PixelKind::BGRA8: case PixelKind::BGRX8: case PixelKind::RGB10A2: return 4;
    case PixelKind::RGBA16F: return 8;
    default: return 0;
    }
}

inline float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ffu, bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {  // subnormal: normalise
            exp = 127 - 15 + 1;
            while (!(man & 0x400u)) { man <<= 1; exp--; }
            bits = sign | (exp << 23) | ((man & 0x3ffu) << 13);
        }
    } else if (exp == 31) bits = sign | 0x7f800000u | (man << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Linear (scRGB) -> 8-bit sRGB, clamped. NaN -> 0.
inline uint8_t LinearToSrgb8(float v) {
    if (!(v > 0.f)) return 0;
    if (v >= 1.f) return 255;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    const int i = int(s * 255.f + 0.5f);
    return uint8_t(i < 0 ? 0 : i > 255 ? 255 : i);
}

// Converts a mapped readback footprint into tightly packed, top-down, opaque BGRA8.
// The 8-bit formats are copied byte-exact: an sRGB backbuffer already stores the
// encoded bytes the desktop shows, a UNORM or TYPELESS one stores whatever Echo wrote,
// which is what the window displays. R10G10B10A2 keeps the top 8 bits per channel;
// FP16 (scRGB) is clamped and sRGB-encoded. Alpha is always forced to 255.
inline bool ConvertToBgra(DXGI_FORMAT format, const uint8_t* src, size_t srcBytes, uint32_t rowPitch,
                          uint32_t width, uint32_t height, std::vector<uint8_t>& out, const char** why = nullptr) {
    auto fail = [&](const char* r) { if (why) *why = r; return false; };
    const PixelKind kind = Classify(format);
    const uint32_t bpp = BytesPerPixel(kind);
    if (!bpp) return fail("unsupported-format");
    if (!src || !width || !height || width > 16384 || height > 16384) return fail("bad-size");
    const uint64_t rowBytes = uint64_t(width) * bpp;
    if (rowPitch < rowBytes) return fail("row-pitch-too-small");
    if (uint64_t(rowPitch) * (height - 1) + rowBytes > srcBytes) return fail("source-too-small");
    out.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* s = src + size_t(rowPitch) * y;
        uint8_t* d = out.data() + size_t(width) * 4 * y;
        switch (kind) {
        case PixelKind::RGBA8:
            for (uint32_t x = 0; x < width; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
            break;
        case PixelKind::BGRA8: case PixelKind::BGRX8:
            for (uint32_t x = 0; x < width; x++, s += 4, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
            break;
        case PixelKind::RGB10A2:
            for (uint32_t x = 0; x < width; x++, s += 4, d += 4) {
                uint32_t v; std::memcpy(&v, s, 4);
                d[2] = uint8_t((v >> 2) & 0xff);          // R bits 0..9
                d[1] = uint8_t((v >> 12) & 0xff);         // G bits 10..19
                d[0] = uint8_t((v >> 22) & 0xff);         // B bits 20..29
                d[3] = 255;
            }
            break;
        case PixelKind::RGBA16F:
            for (uint32_t x = 0; x < width; x++, s += 8, d += 4) {
                uint16_t c[4]; std::memcpy(c, s, 8);
                d[2] = LinearToSrgb8(HalfToFloat(c[0]));
                d[1] = LinearToSrgb8(HalfToFloat(c[1]));
                d[0] = LinearToSrgb8(HalfToFloat(c[2]));
                d[3] = 255;
            }
            break;
        default:
            return fail("unsupported-format");
        }
    }
    if (why) *why = nullptr;
    return true;
}

// ---- CF_DIB ------------------------------------------------------------------
// 24-bit BI_RGB bottom-up DIB (the most widely accepted clipboard image: no alpha ambiguity).
inline uint32_t DibStride24(uint32_t width) { return (width * 3 + 3) & ~3u; }

inline bool BuildDib24(const uint8_t* bgra, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
    if (!bgra || !width || !height || width > 16384 || height > 16384) return false;
    const uint32_t stride = DibStride24(width);
    out.assign(sizeof(BITMAPINFOHEADER) + size_t(stride) * height, 0);
    BITMAPINFOHEADER h = {};
    h.biSize = sizeof(BITMAPINFOHEADER);
    h.biWidth = LONG(width);
    h.biHeight = LONG(height);      // positive: bottom-up
    h.biPlanes = 1;
    h.biBitCount = 24;
    h.biCompression = BI_RGB;
    h.biSizeImage = stride * height;
    h.biXPelsPerMeter = h.biYPelsPerMeter = 3780;  // 96 dpi
    std::memcpy(out.data(), &h, sizeof(h));
    uint8_t* bits = out.data() + sizeof(BITMAPINFOHEADER);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* s = bgra + size_t(width) * 4 * (height - 1 - y);
        uint8_t* d = bits + size_t(stride) * y;
        for (uint32_t x = 0; x < width; x++, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    return true;
}

// ---- shot files --------------------------------------------------------------
inline std::wstring ShotBaseName(const SYSTEMTIME& t) {
    wchar_t name[64];
    swprintf_s(name, L"EchoSelfie_%04u%02u%02u_%02u%02u%02u", unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay),
               unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));
    return name;
}

// The Pictures known folder (FOLDERID_Pictures: follows OneDrive / Group Policy redirection);
// %USERPROFILE%\Pictures only if that lookup fails. Empty string if neither is available.
inline std::wstring PicturesFolder() {
    PWSTR known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &known)) && known && known[0]) dir = known;
    CoTaskMemFree(known);
    if (!dir.empty()) return dir;
    wchar_t profile[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::wstring();
    return std::wstring(profile) + L"\\Pictures";
}
// <Pictures>\Echo Selfies (empty string if no Pictures folder could be determined).
inline std::wstring DefaultShotDir() {
    const std::wstring pictures = PicturesFolder();
    return pictures.empty() ? pictures : pictures + L"\\Echo Selfies";
}

// Creates every missing component of an absolute directory path.
inline bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    for (size_t i = 3; i <= dir.size(); i++) {   // skip "C:\"
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
            const std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    const DWORD b = GetFileAttributesW(dir.c_str());
    return b != INVALID_FILE_ATTRIBUTES && (b & FILE_ATTRIBUTE_DIRECTORY);
}

// Reserves a fresh file name by creating it empty with CREATE_NEW (never overwrites):
// <base>.png, then <base>_2.png, <base>_3.png ... up to _999.
inline bool ReserveShotPath(const std::wstring& dir, const SYSTEMTIME& t, std::wstring& out) {
    const std::wstring base = dir + L"\\" + ShotBaseName(t);
    for (int n = 1; n <= 999; n++) {
        std::wstring path = n == 1 ? base + L".png" : base + L"_" + std::to_wstring(n) + L".png";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); out = std::move(path); return true; }
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

// ---- request state machine ----------------------------------------------------
// Idle -> Requested (RequestShot, any thread)
// Requested -> Recording (Present hook) | Idle (worker: no desktop Present in time)
// Recording -> InFlight (copy submitted) | Idle (shot abandoned)
// InFlight -> Processing (worker: fence reached) | Idle (worker: GPU timeout)
// Processing -> Idle (worker: clipboard + PNG done)
class ShotState {
public:
    enum State : int { Idle = 0, Requested, Recording, InFlight, Processing };
    State Get() const { return State(state_.load()); }
    bool Request(uint64_t now) {
        int expected = Idle;
        if (!state_.compare_exchange_strong(expected, Requested)) return false;
        requestedAt_.store(now);
        return true;
    }
    bool BeginRecord() { return move(Requested, Recording); }
    bool Submitted() { return move(Recording, InFlight); }
    bool BeginProcess() { return move(InFlight, Processing); }
    void Finish() { state_.store(Idle); }
    bool AbortRecord() { return move(Recording, Idle); }
    bool AbortInFlight() { return move(InFlight, Idle); }
    // Worker: cancel a request no Present picked up within `timeoutMs`.
    bool Expire(uint64_t now, uint64_t timeoutMs) {
        if (Get() != Requested || now - requestedAt_.load() < timeoutMs) return false;
        return move(Requested, Idle);
    }
private:
    bool move(int from, int to) { return state_.compare_exchange_strong(from, to); }
    std::atomic<int> state_{Idle};
    std::atomic<uint64_t> requestedAt_{0};
};

}  // namespace capture
