#pragma once

// The standalone PCSX2 GS behind a plain interface, so the host side (SDL,
// the PS2Recomp runtime) never includes PCSX2's headers. Every call is made
// on one thread: the one that called open(), which owns the GPU context.

#include <cstdint>
#include <string>
#include <vector>

namespace pcsx2gs
{
    struct NativeWindow
    {
        enum class Type
        {
            X11,
            Win32,
        };
        Type type = Type::X11;
        void *display = nullptr; // X11 Display*
        void *handle = nullptr;  // X11 Window id, or HWND
        uint32_t width = 0;
        uint32_t height = 0;
        float scale = 1.0f;
    };

    // The graphics API PCSX2's hardware renderer draws with. The Direct3D
    // ones exist on Windows only.
    enum class Api
    {
        OpenGL,
        Direct3D11,
        Direct3D12,
        Auto, // what PCSX2 picks for the GPU by itself (Windows; OpenGL elsewhere)
    };

    struct Settings
    {
        Api api = Api::OpenGL;
        bool software = false;  // PCSX2's software renderer instead of the hardware one
        bool null = false;      // draw nothing (measuring everything else)
        float upscale = 1.0f;   // internal resolution, multiples of native
        bool vsync = false;     // present on the host's vertical blank
        std::string cacheDir;   // where compiled shaders are kept between runs (UTF-8)
        std::string programList; // shader programs to make at the start besides the cache's own list (UTF-8)
    };

    bool open(const NativeWindow &window, const Settings &settings, std::string &error);
    // After open: the renderer that opened and the GPU it runs on.
    std::string describe();
    void close();

    // One GIF packet (whole qwords) from PATH1, 2 or 3.
    void transfer(uint32_t path, const uint8_t *data, uint32_t qwc);
    // VBlank start: the privileged registers as the guest left them (0x2000
    // bytes, PS2 layout from 0x12000000), then the field is shown.
    void vsync(const uint8_t *privRegs);
    // A local-to-host transfer set up by TRXDIR = 1, read into dst.
    void readLocal(uint8_t *dst, uint32_t qwc);
    // Late reads, for reads whose answer may be a frame or so old: the EE
    // sends the transfer's registers, takes its answer from readLocalNow
    // and goes on, and the GS then reads the transfer as ever (readLocal)
    // and calls noteLateRead, bringing local memory up to date from video
    // memory; at the next VBlank, before vsync, keepLateReads keeps a copy
    // of local memory where those reads read, which is what the next
    // readLocalNow answers from (local memory as it is now when there is no
    // copy). readLocalNow is the one call made from another thread (the
    // EE's); noteLateRead and keepLateReads are the GS thread's.
    // With the hardware renderers a late read of a 16-bit depth buffer
    // goes another way: copyDepthLate (GS thread, in place of readLocal and
    // noteLateRead) has the GPU copy the whole buffer once a frame, without
    // waiting for it; keepLateReads collects that copy at the VBlank after
    // the next, and readLocalNow answers from it. False when it does not
    // apply (the software renderer, another format): read the usual way.
    void readLocalNow(uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
    bool copyDepthLate();
    void noteLateRead();
    void keepLateReads();
    void resize(uint32_t width, uint32_t height, float scale);
    // The last presented frame as RGBA8, at the GS's internal resolution
    // with the display's aspect applied.
    bool snapshot(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height);

    // What the GS did that can hold a frame up, since the last call: shader
    // programs compiled (the first time an effect is drawn; from then on they
    // load from the cache folder), programs loaded from that cache, and
    // waits for the GPU (reading VRAM back).
    struct Stalls
    {
        uint32_t compiled = 0;
        double compiledMs = 0.0;
        uint32_t loaded = 0;
        double loadedMs = 0.0;
        uint32_t gpuWaits = 0;
        double gpuWaitMs = 0.0;
    };
    Stalls takeStalls();

    // What the hardware renderer keeps in video memory now, in bytes: the
    // texture cache's textures (sources), its render targets, the textures
    // kept by hash, and the device's pool of spare textures. Zero for the
    // software and null renderers.
    struct VideoMemory
    {
        uint64_t sources = 0;
        uint64_t targets = 0;
        uint64_t hashed = 0;
        uint64_t pool = 0;
    };
    VideoMemory videoMemory();
}
