// Smoke test for the standalone PCSX2 GS: an X11 window, the GS opened on it
// with OpenGL, PS2 display registers set for a 640x448 32-bit frame, one GIF
// packet drawing a full-screen sprite, a vsync to present, and the window's
// centre pixel read back. Exit 0 when that pixel is the sprite's colour.

#include "GS/GS.h"
#include "Config.h"
#include "common/AlignedMalloc.h"
#include "pcsx2gs_host.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    constexpr int kW = 640, kH = 448;

    // Privileged registers, at their offsets from 0x12000000.
    void setPriv(u8 *regs, u32 offset, u64 value)
    {
        std::memcpy(regs + offset, &value, 8);
    }

    struct Packet
    {
        std::vector<u64> words;
        void tag(u32 nloop, bool eop, u32 nreg, u64 regs)
        {
            words.push_back(u64(nloop) | (u64(eop) << 15) | (u64(nreg) << 60));
            words.push_back(regs);
        }
        void ad(u8 reg, u64 value)
        {
            words.push_back(value);
            words.push_back(reg);
        }
    };
}

int main()
{
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
    {
        std::printf("no X display\n");
        return 2;
    }
    const Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, kW, kH, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    WindowInfo wi;
    wi.type = WindowInfo::Type::X11;
    wi.display_connection = dpy;
    wi.window_handle = reinterpret_cast<void *>(win);
    wi.surface_width = kW;
    wi.surface_height = kH;
    pcsx2gs::host().acquireWindow = [&](bool) { return std::optional<WindowInfo>(wi); };

    u8 *regs = static_cast<u8 *>(_aligned_malloc(0x2000, 32));
    std::memset(regs, 0, 0x2000);
    Pcsx2Config::GSOptions options;
    if (!GSopen(options, GSRendererType::OGL, regs, GSVSyncMode::Disabled, false))
    {
        std::printf("GSopen(OpenGL) failed\n");
        return 1;
    }
    std::printf("GSopen(OpenGL): ok\n");

    // CRT1 on, 640x448 PSMCT32 at FBP 0 (DISPLAY: DW = 640*4-1 in VCK, DH = 447).
    setPriv(regs, 0x0000, 0x0001u | (1u << 5) | (0xFFull << 8)); // PMODE: EN1, MMOD, ALP
    setPriv(regs, 0x0010, (32ull << 3) | (2ull << 13) | (4ull << 21)); // SMODE1: LC 32, CMOD NTSC, SPML 4
    setPriv(regs, 0x0020, 0x1);                                  // SMODE2: INT
    setPriv(regs, 0x0070, (10ull << 9));                         // DISPFB1: FBW 10
    setPriv(regs, 0x0080, (636ull) | (50ull << 12) | (3ull << 23) | (2559ull << 32) | (447ull << 44)); // DISPLAY1: MAGH 3

    Packet p;
    p.tag(9, true, 1, 0xE); // A+D
    p.ad(0x4C, 10ull << 16);                                   // FRAME_1: FBP 0, FBW 10, PSMCT32
    p.ad(0x40, 0ull | (639ull << 16) | (0ull << 32) | (447ull << 48)); // SCISSOR_1
    p.ad(0x18, 0);                                             // XYOFFSET_1
    p.ad(0x47, 0);                                             // TEST_1
    p.ad(0x4E, 0x100000000ull);                                // ZBUF_1: ZMSK
    p.ad(0x00, 6);                                             // PRIM: sprite
    p.ad(0x01, 0x80u | (0x00u << 8) | (0xFFu << 0) | (0x80ull << 24)); // RGBAQ
    p.ad(0x05, 0);                                             // XYZ2 (0,0)
    p.ad(0x05, (u64(kW) << 4) | (u64(kH) << 20));              // XYZ2 (640,448)
    GSgifTransfer(reinterpret_cast<const u8 *>(p.words.data()), static_cast<u32>(p.words.size() / 2));
    for (int i = 0; i < 3; ++i)
        GSvsync(i & 1, true);
    XSync(dpy, False);

    XImage *img = XGetImage(dpy, win, kW / 2, kH / 2, 1, 1, AllPlanes, ZPixmap);
    const unsigned long px = img ? XGetPixel(img, 0, 0) : 0;
    std::printf("centre pixel %06lx\n", px);
    GSclose();
    return (px & 0xFF0000) > 0x800000 ? 0 : 3;
}
