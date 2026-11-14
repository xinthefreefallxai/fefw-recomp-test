// SPDX-License-Identifier: GPL-3.0-or-later
// caps:su (IScreenShotApplicationService): the photos the game saves are written as PNG files into the album folder ([album] dir,
// default "album" next to the exe).
//
// The path from the game (Pokopia main, sdk module from the same title, read in IDA):
//   nn::album::SaveScreenshot(const void* rgba, size_t size, ImageSize, AlbumReportOption)    sdk 0x1a2160
//     -> nn::capsrv::SaveScreenShotForApplication(entry*, data, size, ScreenShotAttribute&, ApplicationData&, option)  sdk 0x28182c
//     -> caps:su (InitializeForApplication, sdk 0x2815f8) cmd 205 SaveScreenShotEx1 (proxy sdk 0x281d40 / 0x2821ec).
// ImageSize 0 is 1280x720 and 1 is 1920x1080 (nn::album::GetImageWidth / GetImageHeight, sdk 0x1a0ab0 / 0x1a0ae0); the data is linear
// RGBA8, at least 4 * width * height bytes (SaveScreenshot aborts otherwise). Cmd 205 request: raw ScreenShotAttribute (0x40 bytes, u32
// ImageSize first), u32 AlbumReportOption at 0x40, u64 AppletResourceUserId at 0x48; buffer 0 the ApplicationData (0x404 bytes on
// Switch 2, 0x400 on Switch 1), buffer 1 the image (both MapAlias in-buffers). Reply: the 0x20-byte ApplicationAlbumEntry. Cmds 203
// (Ex0, no ApplicationData) and 210 (Ex2, a user id list) share the raw layout and take the image as their largest buffer.
// ApplicationAlbumEntry layout from libnx caps.h (CapsApplicationAlbumEntry v1: u64 size, u64 hash, AlbumFileDateTime, storage,
// content, pad, unk_x1f = 1). The PNG is encoded by the Windows Imaging Component.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "services_internal.h"

#include <windows.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

namespace nxrt::services {

namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;

[[maybe_unused]] constexpr size_t kAttributeSize = 0x40;
constexpr size_t kOptionOffset = 0x40;
constexpr size_t kEntrySize = 0x20;
constexpr uint8_t kStorageSd = 1;       // CapsAlbumStorage_Sd
constexpr uint8_t kContentScreenshot = 0;

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

std::filesystem::path album_dir() {
    const std::string dir = config_get_str("album", "dir", "album");     // UTF-8
    std::filesystem::path d(std::u8string(dir.begin(), dir.end()));
    if (d.empty()) d = "album";
    return d.is_absolute() ? d : exe_dir() / d;
}

bool album_write_png(const std::filesystem::path& file, const uint8_t* rgba, uint32_t width, uint32_t height, std::string* err) {
    // 24-bit BGR rows (the album keeps no alpha; the game's alpha channel is whatever its render target held).
    const size_t stride = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> bgr(stride * height);
    for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) {
        bgr[i * 3 + 0] = rgba[i * 4 + 2];
        bgr[i * 3 + 1] = rgba[i * 4 + 1];
        bgr[i * 3 + 2] = rgba[i * 4 + 0];
    }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // S_FALSE / RPC_E_CHANGED_MODE: COM is already usable here
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    const char* step = "CoCreateInstance(WICImagingFactory)";
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) { step = "CreateStream"; hr = factory->CreateStream(&stream); }
    if (SUCCEEDED(hr)) { step = "InitializeFromFilename"; hr = stream->InitializeFromFilename(file.wstring().c_str(), GENERIC_WRITE); }
    if (SUCCEEDED(hr)) { step = "CreateEncoder(PNG)"; hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder); }
    if (SUCCEEDED(hr)) { step = "Encoder Initialize"; hr = encoder->Initialize(stream, WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { step = "CreateNewFrame"; hr = encoder->CreateNewFrame(&frame, &props); }
    if (SUCCEEDED(hr)) { step = "Frame Initialize"; hr = frame->Initialize(props); }
    if (SUCCEEDED(hr)) { step = "SetSize"; hr = frame->SetSize(width, height); }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) { step = "SetPixelFormat"; hr = frame->SetPixelFormat(&fmt); }
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) { step = "SetPixelFormat (24bpp BGR refused)"; hr = E_FAIL; }
    if (SUCCEEDED(hr)) {
        step = "WritePixels";
        hr = frame->WritePixels(height, static_cast<UINT>(stride), static_cast<UINT>(bgr.size()), bgr.data());
    }
    if (SUCCEEDED(hr)) { step = "Frame Commit"; hr = frame->Commit(); }
    if (SUCCEEDED(hr)) { step = "Encoder Commit"; hr = encoder->Commit(); }
    release(props);
    release(frame);
    release(encoder);
    release(stream);
    release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) {
        if (err) {
            char b[96];
            std::snprintf(b, sizeof b, "%s failed (HRESULT 0x%08lX)", step, static_cast<unsigned long>(hr));
            *err = b;
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return false;
    }
    return true;
}

