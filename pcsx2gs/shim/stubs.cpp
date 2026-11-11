// What PCSX2's GS expects from the rest of the emulator, for running it on its
// own as GH1 Recomp's graphics backend. PCSX2's sources are used unmodified;
// this file stands in for the parts of PCSX2 that are not built:
//
//  - features GH1 Recomp leaves out on purpose: video capture, GS dumps,
//    texture replacement (a mod feature), on-screen overlays and menus;
//  - the VM, MTGS thread and host frontend: the runtime drives the GS itself,
//    on its own thread;
//  - the one piece of real work, executable memory for the software
//    renderer's JIT, reserved near the binary as PCSX2 does (its code makes
//    rel32 calls).
//
// Licence: GPL-3.0-or-later, as the code it links with.

#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "MTGS.h"
#include "Memory.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"
#include "Config.h"
#include "Counters.h"
#include "IPU/IPU.h"

#include "common/HostSys.h"
#include "common/Image.h"
#include "common/WindowInfo.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pcsx2gs_host.h"

// EmuConfig and EmuFolders come with Pcsx2Config.cpp.

// --- emulator state the GS asks about -----------------------------------------

bool FMVstarted = false;

const char *ReportVideoMode()
{
    return "NTSC";
}

const char *ReportInterlaceMode()
{
    return "Interlaced";
}

double GetVerticalFrequency()
{
    return 59.94;
}

namespace VMManager
{
    bool HasValidVM()
    {
        return true;
    }
    std::string GetDiscSerial()
    {
        return pcsx2gs::host().serial;
    }
    std::string GetTitle(bool)
    {
        return pcsx2gs::host().title;
    }
    u32 GetDiscCRC()
    {
        return pcsx2gs::host().crc;
    }
    namespace Internal
    {
        const std::vector<u32> &GetSoftwareRendererProcessorList()
        {
            static const std::vector<u32> none;
            return none;
        }
    }
}

namespace MTGS
{
    // The runtime calls the GS on its own thread: there is no MTGS queue.
    void WaitGS(bool, bool, bool) {}
    void RunOnGSThread(AsyncCallType func)
    {
        func();
    }
    void ApplySettings() {}
    void ToggleSoftwareRendering() {}
}

// --- host ------------------------------------------------------------------------

namespace Host
{
    std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window)
    {
        return pcsx2gs::host().acquireWindow ? pcsx2gs::host().acquireWindow(recreate_window) : std::nullopt;
    }
    void ReleaseRenderWindow()
    {
        if (pcsx2gs::host().releaseWindow)
            pcsx2gs::host().releaseWindow();
    }
    void BeginPresentFrame() {}
    // Fullscreen is SDL's borderless window, never a Direct3D exclusive mode,
    // so the swap chain is always made windowed.
    bool IsFullscreen()
    {
        return false;
    }
    void SetFullscreen(bool) {}
    void RunOnCPUThread(std::function<void()> function, bool)
    {
        if (function)
            function();
    }
    void AddIconOSDMessage(std::string, const char *, const std::string_view message, float)
    {
        std::fprintf(stderr, "[pcsx2 gs] %.*s\n", static_cast<int>(message.size()), message.data());
    }
    void AddKeyedOSDMessage(std::string, std::string message, float)
    {
        std::fprintf(stderr, "[pcsx2 gs] %s\n", message.c_str());
    }
    void ReportErrorAsync(const std::string_view title, const std::string_view message)
    {
        std::fprintf(stderr, "[pcsx2 gs] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(),
                     static_cast<int>(message.size()), message.data());
    }
    bool GetBaseBoolSettingValue(const char *, const char *, bool default_value)
    {
        return default_value;
    }
    std::string GetStringSettingValue(const char *, const char *, const char *default_value)
    {
        return default_value ? default_value : "";
    }
    std::string_view TranslateToStringView(const std::string_view, const std::string_view msg)
    {
        return msg;
    }
    std::string TranslateToString(const std::string_view, const std::string_view msg)
    {
        return std::string(msg);
    }
}

// --- left out: capture, dumps, replacements, overlays -----------------------------

namespace GSCapture
{
    bool BeginCapture(float, GSVector2i, float, std::string)
    {
        return false;
    }
    bool DeliverVideoFrame(GSTexture *)
    {
        return false;
    }
    void EndCapture() {}
    void Flush() {}
    std::string GetNextCaptureFileName()
    {
        return {};
    }
    GSVector2i GetSize()
    {
        return GSVector2i(0, 0);
    }
    bool IsCapturing()
    {
        return false;
    }
    bool IsCapturingVideo()
    {
        return false;
    }
}