namespace {

// The image of a SaveScreenShotEx* request: the largest in-buffer.
ipc::Buffer image_buffer(const Request& req) {
    ipc::Buffer best;
    for (size_t i = 0; i < 4; ++i) {
        const ipc::Buffer b = req.in_buf(i);
        if (b.addr && b.size > best.size) best = b;
    }
    return best;
}

uint32_t save_screenshot(Request& req, Response& rsp) {
    static std::mutex save_mu;   // two saves in the same second must not pick the same file name
    const uint32_t image_size = req.arg<uint32_t>(0);
    const uint32_t option = req.arg<uint32_t>(kOptionOffset);
    const ipc::Buffer img = image_buffer(req);
    uint32_t w = 1280, h = 720;
    if (image_size == 1 || (image_size >= 2 && img.size >= 1920ull * 1080 * 4)) {
        w = 1920;
        h = 1080;
    }
    uint8_t entry[kEntrySize] = {};
    if (!img.addr || img.size < static_cast<uint64_t>(w) * h * 4) {
        NXLOG_W("caps", "SaveScreenShot (cmd %u): image buffer of %llu bytes is too small for %ux%u RGBA8: not saved", req.cmd,
                static_cast<unsigned long long>(img.size), w, h);
        rsp.out.insert(rsp.out.end(), entry, entry + sizeof entry);
        return 0;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm lt{};
    localtime_s(&lt, &t);

    std::lock_guard<std::mutex> lk(save_mu);
    const std::filesystem::path dir = album_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char base[64];
    std::snprintf(base, sizeof base, "%04d-%02d-%02d_%02d-%02d-%02d-%03d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
                  lt.tm_min, lt.tm_sec, ms);
    std::filesystem::path file = dir / (std::string(base) + ".png");
    for (int i = 2; std::filesystem::exists(file, ec) && i < 100; ++i) file = dir / (std::string(base) + "_" + std::to_string(i) + ".png");

    std::string why;
    if (!album_write_png(file, reinterpret_cast<const uint8_t*>(img.addr), w, h, &why)) {
        // Reported as saved: a failure would make the game show the error applet for what is a host disk problem.
        NXLOG_E("caps", "SaveScreenShot (cmd %u): writing %s failed: %s", req.cmd, reinterpret_cast<const char*>(file.u8string().c_str()), why.c_str());
        rsp.out.insert(rsp.out.end(), entry, entry + sizeof entry);
        return 0;
    }
    const uint64_t fsize = std::filesystem::file_size(file, ec);
    NXLOG_I("caps", "SaveScreenShot (cmd %u): %ux%u photo saved to %s (%llu bytes, report option 0x%x)", req.cmd, w, h,
            reinterpret_cast<const char*>(file.u8string().c_str()), static_cast<unsigned long long>(ec ? 0 : fsize), option);

    // ApplicationAlbumEntry: u64 size, u64 hash (none kept: 0), AlbumFileDateTime {u16 year, u8 month, day, hour, minute, second, id},
    // u8 storage, u8 content, 5 bytes pad, u8 1.
    const uint64_t size64 = ec ? 0 : fsize;
    std::memcpy(entry + 0, &size64, 8);
    const uint16_t year = static_cast<uint16_t>(lt.tm_year + 1900);
    std::memcpy(entry + 0x10, &year, 2);
    entry[0x12] = static_cast<uint8_t>(lt.tm_mon + 1);
    entry[0x13] = static_cast<uint8_t>(lt.tm_mday);
    entry[0x14] = static_cast<uint8_t>(lt.tm_hour);
    entry[0x15] = static_cast<uint8_t>(lt.tm_min);
    entry[0x16] = static_cast<uint8_t>(lt.tm_sec);
    entry[0x17] = 0;
    entry[0x18] = kStorageSd;
    entry[0x19] = kContentScreenshot;
    entry[0x1F] = 1;
    rsp.out.insert(rsp.out.end(), entry, entry + sizeof entry);
    return 0;
}

}  // namespace

std::shared_ptr<Service> make_screenshot_app() {
    return make_iface("IScreenShotApplicationService", [](Service& s) {
        s.add_cmd(32, "SetShimLibraryVersion", [](Request&, Response&) -> uint32_t {
            NXLOG_D("caps", "SetShimLibraryVersion (caps:su): accepted");
            return 0;
        });
        s.add_cmd(203, "SaveScreenShotEx0", save_screenshot);
        s.add_cmd(205, "SaveScreenShotEx1", save_screenshot);
        s.add_cmd(210, "SaveScreenShotEx2", save_screenshot);
    });
}

}  // namespace nxrt::services