namespace GSDumpReplayer
{
    bool IsReplayingDump()
    {
        return false;
    }
    bool IsRunner()
    {
        return false;
    }
    void RenderUI() {}
    void UpdateGSStats() {}
}

std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string &, const std::string &, u32, u32,
                                                                u32, const u32 *, const freezeData &,
                                                                const GSPrivRegSet *)
{
    return {};
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string &, const std::string &, u32, u32, u32,
                                                      const u32 *, const freezeData &, const GSPrivRegSet *)
{
    return {};
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string &, const std::string &, u32, u32, u32,
                                                       const u32 *, const freezeData &, const GSPrivRegSet *)
{
    return {};
}
void GSDumpBase::ReadFIFO(u32) {}
void GSDumpBase::Transfer(int, const u8 *, size_t) {}
bool GSDumpBase::VSync(int, bool, const GSPrivRegSet *)
{
    return false;
}

namespace GSPng
{
    bool Save(Format, const std::string &, const u8 *, int, int, int, int, bool)
    {
        return false;
    }
}

namespace GSTextureReplacements
{
    void Initialize() {}
    void GameChanged() {}
    void ReloadReplacementMap() {}
    void UpdateConfig(Pcsx2Config::GSOptions &) {}
    void Shutdown() {}
    bool HasAnyReplacementTextures()
    {
        return false;
    }
    bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey &)
    {
        return false;
    }
    GSTexture *LookupReplacementTexture(const GSTextureCache::HashCacheKey &, bool, bool *,
                                        std::pair<u8, u8> *)
    {
        return nullptr;
    }
    void ProcessAsyncLoadedTextures() {}
    void DumpTexture(const GSTextureCache::HashCacheKey &, const GIFRegTEX0 &, const GIFRegTEXA &,
                     GSTextureCache::SourceRegion, GSLocalMemory &, u32)
    {
    }
}

namespace ImGuiManager
{
    void Shutdown(bool)
    {
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
    }
    void WindowResized()
    {
        if (ImGui::GetCurrentContext() && g_gs_device)
            ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(g_gs_device->GetWindowWidth()),
                                                static_cast<float>(g_gs_device->GetWindowHeight()));
    }
    void RequestScaleUpdate() {}
    void ReloadFonts() {}
    void NewFrame()
    {
        if (ImGui::GetCurrentContext())
            ImGui::NewFrame();
    }
    void SkipFrame() {}
    void RenderOSD() {}
}

namespace FullscreenUI
{
    float GetBackgroundBlurScale()
    {
        return 1.0f;
    }
    void Render() {}
}

namespace PerformanceMetrics
{
    void Update(bool, bool, bool) {}
    void OnGPUPresent(float, u64, u64) {}
    void SetGSSWThreadCount(u32) {}
    void SetGSSWThread(u32, Threading::ThreadHandle) {}
    InternalFPSMethod GetInternalFPSMethod()
    {
        return InternalFPSMethod::None;
    }
    void StartSavingMetrics(u32) {}
    bool IsSavingMetrics()
    {
        return false;
    }
    void DumpSavedMetrics() {}
}

// Screenshots are not saved: RGBA8Image needs only to exist.
RGBA8Image::RGBA8Image() = default;
RGBA8Image::RGBA8Image(RGBA8Image &&move) = default;
bool RGBA8Image::SaveToFile(const char *, u8) const
{
    return false;
}

// --- executable memory for the JITs ----------------------------------------------

namespace
{
    // One reservation, as PCSX2 makes its own (main memory and the code
    // cache together): first the memory microVU addresses relative to its
    // code (VU1's micro and data memory, see pcsx2NearCodeData), then the
    // code regions asked for: microVU1's, the VIF unpack one between, and the
    // software renderer's (the end of PCSX2's code map).
    constexpr size_t kNearData = 0x10000;
    constexpr size_t kFirstCode = HostMemoryMap::mVU1recOffset;
    constexpr size_t kAreaSize = kNearData + (HostMemoryMap::CodeSize - kFirstCode);

    u8 *area()
    {
        // Kept mapped until the process exits (the area's destructor wants
        // it unmapped first).
        static SharedMemoryMappingArea *mapping = nullptr;
        static u8 *base = [] {
            mapping = SharedMemoryMappingArea::Create(kAreaSize, true).release();
            u8 *mapped = mapping ? mapping->Map(nullptr, 0, mapping->BasePointer(), kAreaSize, PageAccess_Any()) : nullptr;
            if (!mapped)
                std::fprintf(stderr, "[pcsx2] no executable memory for the recompilers\n");
            return mapped;
        }();
        return base;
    }
}

u8 *pcsx2NearCodeData()
{
    return area();
}

namespace SysMemory
{
    u8 *GetCodePtr(size_t offset)
    {
        if (offset < kFirstCode || offset > HostMemoryMap::CodeSize)
        {
            std::fprintf(stderr, "[pcsx2] code region at %zx was not reserved\n", offset);
            std::abort();
        }
        u8 *base = area();
        return base ? base + kNearData + (offset - kFirstCode) : nullptr;
    }
}

// --- what PCSX2's configuration code (Pcsx2Config.cpp) refers to ------------------
// GH1 Recomp only uses its GS options; controller, USB, audio and memory card
// configuration are the runtime's, so these only need to exist.

#include "SIO/Pad/Pad.h"
#include "USB/USB.h"
#include "Host/AudioStream.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "common/CrashHandler.h"
#include "imgui_freetype.h"

namespace Pad
{
    Pad::ControllerType GetDefaultPadType(u32)
    {
        return Pad::ControllerType::NotConnected;
    }
    const ControllerInfo *GetControllerInfo(Pad::ControllerType)
    {
        return nullptr;
    }
    const ControllerInfo *GetControllerInfoByName(const std::string_view)
    {
        return nullptr;
    }
    std::string GetConfigSection(u32 pad_index)
    {
        return "Pad" + std::to_string(pad_index + 1);
    }
}

namespace USB
{
    s32 DeviceTypeNameToIndex(const std::string_view)
    {
        return 0;
    }
    const char *DeviceTypeIndexToName(s32)
    {
        return "None";
    }
    std::string GetConfigSection(int port)
    {
        return "USB" + std::to_string(port + 1);
    }
}

std::optional<AudioBackend> AudioStream::ParseBackendName(const char *)
{
    return std::nullopt;
}
const char *AudioStream::GetBackendName(AudioBackend)
{
    return "None";
}
void AudioStreamParameters::LoadSave(SettingsWrapper &, const char *) {}

uint FileMcd_GetMtapPort(uint slot)
{
    return slot;
}
uint FileMcd_GetMtapSlot(uint)
{
    return 0;
}
bool FileMcd_IsMultitapSlot(uint)
{
    return false;
}
std::string FileMcd_GetDefaultName(uint slot)
{
    return "Mcd00" + std::to_string(slot + 1) + ".ps2";
}

namespace Host
{
    const char *TranslateToCString(const std::string_view, const std::string_view msg)
    {
        // Callers keep the pointer: intern each string once.
        static std::mutex mutex;
        static std::map<std::string, std::string, std::less<>> interned;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = interned.find(msg);
        if (it == interned.end())
            it = interned.emplace(std::string(msg), std::string(msg)).first;
        return it->second.c_str();
    }
}

namespace CrashHandler
{
#ifndef _WIN32
    void CrashSignalHandler(int, siginfo_t *, void *) {}
#endif
    // A failed assertion on Windows asks for a minidump; none is written.
    void WriteDumpForCaller() {}
}

namespace PageFaultHandler
{
    // Nothing in the GS faults on purpose (that is the EE recompiler's
    // fastmem): every fault goes to the next handler.
    HandlerResult HandlePageFault(void *, void *, bool)
    {
        return HandlerResult::ExecuteNextHandler;
    }
}

#include "imgui_internal.h"

namespace ImGuiFreeType
{
    // No FreeType: imgui's own stb_truetype loader, for the default font.
    const ImFontLoader *GetFontLoader()
    {
        return ImFontAtlasGetFontLoaderForStbTruetype();
    }
}

// --- what the OpenGL device and GS open path also ask for ----------------------------

#include <cstdarg>

#include "GS/Renderers/Common/GSDevice.h"

namespace ImGuiManager
{
    // No overlays are drawn, but the devices render ImGui's draw data every
    // present: an empty context with the default font, and a frame begun
    // after each present (as PCSX2's own manager does).
    bool Initialize()
    {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DisplaySize = ImVec2(static_cast<float>(std::max(1, g_gs_device->GetWindowWidth())),
                                static_cast<float>(std::max(1, g_gs_device->GetWindowHeight())));
        io.Fonts->AddFontDefault();
        NewFrame();
        return true;
    }
}

namespace Host
{
    void AddOSDMessage(std::string message, float)
    {
        std::fprintf(stderr, "[pcsx2 gs] %s\n", message.c_str());
    }
    void ReportFormattedErrorAsync(const std::string_view title, const char *format, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, format);
        std::vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);
        std::fprintf(stderr, "[pcsx2 gs] %.*s: %s\n", static_cast<int>(title.size()), title.data(), buf);
    }
    bool GetBoolSettingValue(const char *, const char *, bool default_value)
    {
        return default_value;
    }
}
