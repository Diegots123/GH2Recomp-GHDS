#include "pcsx2gs_sdl_host.h"
#include "product.h"

#include "pcsx2gs_bridge.h"
#if defined(GH1_PCSX2VU)
#include "pcsx2vu_bridge.h"
#endif
#include "stall_probe.h"
#include "guitars.h"
#include "keyboard_options.h"
#include "runtime/gs/gs_null_backend.h"
#include "runtime/gs/gs_packet_tap.h"
#include "runtime/host_hooks.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ee_video_timing.h"
#include "runtime/vif1_worker.h"
#include "runtime/ps2_disc_image.h"

#include <SDL3/SDL.h>
#if !defined(_WIN32)
#include <cpuid.h>
#include <sched.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <psapi.h>
#include <timeapi.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pcsx2gs
{
    namespace
    {
        // Port 1 as the host last decided it: bit 0 plugged, bit 1 a guitar.
        std::atomic<uint32_t> s_portOne{3u};
        // The screen GH1 shows (GH1's side says so).
        std::atomic<Screen> s_screen{Screen::Other};

        // The CPU's name as it gives it (x86's brand string).
        std::string cpuName()
        {
            uint32_t regs[12] = {};
#if defined(_MSC_VER)
            int r[4];
            __cpuid(r, static_cast<int>(0x80000000u));
            if (static_cast<uint32_t>(r[0]) < 0x80000004u)
                return "unknown";
            for (uint32_t i = 0; i < 3; ++i)
            {
                __cpuid(r, static_cast<int>(0x80000002u + i));
                std::memcpy(regs + i * 4, r, sizeof(r));
            }
#else
            unsigned a, b, c, d;
            if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u)
                return "unknown";
            for (uint32_t i = 0; i < 3; ++i)
            {
                __get_cpuid(0x80000002u + i, &a, &b, &c, &d);
                regs[i * 4] = a;
                regs[i * 4 + 1] = b;
                regs[i * 4 + 2] = c;
                regs[i * 4 + 3] = d;
            }
#endif
            std::string name(reinterpret_cast<const char *>(regs), sizeof(regs));
            name.erase(name.find_last_not_of(std::string(" \0", 2)) + 1);
            name.erase(0, name.find_first_not_of(' '));
            return name.empty() ? "unknown" : name;
        }

        // The CPU threads this process may run on (its affinity, which a
        // test of a weaker PC narrows).
        unsigned availableThreads()
        {
#if defined(_WIN32)
            DWORD_PTR process = 0, system = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
            {
                unsigned n = 0;
                for (DWORD_PTR m = process; m; m &= m - 1)
                    ++n;
                return n;
            }
#else
            cpu_set_t set;
            CPU_ZERO(&set);
            if (sched_getaffinity(0, sizeof(set), &set) == 0)
                return static_cast<unsigned>(CPU_COUNT(&set));
#endif
            return static_cast<unsigned>(std::max(1, SDL_GetNumLogicalCPUCores()));
        }

        // The process's memory in use (its working set, or resident set), MB.
        unsigned processMegabytes()
        {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                return static_cast<unsigned>(pmc.WorkingSetSize >> 20);
            return 0;
#else
            unsigned long pages = 0, resident = 0;
            if (FILE *f = std::fopen("/proc/self/statm", "r"))
            {
                if (std::fscanf(f, "%lu %lu", &pages, &resident) != 2)
                    resident = 0;
                std::fclose(f);
            }
            return static_cast<unsigned>((resident * 4096ull) >> 20);
#endif
        }

        // PS2 pad bits, active low (libpad order).
        constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008;
        constexpr uint16_t kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080;
        constexpr uint16_t kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800;
        constexpr uint16_t kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;

        constexpr uint32_t kPrivRegsSize = 0x2000;

        // The EE's side of a frame, for the hitch log, after the registers in
        // a VBlank record (microseconds): since the VBlank before, the wait
        // for the GS at the one before, and the waits for VRAM reads.
        struct EeTiming
        {
            uint32_t sinceLast;
            uint32_t vblankWait;
            uint32_t readWait;
            uint32_t reads;
            uint32_t vuRuns;    // VU1 programs run, their time, and the longest
            uint32_t vuTotal;
            uint32_t vuLongest;
            uint32_t discWait;  // waiting on disc reads (the host's disk), microseconds, and how many
            uint32_t idle;      // ahead of the clock with nothing to do, before this VBlank
            uint32_t vifBusy;   // the VIF1 thread's work since the last VBlank (0 when it is off)
            uint32_t vifWait;   // the EE waiting for the VIF1 thread to catch up (drain)
        };
        static_assert(sizeof(EeTiming) == 44);
        constexpr uint32_t kFramesAhead = 2; // vsyncs the EE may queue before it waits, as PCSX2

        enum RecordType : uint32_t
        {
            kGif = 1,
            kVSync = 2,
            kRead = 3,
            kReadLate = 4, // a read whose answer the EE already took (path = qwords)
        };

        struct Record
        {
            uint32_t type;
            uint32_t path;
            uint32_t bytes;
            uint32_t pad;
        };
        static_assert(sizeof(Record) == 16);

        struct ReadRequest
        {
            uint8_t *dst;
            uint32_t qwc;
            bool done;
        };

        void putReg(uint8_t *regs, uint32_t offset, uint64_t value)
        {
            std::memcpy(regs + offset, &value, sizeof(value));
        }

        // The runtime's privileged GS registers in the PS2's own layout.
        void privRegsPs2Layout(const GSRegisters &gs, uint8_t *out)
        {
            std::memset(out, 0, kPrivRegsSize);
            putReg(out, 0x000, gs.pmode);
            putReg(out, 0x010, gs.smode1);
            putReg(out, 0x020, gs.smode2);
            putReg(out, 0x030, gs.srfsh);
            putReg(out, 0x040, gs.synch1);
            putReg(out, 0x050, gs.synch2);
            putReg(out, 0x060, gs.syncv);
            putReg(out, 0x070, gs.dispfb1);
            putReg(out, 0x080, gs.display1);
            putReg(out, 0x090, gs.dispfb2);
            putReg(out, 0x0A0, gs.display2);
            putReg(out, 0x0B0, gs.extbuf);
            putReg(out, 0x0C0, gs.extdata);
            putReg(out, 0x0D0, gs.extwrite);
            putReg(out, 0x0E0, gs.bgcolor);
            putReg(out, 0x1000, gs.csr.load(std::memory_order_acquire));
            putReg(out, 0x1010, gs.imr);
            putReg(out, 0x1040, gs.busdir);
            putReg(out, 0x1080, gs.siglblid);
        }

        float envFloat(const char *name, float fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? static_cast<float>(std::atof(v)) : fallback;
        }

        bool envFlag(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && std::strcmp(v, "0") != 0;
        }

        // A test script of button presses by presented frame, so runs are the
        // same however fast the host draws: "1500=cross;2400=start+r2", each
        // press held for 8 frames.
        // A step of GH1_SCRIPT: "<frame>=a+b" holds buttons for 8 frames,
        // "<from>-<to>=..." for that span. "whammy" works the whammy and
        // "flip" makes port 1 the other kind for a while (tests).
        struct ScriptStep
        {
            uint64_t frame;
            uint16_t buttons;
            uint64_t until = 0;
            bool whammy = false;
            bool flip = false;
        };

        std::vector<ScriptStep> parseScript(const char *text)
        {
            static const struct
            {
                const char *name;
                uint16_t bit;
            } kNames[] = {{"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
                          {"cross", kCross}, {"circle", kCircle}, {"square", kSquare},
                          {"triangle", kTriangle}, {"l1", kL1}, {"r1", kR1}, {"l2", kL2},
                          {"r2", kR2}, {"l3", kL3}, {"r3", kR3}, {"start", kStart}, {"select", kSelect}};
            std::vector<ScriptStep> steps;
            std::string all = text ? text : "";
            size_t at = 0;
            while (at < all.size())
            {
                size_t end = all.find(';', at);
                if (end == std::string::npos)
                    end = all.size();
                const std::string step = all.substr(at, end - at);
                at = end + 1;
                const size_t eq = step.find('=');
                if (eq == std::string::npos)
                    continue;
                char *rest = nullptr;
                ScriptStep st{std::strtoull(step.c_str(), &rest, 10), 0};
                st.until = (rest && *rest == '-') ? std::strtoull(rest + 1, nullptr, 10) : st.frame + 8u;
                std::string names = step.substr(eq + 1) + "+";
                size_t n = 0;
                while (n < names.size())
                {
                    const size_t plus = names.find('+', n);
                    const std::string name = names.substr(n, plus - n);
                    n = plus + 1;
                    for (const auto &k : kNames)
                        if (name == k.name)
                            st.buttons |= k.bit;
                    st.whammy |= name == "whammy";
                    st.flip |= name == "flip";
                }
                steps.push_back(st);
            }
            return steps;
        }

        class SdlHost;
        // The host the EE's late reads go to (readLocalLate), while it runs.
        std::atomic<SdlHost *> s_lateReads{nullptr};

        class SdlHost final : public PS2Runtime::HostFrontend
        {
        public:
            explicit SdlHost(const HostConfig &config) : m_config(config) {}

            bool initialize(PS2Runtime &runtime, const char *title) override
            {
                m_runtime = &runtime;
#if defined(_WIN32)
                // Waits wake to the millisecond, not to Windows' default
                // 15.6 ms tick: the VBlanks are paced by such waits.
                timeBeginPeriod(1);
                // Above other programs (a browser, an antivirus scan): with
                // few CPU threads they otherwise take turns with the game's
                // and a frame waits. The game sleeps when it is ahead, so it
                // takes no more than it needs.
                if (m_config.raisePriority)
                    std::fprintf(stderr, "[host] priority: %s\n",
                                 SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) ? "above normal"
                                                                                                    : "normal (refused)");
                else
                    std::fprintf(stderr, "[host] priority: normal\n");
#endif
                if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
                {
                    std::fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError());
                    return false;
                }
                // The community controller database (SDL_GameControllerDB)
                // beside the exe: the layouts of many more USB adapters for
                // the DualShock 2 than SDL itself knows.
                if (const char *base = SDL_GetBasePath())
                {
                    const int added =
                        SDL_AddGamepadMappingsFromFile((std::string(base) + "gamecontrollerdb.txt").c_str());
                    if (added > 0)
                        std::fprintf(stderr, "[host] gamecontrollerdb.txt: %d layouts\n", added);
                }
                const auto &options = runtime.hostOptions();
                SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
                if (options.hidden)
                    flags |= SDL_WINDOW_HIDDEN;
                else if (m_config.fullscreen)
                    flags |= SDL_WINDOW_FULLSCREEN;
                m_window = SDL_CreateWindow(title, static_cast<int>(m_config.windowWidth),
                                            static_cast<int>(m_config.windowHeight), flags);
                if (!m_window)
                {
                    std::fprintf(stderr, "[host] SDL_CreateWindow: %s\n", SDL_GetError());
                    return false;
                }

                NativeWindow nw;
                const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(_WIN32)
                nw.type = NativeWindow::Type::Win32;
                nw.handle = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
                nw.type = NativeWindow::Type::X11;
                nw.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
                nw.handle = reinterpret_cast<void *>(static_cast<uintptr_t>(
                    SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
                if (!nw.display)
                {
                    std::fprintf(stderr, "[host] the window is not an X11 window (set SDL_VIDEO_DRIVER=x11)\n");
                    return false;
                }
#endif
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
                nw.width = static_cast<uint32_t>(pw);
                nw.height = static_cast<uint32_t>(ph);
                nw.scale = SDL_GetWindowDisplayScale(m_window);

                Settings settings;
                settings.api = m_config.api;
                settings.software = m_config.software;
                settings.null = m_config.null;
                settings.upscale = m_config.upscale;
                settings.vsync = m_config.vsync;
                settings.halfPixelOffset = m_config.gsHalfPixelOffset;
                settings.nativeScaling = m_config.gsNativeScaling;
                if (const char *base = SDL_GetBasePath())
                {
                    settings.cacheDir = std::string(base) + "cache";
                    settings.programList = std::string(base) + GHR_PROGRAMS;
                }
                std::string error;
                bool opened = pcsx2gs::open(nw, settings, error);
                if (!opened && settings.api != Api::OpenGL && !settings.software && !settings.null)
                {
                    // The API the ini asks for is not there (an old driver):
                    // OpenGL rather than no game.
                    std::fprintf(stderr, "[host] %s; trying OpenGL\n", error.c_str());
                    settings.api = Api::OpenGL;
                    opened = pcsx2gs::open(nw, settings, error);
                }
                if (!opened)
                {
                    std::fprintf(stderr, "[host] %s\n", error.c_str());
                    return false;
                }
                std::fprintf(stderr, "[gs] renderer: %s\n", pcsx2gs::describe().c_str());
                const unsigned threads = availableThreads();
                std::fprintf(stderr, "[host] CPU: %s, %d threads (%u for this process); %d MB of memory\n",
                             cpuName().c_str(), SDL_GetNumLogicalCPUCores(), threads, SDL_GetSystemRAM());
                // VIF1 and VU1 on a thread of their own (vu1_thread): with
                // two CPU threads or more free, by default, the EE's own
                // thread no longer runs the VU1 programs. (Two threads at
                // full speed, on Diego's PC: the EE's work went from ~14 ms a
                // frame in a song to ~8, the VU1 thread's ~6, smooth.)
                const bool vu1Thread = m_config.vu1Thread == 1 || (m_config.vu1Thread < 0 && threads >= 2);
                if (vu1Thread)
                    ps2x::vif1::start(runtime.memory());
                std::fprintf(stderr, "[host] VU1: %s\n",
                             vu1Thread ? "on its own thread" : "on the EE's thread");

                // The runtime's own rasterizer stops; the GS gets every packet.
                runtime.gs().setRasterBackend(std::make_unique<GSNullBackend>(
                    [this](uint8_t *dst, uint32_t maxBytes) { return readLocal(dst, maxBytes); }));
                ps2x::gs::setPacketTap([this](uint32_t path, const uint8_t *data, uint32_t bytes)
                                       { push(kGif, path, data, bytes); });
                ps2x::host::setVBlankHook([this](uint64_t) { vblank(); });
                s_lateReads.store(this, std::memory_order_release);
                ps2x::host::setPadSource([this](int port, int slot, uint8_t *data, size_t size)
                                         { return readPad(port, slot, data, size); });

                m_shotFrom = [] {
                    const char *v = std::getenv("PS2X_SHOT_FROM");
                    return v ? std::strtoull(v, nullptr, 10) : 0ull;
                }();
                m_statsStart = std::chrono::steady_clock::now();
                m_script = parseScript(std::getenv("GH1_SCRIPT"));
                matchRefresh();
                if (const char *v = std::getenv("GH1_DISPTRACE"))
                    std::sscanf(v, "%llu-%llu", reinterpret_cast<unsigned long long *>(&m_traceFrom),
                                reinterpret_cast<unsigned long long *>(&m_traceTo));
                if (const char *v = std::getenv("GH1_EXIT_AT"))
                    m_exitAt = std::strtoull(v, nullptr, 10);
                if (const char *v = std::getenv("GH1_HITCH_MS"))
                    m_hitchMs = std::atof(v);
                m_hitchAll = envFlag("GH1_HITCH_ALL"); // also every frame that waited on the GPU or read VRAM
                return true;
            }

            bool frame(PS2Runtime &runtime) override
            {
                // Window and controller events every 2 ms at most: this
                // thread also draws everything the EE sends, often in many
                // small batches a frame, and the game reads the pad once a
                // frame.
                bool running = true;
                const auto pumpFrom = std::chrono::steady_clock::now();
                if (pumpFrom - m_lastPump >= std::chrono::milliseconds(2))
                {
                    running = pumpEvents();
                    m_lastPump = pumpFrom;
                    const auto pumped = std::chrono::steady_clock::now() - pumpFrom;
                    m_framePump += pumped;
                    m_framePumpMax = std::max(m_framePumpMax, pumped);
                }
                if (!running || (m_exitAt != 0 && m_frames >= m_exitAt))
                {
                    stop();
                    return false;
                }
                {
                    std::unique_lock lock(m_mutex);
                    if (m_pending.empty())
                    {
                        m_hostWaiting = true;
                        const auto waitFrom = std::chrono::steady_clock::now();
                        m_work.wait_for(lock, std::chrono::milliseconds(4));
                        m_frameIdle += std::chrono::steady_clock::now() - waitFrom;
                        m_hostWaiting = false;
                    }
                    m_draining.swap(m_pending);
                }
                const uint32_t vsyncs = process(m_draining);
                m_draining.clear();
                if (vsyncs != 0)
                {
                    {
                        std::lock_guard lock(m_mutex);
                        m_queuedVsyncs -= std::min(m_queuedVsyncs, vsyncs);
                    }
                    m_space.notify_all();
                }
                (void)runtime;
                return true;
            }

            // A local-to-host read answered at once from local memory as the GS
            // has it (what earlier reads brought back from video memory), the
            // read itself queued behind the transfer's registers so that the
            // GS brings that memory up to date for the next one. For reads a
            // frame old serves as well as now (see gh1/flares.cpp): the EE
            // neither waits for the GS to catch up nor for the GPU to finish.
            void readLate(uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
            {
                pcsx2gs::readLocalNow(dst, bytes, bitbltbuf, trxpos, trxreg);
                // Behind the transfer's registers, which may still be with
                // the VIF1 thread.
                const uint32_t qwc = (bytes + 15u) / 16u;
                ps2x::vif1::post([this, qwc]() {
                    std::lock_guard batchLock(m_batchMutex);
                    append(Record{kReadLate, qwc, 0, 0}, nullptr, 0);
                });
                ++m_eeReads;
            }

            void shutdown(PS2Runtime &) override
            {
                s_lateReads.store(nullptr, std::memory_order_release);
                ps2x::vif1::stop(); // its last packets reach the GS while it is still open
                stop();
                ps2x::host::setVBlankHook({});
                ps2x::host::setPadSource({});
                ps2x::gs::setPacketTap({});
                pcsx2gs::close();
                for (guitars::Device &d : m_devices)
                    guitars::close(d);
                m_devices.clear();
                m_options.close();
                if (m_window)
                    SDL_DestroyWindow(m_window);
                m_window = nullptr;
                SDL_Quit();
#if defined(_WIN32)
                timeEndPeriod(1);
#endif
            }

        private:
            // With vsync on, the game's VBlanks at the monitor's own rate when
            // it is the PS2's within 1% (a "60 Hz" monitor at 60.00 against
            // NTSC's 59.94), or a whole multiple of it (120, 180, 240 Hz...):
            // every picture then lasts the same number of refreshes, where
            // 59.94 against 60.00 showed one picture twice every 17 seconds.
            // (GH1 times its songs by the audio; the 0.1% only reaches menus
            // and animations.)
            void matchRefresh()
            {
                if (!m_config.vsync || !m_window || !m_runtime)
                    return;
                const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
                if (!mode)
                    return;
                const double refresh = mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0
                                           ? static_cast<double>(mode->refresh_rate_numerator) / mode->refresh_rate_denominator
                                           : static_cast<double>(mode->refresh_rate);
                constexpr double kNtsc = 60000.0 / 1001.0;
                double rate = kNtsc;
                for (const double divisor : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0})
                    if (std::abs(refresh / divisor - kNtsc) <= kNtsc * 0.01)
                    {
                        rate = refresh / divisor;
                        break;
                    }
                m_runtime->eeScheduler().setVBlankPeriod(
                    std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate + 0.5)));
                std::fprintf(stderr, "[host] monitor at %.3f Hz: VBlanks at %.3f Hz\n", refresh, rate);
            }

            // --- EE side --------------------------------------------------------

            // The EE's records gather here and go to the GS thread in batches
            // (kBatchBytes, or at once for a VBlank or a VRAM read): handing
            // each GIF packet over by itself, with a wake-up of that thread,
            // cost the EE a fifth of its time, hundreds of times a frame.
            // Small enough that the GS draws a frame while the EE still
            // makes it, so the frame can be shown as soon as its VBlank
            // arrives (a big batch left at the VBlank put a variable delay
            // before each picture): the GS thread is only woken when it is
            // waiting for work, which keeps small batches cheap.
            static constexpr size_t kBatchBytes = 48 * 1024;

            void append(const Record &rec, const void *a, uint32_t aBytes, const void *b = nullptr, uint32_t bBytes = 0)
            {
                const size_t at = m_batch.size();
                m_batch.resize(at + sizeof(Record) + ((rec.bytes + 15u) & ~15u));
                std::memcpy(m_batch.data() + at, &rec, sizeof(rec));
                if (aBytes)
                    std::memcpy(m_batch.data() + at + sizeof(rec), a, aBytes);
                if (bBytes)
                    std::memcpy(m_batch.data() + at + sizeof(rec) + aBytes, b, bBytes);
            }

            // Hands the batch to the GS thread; m_batchMutex held, and
            // m_mutex taken here.
            void handOver(std::unique_lock<std::mutex> &lock, bool vsync)
            {
                if (m_pending.empty())
                    m_pending.swap(m_batch);
                else
                    m_pending.insert(m_pending.end(), m_batch.begin(), m_batch.end());
                m_batch.clear();
                if (vsync)
                    ++m_queuedVsyncs;
                (void)lock;
                if (m_hostWaiting || vsync)
                    m_work.notify_one();
            }

            void push(uint32_t type, uint32_t path, const void *data, uint32_t bytes)
            {
                std::lock_guard batchLock(m_batchMutex);
                append(Record{type, path, bytes, 0}, data, bytes);
                if (m_batch.size() >= kBatchBytes)
                {
                    std::unique_lock lock(m_mutex);
                    handOver(lock, false);
                }
            }

            void vblank()
            {
                uint8_t regs[kPrivRegsSize];
                privRegsPs2Layout(m_runtime->memory().gs(), regs);
                // For the hitch log: the time since the last VBlank reached
                // here (the EE's side of a late frame) and how long the EE
                // then waited for the GS, in microseconds.
                const auto now = std::chrono::steady_clock::now();
                const uint32_t sinceLast = m_eeLastVblank.time_since_epoch().count() == 0
                                               ? 0u
                                               : static_cast<uint32_t>(std::min<int64_t>(
                                                     std::chrono::duration_cast<std::chrono::microseconds>(now - m_eeLastVblank).count(),
                                                     0xFFFFFFFF));
                m_eeLastVblank = now;
                const DiscReadStats disc = ps2TakeDiscReadStats();
                EeTiming timing{sinceLast, m_eeWaitedUs, m_eeReadWaitUs.exchange(0), m_eeReads.exchange(0), 0, 0, 0,
                                static_cast<uint32_t>(std::min<uint64_t>(disc.waitedUs, 0xFFFFFFFFu)),
                                static_cast<uint32_t>(std::min<uint64_t>(ps2x::takePacingIdleNs() / 1000u, 0xFFFFFFFFu))};
#if defined(GH1_PCSX2VU)
                const pcsx2vu::Timing vu = pcsx2vu::takeTiming();
                timing.vuRuns = vu.runs;
                timing.vuTotal = static_cast<uint32_t>(std::min<uint64_t>(vu.totalNs / 1000u, 0xFFFFFFFFu));
                timing.vuLongest = static_cast<uint32_t>(std::min<uint64_t>(vu.longestNs / 1000u, 0xFFFFFFFFu));
                m_vuSlowRuns.fetch_add(vu.slowRuns, std::memory_order_relaxed);
                m_vuClears.fetch_add(vu.clears, std::memory_order_relaxed);
                m_vuCompiled.fetch_add(vu.compiledBytes, std::memory_order_relaxed);
                m_vuResets.fetch_add(vu.cacheResets, std::memory_order_relaxed);
#endif
                timing.vifBusy = static_cast<uint32_t>(std::min<uint64_t>(ps2x::vif1::takeBusyNs() / 1000u, 0xFFFFFFFFu));
                timing.vifWait = static_cast<uint32_t>(std::min<uint64_t>(ps2x::vif1::takeDrainWaitNs() / 1000u, 0xFFFFFFFFu));
                std::unique_lock<std::mutex> lock;
                if (ps2x::vif1::active())
                {
                    // The frame's last packets may still be with the VIF1
                    // thread: the VBlank's record goes behind them, there.
                    std::vector<uint8_t> copy(regs, regs + kPrivRegsSize);
                    m_postedVsyncs.fetch_add(1u);
                    ps2x::vif1::post([this, copy = std::move(copy), timing]() {
                        std::unique_lock batchLock(m_batchMutex);
                        append(Record{kVSync, 0, kPrivRegsSize + sizeof(EeTiming), 0}, copy.data(), kPrivRegsSize,
                               &timing, sizeof(timing));
                        std::unique_lock lock(m_mutex);
                        handOver(lock, true);
                        m_postedVsyncs.fetch_sub(1u);
                        lock.unlock();
                        batchLock.unlock();
                        m_space.notify_all();
                    });
                    lock = std::unique_lock(m_mutex);
                }
                else
                {
                    std::unique_lock batchLock(m_batchMutex);
                    append(Record{kVSync, 0, kPrivRegsSize + sizeof(EeTiming), 0}, regs, kPrivRegsSize, &timing,
                           sizeof(timing));
                    lock = std::unique_lock(m_mutex);
                    handOver(lock, true);
                }
                // Too far ahead of the picture: the EE waits for the GS (the
                // VBlanks the VIF1 thread has yet to hand over count too).
                const auto waitFrom = std::chrono::steady_clock::now();
                while (m_queuedVsyncs + m_postedVsyncs.load() > kFramesAhead && !m_stopping &&
                       !m_runtime->isStopRequested())
                    m_space.wait_for(lock, std::chrono::milliseconds(10));
                m_eeWaitedUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                         std::chrono::steady_clock::now() - waitFrom)
                                                         .count());
                m_eeLastVblank += std::chrono::steady_clock::now() - waitFrom;
            }

            // A local-to-host read of maxBytes. The GS sends whole qwords:
            // a read that ends inside one (GH1's lens flares read a single
            // 16-bit Z value) takes the whole qword and keeps what was asked.
            uint32_t readLocal(uint8_t *dst, uint32_t maxBytes)
            {
                if ((maxBytes & 15u) != 0u)
                {
                    std::vector<uint8_t> whole((maxBytes + 15u) & ~15u);
                    const uint32_t got = readLocal(whole.data(), static_cast<uint32_t>(whole.size()));
                    const uint32_t keep = std::min(got, maxBytes);
                    std::memcpy(dst, whole.data(), keep);
                    return keep;
                }
                const auto t0 = std::chrono::steady_clock::now();
                ReadRequest req{dst, maxBytes / 16u, false};
                const ReadRequest *ptr = &req;
                std::unique_lock batchLock(m_batchMutex);
                append(Record{kRead, 0, sizeof(ptr), 0}, &ptr, sizeof(ptr));
                std::unique_lock lock(m_mutex);
                handOver(lock, false);
                batchLock.unlock();
                while (!req.done && !m_stopping && !m_runtime->isStopRequested())
                    m_space.wait_for(lock, std::chrono::milliseconds(10));
                lock.unlock();
                m_eeReadWaitUs += static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                ++m_eeReads;
                return req.done ? req.qwc * 16u : 0u;
            }

            bool readPad(int port, int slot, uint8_t *data, size_t size)
            {
                if (port != 0 || slot != 0 || size < 32)
                    return false;
                const uint64_t s = m_pad.load(std::memory_order_acquire);
                std::memset(data, 0, 32);
                data[0] = 0x01;
                data[1] = 0x73;
                data[2] = static_cast<uint8_t>(s & 0xFF);
                data[3] = static_cast<uint8_t>((s >> 8) & 0xFF);
                data[4] = static_cast<uint8_t>(s >> 16); // right x
                data[5] = static_cast<uint8_t>(s >> 24); // right y
                data[6] = static_cast<uint8_t>(s >> 32); // left x
                data[7] = static_cast<uint8_t>(s >> 40); // left y
                return true;
            }

            void stop()
            {
                {
                    std::lock_guard lock(m_mutex);
                    m_stopping = true;
                }
                m_space.notify_all();
            }

            // --- host side ------------------------------------------------------

            uint32_t process(const std::vector<uint8_t> &buffer)
            {
                uint32_t vsyncs = 0;
                size_t at = 0;
                while (at + sizeof(Record) <= buffer.size())
                {
                    Record rec;
                    std::memcpy(&rec, buffer.data() + at, sizeof(rec));
                    const uint8_t *payload = buffer.data() + at + sizeof(rec);
                    at += sizeof(rec) + ((rec.bytes + 15u) & ~15u);
                    switch (rec.type)
                    {
                    case kGif:
                    {
                        if (tracing())
                            traceFrames(rec.path, payload, rec.bytes);
                        const auto t0 = std::chrono::steady_clock::now();
                        pcsx2gs::transfer(rec.path, payload, rec.bytes / 16u);
                        m_frameGs += std::chrono::steady_clock::now() - t0;
                        break;
                    }
                    case kVSync:
                    {
                        if (tracing())
                            traceVsync(payload);
                        const auto kept = std::chrono::steady_clock::now();
                        pcsx2gs::keepLateReads();
                        const auto t0 = std::chrono::steady_clock::now();
                        m_frameCollect += t0 - kept;
                        pcsx2gs::vsync(payload);
                        const auto t1 = std::chrono::steady_clock::now();
                        ++vsyncs;
                        presented();
                        hitchCheck(payload, rec, t1 - t0, t1);
                        break;
                    }
                    case kReadLate:
                    {
                        m_lateScratch.resize(static_cast<size_t>(rec.path) * 16u);
                        const auto t0 = std::chrono::steady_clock::now();
                        if (!pcsx2gs::copyDepthLate())
                        {
                            pcsx2gs::readLocal(m_lateScratch.data(), rec.path);
                            pcsx2gs::noteLateRead();
                        }
                        m_frameReads += std::chrono::steady_clock::now() - t0;
                        ++m_frameReadCount;
                        break;
                    }
                    case kRead:
                    {
                        ReadRequest *req;
                        std::memcpy(&req, payload, sizeof(req));
                        const auto t0 = std::chrono::steady_clock::now();
                        pcsx2gs::readLocal(req->dst, req->qwc);
                        m_frameReads += std::chrono::steady_clock::now() - t0;
                        ++m_frameReadCount;
                        {
                            std::lock_guard lock(m_mutex);
                            req->done = true;
                        }
                        m_space.notify_all();
                        break;
                    }
                    default:
                        break;
                    }
                }
                return vsyncs;
            }

            bool tracing() const { return m_traceTo != 0 && m_frames >= m_traceFrom && m_frames <= m_traceTo; }

            // The hitch log: a frame shown late (more than GH1_HITCH_MS after
            // the one before, 25 by default; 0 turns it off), or one that
            // compiled shaders, gets a line with where its time went: the
            // GS drawing it, presenting it, shaders, VRAM reads, and the EE
            // (a VBlank that reached the host late, a wait for the GS), and
            // for how many VBlanks the game has shown the same picture.
            void hitchCheck(const uint8_t *regs, const Record &rec, std::chrono::steady_clock::duration present,
                            std::chrono::steady_clock::time_point now)
            {
                EeTiming ee{};
                if (rec.bytes >= kPrivRegsSize + sizeof(EeTiming))
                    std::memcpy(&ee, regs + kPrivRegsSize, sizeof(ee));
                using ms = std::chrono::duration<double, std::milli>;
                uint64_t dispfb1, dispfb2;
                std::memcpy(&dispfb1, regs + 0x70, 8);
                std::memcpy(&dispfb2, regs + 0x90, 8);
                const uint64_t shown = dispfb1 ^ (dispfb2 << 1);
                m_samePicture = shown == m_lastShown ? m_samePicture + 1u : 0u;
                m_lastShown = shown;
                const pcsx2gs::Stalls stalls = pcsx2gs::takeStalls();
                const double since = m_lastPresent.time_since_epoch().count() == 0 ? 0.0 : ms(now - m_lastPresent).count();
                m_lastPresent = now;
                const bool late = m_hitchMs > 0.0 && since > m_hitchMs && m_frames > 60;
                const bool stalled = m_hitchAll && (stalls.loaded != 0 || stalls.gpuWaits != 0 || m_frameReadCount != 0);
                if ((late || stalled || stalls.compiled != 0) && m_hitchLines < (m_hitchAll ? 200000u : 5000u))
                {
                    ++m_hitchLines;
                    std::fprintf(stderr,
                                 "[hitch] frame %llu: %.1f ms since the last (GS %.1f, present %.1f; shaders %u "
                                 "compiled %.1f ms, %u from the cache %.1f ms; GPU waits %u %.1f ms; VRAM reads %u "
                                 "%.1f ms, copies collected %.1f ms; events %.1f ms, longest %.1f; waiting for the EE %.1f ms; EE: VBlank after %.1f ms, asleep %.1f ms of it, waited %.1f ms, "
                                 "VRAM reads %u waited %.1f ms, VU1 %u programs %.1f ms, longest %.1f, waited for the "
                                 "VU1 thread %.1f ms, disc reads %.1f ms; same picture for %u)\n",
                                 static_cast<unsigned long long>(m_frames), since, ms(m_frameGs).count(),
                                 ms(present).count(), stalls.compiled, stalls.compiledMs, stalls.loaded, stalls.loadedMs,
                                 stalls.gpuWaits, stalls.gpuWaitMs, m_frameReadCount, ms(m_frameReads).count(),
                                 ms(m_frameCollect).count(), ms(m_framePump).count(), ms(m_framePumpMax).count(),
                                 ms(m_frameIdle).count(), ee.sinceLast / 1000.0,
                                 ee.idle / 1000.0, ee.vblankWait / 1000.0, ee.reads, ee.readWait / 1000.0, ee.vuRuns,
                                 ee.vuTotal / 1000.0, ee.vuLongest / 1000.0, ee.vifWait / 1000.0, ee.discWait / 1000.0,
                                 m_samePicture);
                }
                // The EE's own work in the frame: its time but for waiting on
                // the clock, VRAM reads and the VIF1 thread (sinceLast already
                // leaves out the wait for the GS at the VBlank before).
                const double eeBusy =
                    std::max(0.0, (static_cast<double>(ee.sinceLast) - ee.idle - ee.readWait - ee.vifWait) / 1000.0);
                pace(since, ms(m_frameGs).count(), ms(present).count(), ee.sinceLast / 1000.0, eeBusy,
                     ee.vuTotal / 1000.0, ee.vuRuns, ee.vifBusy / 1000.0, ee.vifWait / 1000.0);
                m_frameGs = {};
                m_frameReads = {};
                m_frameCollect = {};
                m_frameIdle = {};
                m_frameReadCount = 0;
                m_framePump = {};
                m_framePumpMax = {};
            }

            // Every 10 s, how evenly the pictures came: the VBlanks, how many
            // brought a new picture from the game and how many showed the
            // last one again, how many were shown more than 20 ms after the
            // one before, and the GS's, the presentation's and the EE's times
            // (average and longest). A frame a little late, or a picture the
            // game did not finish in time, is a small stutter the hitch log
            // (25 ms and up) does not list.
            void pace(double since, double gs, double present, double ee, double eeBusy, double vu, uint32_t vuRuns,
                      double vif, double vifWait)
            {
                if (m_frames < 300)
                    return;
                ++m_paceFrames;
                m_paceVif += vif;
                m_paceVifWait += vifWait;
                m_paceVifMax = std::max(m_paceVifMax, vif);
                m_paceEe += ee;
                m_paceEeBusy += eeBusy;
                m_paceEeBusyMax = std::max(m_paceEeBusyMax, eeBusy);
                m_paceVu += vu;
                m_paceVuRuns += vuRuns;
                m_paceRepeats += m_samePicture != 0 ? 1u : 0u;
                m_paceLate += since > 20.0 ? 1u : 0u;
                m_paceGs += gs;
                m_paceGsMax = std::max(m_paceGsMax, gs);
                m_pacePresent += present;
                m_pacePresentMax = std::max(m_pacePresentMax, present);
                m_paceSinceMax = std::max(m_paceSinceMax, since);
                m_paceSince += since;
                m_paceSince2 += since * since;
                m_paceEeMax = std::max(m_paceEeMax, ee);
                if (m_paceFrames < 600)
                    return;
                if (m_paceLines < 2000)
                {
                    ++m_paceLines;
                    // The spread of the intervals between pictures: what an
                    // uneven picture with no slow frame shows up as.
                    const double mean = m_paceSince / m_paceFrames;
                    const double spread = std::sqrt(std::max(0.0, m_paceSince2 / m_paceFrames - mean * mean));
                    const pcsx2gs::VideoMemory vram = pcsx2gs::videoMemory();
                    const auto mb = [](uint64_t bytes) { return static_cast<unsigned>(bytes >> 20); };
                    std::fprintf(stderr,
                                 "[pace] frame %llu, last %u VBlanks: %u repeated the picture, %u shown over 20 ms "
                                 "(longest %.1f), every %.2f ms +- %.2f; GS %.1f ms avg, %.1f max; present %.1f avg, "
                                 "%.1f max; EE %.1f avg, longest %.1f, working %.1f avg, %.1f max; VU1 %.1f ms, %u programs a frame, "
                                 "%u over 1 ms, %u microcode uploads, %llu KB compiled, %u cache resets; VU1 thread %.1f ms avg, %.1f max, EE waited for it %.1f; "
                                 "memory %u MB, VRAM %u MB "
                                 "(textures %u, targets %u, hashed %u, pool %u)\n",
                                 static_cast<unsigned long long>(m_frames), m_paceFrames, m_paceRepeats, m_paceLate,
                                 m_paceSinceMax, mean, spread, m_paceGs / m_paceFrames, m_paceGsMax,
                                 m_pacePresent / m_paceFrames, m_pacePresentMax, m_paceEe / m_paceFrames, m_paceEeMax,
                                 m_paceEeBusy / m_paceFrames, m_paceEeBusyMax, m_paceVu / m_paceFrames,
                                 m_paceVuRuns / m_paceFrames,
                                 m_vuSlowRuns.exchange(0, std::memory_order_relaxed),
                                 m_vuClears.exchange(0, std::memory_order_relaxed),
                                 static_cast<unsigned long long>(m_vuCompiled.exchange(0, std::memory_order_relaxed) / 1024),
                                 m_vuResets.exchange(0, std::memory_order_relaxed), m_paceVif / m_paceFrames, m_paceVifMax,
                                 m_paceVifWait / m_paceFrames, processMegabytes(),
                                 mb(vram.sources + vram.targets + vram.hashed + vram.pool), mb(vram.sources),
                                 mb(vram.targets), mb(vram.hashed), mb(vram.pool));
                }
                m_paceFrames = m_paceRepeats = m_paceLate = 0;
                m_paceGs = m_paceGsMax = m_pacePresent = m_pacePresentMax = m_paceSinceMax = m_paceEeMax = 0.0;
                m_paceSince = m_paceSince2 = m_paceEe = m_paceVu = m_paceEeBusy = m_paceEeBusyMax = 0.0;
                m_paceVif = m_paceVifMax = m_paceVifWait = 0.0;
                m_paceVuRuns = 0;
            }

            // GH1_DISPTRACE=<from>-<to>: per frame, the display registers at
            // VBlank and the framebuffers (FRAME_1/2 FBP) drawn to before it.
            void traceFrames(uint32_t path, const uint8_t *data, uint32_t bytes)
            {
                // FNV-1a of each path's data this frame, and its size.
                uint64_t &h = m_traceHash[path & 3u];
                if (h == 0)
                    h = 1469598103934665603ull;
                for (uint32_t i = 0; i < bytes; ++i)
                    h = (h ^ data[i]) * 1099511628211ull;
                m_traceBytes[path & 3u] += bytes;
                // GH1_DISPDUMP=<dir>: each traced frame's packets, {u32 path, u32 bytes, data}.
                static const char *dumpDir = std::getenv("GH1_DISPDUMP");
                if (dumpDir)
                {
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/gif_%06llu.bin", dumpDir, (unsigned long long)m_frames);
                    if (std::FILE *f = std::fopen(name, "ab"))
                    {
                        std::fwrite(&path, 4, 1, f);
                        std::fwrite(&bytes, 4, 1, f);
                        std::fwrite(data, 1, bytes, f);
                        std::fclose(f);
                    }
                }
                ++m_tracePackets[path & 3u];
                uint32_t at = 0;
                while (at + 16u <= bytes)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data + at, 8);
                    std::memcpy(&hi, data + at + 8, 8);
                    at += 16u;
                    const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                    uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    if (flg == 0u)
                    {
                        for (uint32_t i = 0; i < nloop * nreg && at + 16u <= bytes; ++i, at += 16u)
                        {
                            if (((hi >> ((i % nreg) * 4u)) & 0xFu) != 0xEu)
                                continue;
                            uint64_t v, a;
                            std::memcpy(&v, data + at, 8);
                            std::memcpy(&a, data + at + 8, 8);
                            const uint32_t reg = static_cast<uint32_t>(a & 0xFFu);
                            char item[48];
                            item[0] = 0;
                            if (reg == 0x4C || reg == 0x4D) // FRAME_1/2: FBP, FBW, PSM
                                std::snprintf(item, sizeof(item), "F%u:%x/%u/%x", path, unsigned(v & 0x1FF),
                                              unsigned((v >> 16) & 0x3F), unsigned((v >> 24) & 0x3F));
                            else if (reg == 0x4E || reg == 0x4F) // ZBUF_1/2: ZBP, PSM, ZMSK
                                std::snprintf(item, sizeof(item), "Z%u:%x/%x/%u", path, unsigned(v & 0x1FF),
                                              unsigned((v >> 24) & 0xF), unsigned((v >> 32) & 1));
                            else if (reg == 0x06 || reg == 0x07) // TEX0_1/2: TBP0, TBW, PSM
                                std::snprintf(item, sizeof(item), "T%u:%x/%u/%x", path, unsigned(v & 0x3FFF),
                                              unsigned((v >> 14) & 0x3F), unsigned((v >> 20) & 0x3F));
                            else if (reg == 0x40 || reg == 0x41) // SCISSOR
                                std::snprintf(item, sizeof(item), "S%u:%u-%u,%u-%u", path, unsigned(v & 0x7FF),
                                              unsigned((v >> 16) & 0x7FF), unsigned((v >> 32) & 0x7FF), unsigned((v >> 48) & 0x7FF));
                            else if (reg == 0x18 || reg == 0x19) // XYOFFSET
                                std::snprintf(item, sizeof(item), "O%u:%u,%u", path, unsigned(v & 0xFFFF) >> 4,
                                              unsigned((v >> 32) & 0xFFFF) >> 4);
                            else if (reg == 0x47 || reg == 0x48) // TEST
                                std::snprintf(item, sizeof(item), "E%u:%llx", path, (unsigned long long)(v & 0x7FFFF));
                            else if (reg == 0x50) // BITBLTBUF: DBP, DBW, DPSM
                                std::snprintf(item, sizeof(item), "U%u:%x/%u/%x", path, unsigned((v >> 32) & 0x3FFF),
                                              unsigned((v >> 48) & 0x3F), unsigned((v >> 56) & 0x3F));
                            if (item[0])
                                m_traceItems[item]++;
                        }
                    }
                    else if (flg == 1u)
                        at += ((nloop * nreg + 1u) / 2u) * 16u;
                    else
                        at += nloop * 16u;
                }
            }

            void traceVsync(const uint8_t *regs)
            {
                auto reg = [&](uint32_t off) { uint64_t v; std::memcpy(&v, regs + off, 8); return v; };
                std::string fbps;
                for (uint32_t p = 1; p <= 3; ++p)
                {
                    char t[96];
                    std::snprintf(t, sizeof(t), " P%u:%up/%ub/%016llx", p, m_tracePackets[p], m_traceBytes[p],
                                  (unsigned long long)m_traceHash[p]);
                    fbps += t;
                    m_traceHash[p] = 0;
                    m_traceBytes[p] = 0;
                    m_tracePackets[p] = 0;
                }
                for (const auto &[item, n] : m_traceItems)
                    fbps += " " + item + "x" + std::to_string(n);
                std::printf("[disp] %llu pmode %llx smode2 %llx dispfb1 %llx display1 %llx dispfb2 %llx display2 %llx csr %llx drew%s\n",
                            static_cast<unsigned long long>(m_frames),
                            (unsigned long long)reg(0x0), (unsigned long long)reg(0x20), (unsigned long long)reg(0x70),
                            (unsigned long long)reg(0x80), (unsigned long long)reg(0x90), (unsigned long long)reg(0xA0),
                            (unsigned long long)reg(0x1000), fbps.c_str());
                m_traceItems.clear();
            }

            void presented()
            {
                ++m_frames;
                const auto &options = m_runtime->hostOptions();
                if (options.shotEvery != 0 && m_frames % options.shotEvery == 0 && m_frames >= m_shotFrom)
                    shot();
                if (m_config.stats)
                {
                    const auto now = std::chrono::steady_clock::now();
                    const double secs = std::chrono::duration<double>(now - m_statsStart).count();
                    if (secs >= 5.0)
                    {
                        std::printf("[gs] %.1f fps\n", (m_frames - m_statsFrames) / secs);
                        std::fflush(stdout);
                        m_statsStart = now;
                        m_statsFrames = m_frames;
                    }
                }
            }

            void shot()
            {
                std::vector<uint32_t> rgba;
                uint32_t w = 0, h = 0;
                if (!pcsx2gs::snapshot(rgba, w, h) || rgba.empty())
                    return;
                SDL_Surface *surface = SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h),
                                                             SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                                             static_cast<int>(w * 4));
                if (!surface)
                    return;
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(m_frames));
                SDL_SavePNG(surface, (m_runtime->hostOptions().shotDir / name).string().c_str());
                SDL_DestroySurface(surface);
            }

            bool pumpEvents()
            {
                bool keep = true;
                SDL_Event ev;
                // SDL's own part (window messages, controllers) timed apart
                // from handling the events, for the log: a pump that holds
                // this thread up holds the picture up too.
                const auto pumpFrom = std::chrono::steady_clock::now();
                thread_local stallprobe::Slot *probe = stallprobe::slot("events");
                stallprobe::begin(probe);
                SDL_PumpEvents();
                const auto sdlPart = std::chrono::steady_clock::now() - pumpFrom;
                std::string where;
                stallprobe::end(probe, 100.0, where);
                uint32_t eventCount = 0;
                char eventTypes[96] = {};
                size_t eventTypesUsed = 0;
                while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1)
                {
                    ++eventCount;
                    if (eventTypesUsed + 8 < sizeof(eventTypes))
                        eventTypesUsed += static_cast<size_t>(std::snprintf(
                            eventTypes + eventTypesUsed, sizeof(eventTypes) - eventTypesUsed, " %x", ev.type));
                    if (m_options.handle(ev))
                        continue; // the options window's own
                    switch (ev.type)
                    {
                    case SDL_EVENT_QUIT:
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        keep = false;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        // F11 or Alt+Enter: borderless fullscreen on the desktop, and back.
                        if (!ev.key.repeat && (ev.key.key == SDLK_F11 ||
                                               (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                        {
                            const bool full = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
                            SDL_SetWindowFullscreen(m_window, !full);
                        }
                        // F1: the keyboard options, on the main menu only.
                        if (!ev.key.repeat && ev.key.key == SDLK_F1 &&
                            s_screen.load(std::memory_order_acquire) == Screen::MainMenu)
                        {
                            // Over the game in fullscreen, the options window
                            // stayed unseen until something was clicked (an
                            // OpenGL window filling the screen on Windows):
                            // the game leaves fullscreen while it is open,
                            // and goes back when it closes.
                            if (!m_options.isOpen() && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN))
                            {
                                SDL_SetWindowFullscreen(m_window, false);
                                SDL_SyncWindow(m_window);
                                m_fullscreenAfterOptions = true;
                                std::fprintf(stderr, "[host] F1: out of fullscreen while the options are open\n");
                            }
                            m_options.toggle(m_window);
                        }
                        break;
                    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                        if (ev.window.windowID == SDL_GetWindowID(m_window))
                            matchRefresh();
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        if (ev.window.windowID == SDL_GetWindowID(m_window))
                            pcsx2gs::resize(static_cast<uint32_t>(ev.window.data1),
                                            static_cast<uint32_t>(ev.window.data2), SDL_GetWindowDisplayScale(m_window));
                        break;
                    case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        // Every controller, guitars that are not SDL gamepads included.
                        guitars::Device d;
                        if (guitars::open(ev.jdevice.which, d))
                            m_devices.push_back(d);
                        break;
                    }
                    case SDL_EVENT_JOYSTICK_REMOVED:
                        for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
                        {
                            if (it->id == ev.jdevice.which)
                            {
                                guitars::close(*it);
                                m_devices.erase(it);
                                break;
                            }
                        }
                        break;
                    default:
                        break;
                    }
                }
                updatePad();
                if (m_options.isOpen() && s_screen.load(std::memory_order_acquire) != Screen::MainMenu)
                    m_options.close(); // the game left the main menu
                if (m_fullscreenAfterOptions && !m_options.isOpen())
                {
                    m_fullscreenAfterOptions = false;
                    SDL_SetWindowFullscreen(m_window, true);
                    SDL_RaiseWindow(m_window);
                    std::fprintf(stderr, "[host] options closed: back to fullscreen\n");
                }
                m_options.draw();
                using ms = std::chrono::duration<double, std::milli>;
                const auto total = std::chrono::steady_clock::now() - pumpFrom;
                if (total > std::chrono::milliseconds(20) && m_slowPumpLines < 500)
                {
                    ++m_slowPumpLines;
                    std::fprintf(stderr, "[host] events took %.1f ms at frame %llu (SDL's own %.1f ms; %u events:%s)%s%s\n",
                                 ms(total).count(), static_cast<unsigned long long>(m_frames), ms(sdlPart).count(),
                                 eventCount, eventTypes, where.empty() ? "" : "; ", where.c_str());
                }
                return keep;
            }

            void updatePad()
            {
                uint16_t pressed = 0;
                int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;

                // A PS2 guitar on a PS2-to-USB adapter ("Twin USB Joystick")
                // is the adapter's gamepad, holding D-Left as every PS2 guitar
                // does, from the moment it is there: a gamepad that holds it
                // from the first second after its first button (an adapter
                // may tell nothing until a button changes, or have its port
                // empty for a while; a button, since an empty port may hold
                // the stick to the left) for kAdapterGuitarFrames is that
                // guitar, and its buttons and sticks pass through as is. Many adapters
                // give a pad in digital mode (as a guitar is: nothing on the
                // adapter's side turns it analog) its d-pad as the left stick:
                // D-Left is then the stick held to the left, and the guitar's
                // d-pad (the strum) is read from the stick. One that does
                // not hold it, or lets go, is a DualShock for good (so a
                // DualShock's D-Left never makes it a guitar for long).
                // gamepad = guitar or dualshock in the ini decides instead.
                for (guitars::Device &d : m_devices)
                {
                    if (d.guitar || !d.gamepad || d.notGuitar || m_config.gamepadAs != GamepadAs::Auto)
                        continue;
                    const bool hatLeft = SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
                    const bool stickLeft = SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_LEFTX) <= -kStickDpad;
                    if (!d.heard)
                    {
                        // A button: the stick alone does not count, since an
                        // empty port of a Twin USB adapter sends the stick
                        // held to the left as a guitar does.
                        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !d.heard; ++b)
                            d.heard = SDL_GetGamepadButton(d.gamepad, static_cast<SDL_GamepadButton>(b));
                        if (!d.heard)
                            continue;
                    }
                    ++d.age;
                    const bool wasGuitar = d.leftHeld >= kAdapterGuitarFrames;
                    if (hatLeft || stickLeft)
                    {
                        d.dpadOnStick = !hatLeft;
                        d.leftFree = 0;
                        if (d.leftHeld < kAdapterGuitarFrames)
                            ++d.leftHeld;
                    }
                    else if (wasGuitar)
                    {
                        // Let go for a while (not for a strum on an adapter
                        // that drops it then): a DualShock after all.
                        if (++d.leftFree >= kAdapterLetGoFrames)
                            d.notGuitar = true;
                    }
                    else if (d.age > kAdapterGraceFrames)
                    {
                        d.notGuitar = true;
                    }
                    else
                    {
                        d.leftHeld = 0;
                    }
                    const bool isGuitar = d.leftHeld >= kAdapterGuitarFrames && !d.notGuitar;
                    if (isGuitar != wasGuitar || (d.notGuitar && wasGuitar))
                        std::fprintf(stderr, "[host] controller: %s: %s\n", SDL_GetGamepadName(d.gamepad),
                                     !isGuitar        ? "a DualShock 2 (D-Left let go)"
                                     : d.dpadOnStick ? "a PS2 guitar on an adapter (D-Left held, the d-pad on the left stick)"
                                                     : "a PS2 guitar on an adapter (D-Left held)");
                    else if (d.notGuitar)
                        std::fprintf(stderr, "[host] controller: %s: a DualShock 2 (no D-Left held)\n",
                                     SDL_GetGamepadName(d.gamepad));
                }
                // What each gamepad that is no known guitar sends, raw (its
                // buttons, hats and axes as the adapter numbers them, an axis
                // as -1, 0 or 1 by where it is), each time it changes, up to
                // kRawLines lines a gamepad: what a PS2 guitar on an adapter
                // turns into is in the log.
                for (guitars::Device &d : m_devices)
                {
                    if (d.guitar || !d.joystick || d.rawLines >= kRawLines)
                        continue;
                    std::string now;
                    char part[32];
                    uint32_t buttons = 0;
                    for (int b = 0; b < std::min(SDL_GetNumJoystickButtons(d.joystick), 32); ++b)
                        if (SDL_GetJoystickButton(d.joystick, b))
                            buttons |= 1u << b;
                    std::snprintf(part, sizeof(part), "buttons %x", buttons);
                    now += part;
                    for (int h = 0; h < std::min(SDL_GetNumJoystickHats(d.joystick), 4); ++h)
                    {
                        std::snprintf(part, sizeof(part), " hat%d %x", h, SDL_GetJoystickHat(d.joystick, h));
                        now += part;
                    }
                    now += " axes";
                    for (int a = 0; a < std::min(SDL_GetNumJoystickAxes(d.joystick), 8); ++a)
                    {
                        const int v = SDL_GetJoystickAxis(d.joystick, a);
                        now += v <= -kStickDpad ? " -1" : v >= kStickDpad ? " 1" : " 0";
                    }
                    if (now != d.rawLast)
                    {
                        d.rawLast = now;
                        ++d.rawLines;
                        std::fprintf(stderr, "[host] raw %s #%u frame %llu: %s\n", SDL_GetJoystickName(d.joystick),
                                     static_cast<unsigned>(d.id), static_cast<unsigned long long>(m_frames), now.c_str());
                    }
                }
                auto adapterGuitar = [this](const guitars::Device &d)
                {
                    if (d.guitar || !d.gamepad)
                        return false;
                    if (m_config.gamepadAs != GamepadAs::Auto)
                        return m_config.gamepadAs == GamepadAs::Guitar;
                    return !d.notGuitar && d.leftHeld >= kAdapterGuitarFrames;
                };

                // What port 1 is. The keyboard turned on plays before any
                // controller; off, it plays only while none is connected.
                // Otherwise a guitar when one is connected, else a DualShock.
                const keyboard::Config &kb = m_config.keyboard;
                bool anyGuitar = false, anyPad = false;
                for (const guitars::Device &d : m_devices)
                    (d.guitar || adapterGuitar(d) ? anyGuitar : anyPad) = true;
                const Screen screen = s_screen.load(std::memory_order_acquire);
                // At start the keyboard is on exactly when no controller is
                // connected (controllers that show up in the first seconds,
                // as SDL finds them, count too); F1 changes it afterwards.
                const bool starting = m_frames < kStartupFrames;
                if (starting && !m_config.keyboardForced)
                    m_config.keyboard.on = !anyGuitar && !anyPad;
                const bool keyboardPlays = kb.on || (!anyGuitar && !anyPad);
                // Port 1 takes a new kind only on the main menu (and once at
                // start): elsewhere, a controller plugged in or out, or the
                // keyboard's settings, leave it as it is, and the keyboard
                // plays as the kind the port has.
                bool wantGuitar = m_config.guitar && (keyboardPlays ? !kb.noStrum : anyGuitar);
                // GH1_TEST_FLIP=1 (tests): F9 makes port 1 the other kind for
                // a few seconds, wherever the game is, as a controller swapped
                // and swapped back.
                // GH1_TEST_ADAPTER=guitar|guitar-stick|pad (tests): a virtual gamepad
                // plugged in at 2 s that tells nothing for 3 s (as an adapter
                // that reports only changes), then holds D-Left for good
                // (guitar: plus frets and strums now and then) or presses X
                // and later holds D-Left for 3 s (pad).
                static const char *testAdapter = std::getenv("GH1_TEST_ADAPTER");
                if (testAdapter)
                {
                    if (m_frames >= 120u && !m_testPadMade)
                    {
                        m_testPadMade = true;
                        SDL_VirtualJoystickDesc desc;
                        SDL_INIT_INTERFACE(&desc);
                        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
                        desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
                        desc.name = "Test adapter";
                        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
                        m_testPad = id ? SDL_OpenJoystick(id) : nullptr;
                    }
                    if (m_testPad)
                    {
                        const bool guitar = std::strcmp(testAdapter, "guitar") == 0;
                        const bool guitarStick = std::strcmp(testAdapter, "guitar-stick") == 0;
                        const uint64_t t = m_frames;
                        auto set = [&](SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(m_testPad, b, down); };
                        if (guitarStick)
                        {
                            // A guitar in digital mode on an adapter that gives it the d-pad as the stick.
                            SDL_SetJoystickVirtualAxis(m_testPad, SDL_GAMEPAD_AXIS_LEFTX, t >= 300u ? -32768 : 0);
                            SDL_SetJoystickVirtualAxis(m_testPad, SDL_GAMEPAD_AXIS_LEFTY,
                                                       t >= 600u && t % 120u < 6u ? 32767 : 0); // a strum down every 2 s
                            set(SDL_GAMEPAD_BUTTON_EAST, t >= 600u && t % 120u < 60u);           // red held for 1 s
                        }
                        else if (guitar)
                        {
                            set(SDL_GAMEPAD_BUTTON_DPAD_LEFT, t >= 300u);
                            set(SDL_GAMEPAD_BUTTON_DPAD_DOWN, t >= 600u && t % 120u < 6u); // a strum every 2 s
                            set(SDL_GAMEPAD_BUTTON_EAST, t >= 600u && t % 120u < 60u);     // red held for 1 s
                        }
                        else
                        {
                            set(SDL_GAMEPAD_BUTTON_SOUTH, t >= 300u && t < 310u);
                            set(SDL_GAMEPAD_BUTTON_DPAD_LEFT, t >= 600u && t < 780u);
                        }
                    }
                }
                static const bool testFlip = std::getenv("GH1_TEST_FLIP") != nullptr;
                bool scriptFlip = false;
                for (const ScriptStep &st : m_script)
                    scriptFlip |= st.flip && m_frames >= st.frame && m_frames < st.until;
                if (testFlip)
                {
                    const bool *k = SDL_GetKeyboardState(nullptr);
                    if (k[SDL_SCANCODE_F9] && m_testFlipUntil + 30u < m_frames)
                        m_testFlipUntil = m_frames + 150u;
                }
                if (scriptFlip || m_frames < m_testFlipUntil)
                    wantGuitar = !wantGuitar;
                const bool guitarPort =
                    (!m_identityKnown || starting || screen == Screen::MainMenu || testFlip || !m_script.empty())
                        ? wantGuitar
                        : m_guitarPort;
                if (!m_identityKnown || guitarPort != m_guitarPort)
                {
                    // A new kind of controller: unplugged for a moment, so the
                    // game looks at it again (not at boot, before it has looked).
                    if (m_identityKnown || !guitarPort)
                        m_unplugUntil = m_frames + kUnplugFrames;
                    m_guitarPort = guitarPort;
                    m_identityKnown = true;
                    std::fprintf(stderr, "[host] port 1: %s\n", guitarPort ? "guitar" : "DualShock 2");
                }
                s_portOne.store((m_frames >= m_unplugUntil ? 1u : 0u) | (guitarPort ? 2u : 0u),
                                std::memory_order_release);

                // Held whammy keys move the whammy the way a hand works the
                // bar or the stick: from rest to full and back, five times a
                // second.
                const uint64_t phase = m_frames % 12u;
                const float wave = static_cast<float>(phase < 6u ? phase : 12u - phase) / 6.0f;
                float whammy = 0.0f;
                bool keyWhammy = false;

                // A key held while the game goes between a song and the menus
                // counts again only once it is let go: Enter that resumes from
                // the pause menu must not then press Start in the song.
                const bool inSong = screen == Screen::Song;
                const bool *keys = SDL_GetKeyboardState(nullptr);
                if (inSong != m_keysInSong)
                {
                    m_keysInSong = inSong;
                    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                        m_keyLatched[code] = keys[code];
                }
                for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                    m_keyLatched[code] = m_keyLatched[code] && keys[code];
                // Keys pressed on the options screen are its own: the Enter
                // that closes it must not then confirm in the game's menu.
                // (It may have closed in this frame's events, with the key
                // still down: the frame after counts too.)
                if (m_options.isOpen() || m_optionsWereOpen)
                    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                        m_keyLatched[code] = m_keyLatched[code] || keys[code];
                m_optionsWereOpen = m_options.isOpen();

                if (keyboardPlays && !m_options.isOpen())
                {
                    // Alt+Enter is fullscreen, not a button.
                    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
                    auto down = [&](int code)
                    {
                        return code > 0 && code < SDL_SCANCODE_COUNT && keys[code] && !m_keyLatched[code] &&
                               !(alt && code == SDL_SCANCODE_RETURN);
                    };
                    if (inSong)
                    {
                        if (guitarPort)
                        {
                            static constexpr uint16_t kBits[keyboard::kStrumKeyCount] = {
                                kR2, kCircle, kTriangle, kCross, kSquare, kUp, kDown, kStart, kSelect, 0};
                            for (int k = 0; k < keyboard::kStrumKeyCount; ++k)
                                if (down(kb.strum[k]))
                                    pressed |= kBits[k];
                            keyWhammy = down(kb.strum[keyboard::kStrumWhammy]);
                        }
                        else
                        {
                            static constexpr uint16_t kBits[keyboard::kPadKeyCount] = {
                                kL2, kL1, kR1, kR2, kCross, kStart, kSelect, 0};
                            for (int k = 0; k < keyboard::kPadKeyCount; ++k)
                                if (down(kb.pad[k]))
                                    pressed |= kBits[k];
                            keyWhammy = down(kb.pad[keyboard::kPadWhammy]);
                        }
                    }
                    else
                    {
                        // The menus: confirm and back are what the port's kind
                        // selects and goes back with (green and red on a
                        // guitar; X and triangle on a DualShock).
                        const uint16_t kBits[keyboard::kMenuKeyCount] = {
                            kUp, kDown, kLeft, kRight, guitarPort ? kR2 : kCross,
                            guitarPort ? kCircle : kTriangle, kStart};
                        for (int k = 0; k < keyboard::kMenuKeyCount; ++k)
                            if (down(kb.menu[k]))
                                pressed |= kBits[k];
                        // Playing as a guitar, the fret keys are its frets in
                        // the menus too, as a guitar's are (green selects and
                        // red goes back there): the main menu's cheats are
                        // fret presses. A fret key that is also a menu key
                        // does only what the menus give it.
                        if (guitarPort)
                        {
                            static constexpr uint16_t kFretBits[] = {kR2, kCircle, kTriangle, kCross, kSquare};
                            for (int k = keyboard::kStrumGreen; k <= keyboard::kStrumOrange; ++k)
                                if (std::find(kb.menu.begin(), kb.menu.end(), kb.strum[k]) == kb.menu.end() &&
                                    down(kb.strum[k]))
                                    pressed |= kFretBits[k - keyboard::kStrumGreen];
                        }
                    }
                    if (keyWhammy)
                        whammy = wave;
                }

                // Controllers, unless the keyboard plays before them: guitars
                // as the PS2 guitar; other gamepads as a DualShock 2, its
                // buttons where SDL puts them (fixed: no rebinding).
                const guitars::Device *sticksFrom = nullptr; // the gamepad last used
                for (guitars::Device &d : m_devices)
                {
                    if (kb.on)
                        break;
                    if (d.guitar)
                    {
                        const guitars::State st = guitars::read(d);
                        pressed |= st.buttons;
                        whammy = std::max(whammy, st.whammy);
                        continue;
                    }
                    SDL_Gamepad *pad = d.gamepad;
                    uint16_t mine = 0;
                    auto btn = [&](SDL_GamepadButton b, uint16_t bit)
                    {
                        if (SDL_GetGamepadButton(pad, b))
                            mine |= bit;
                    };
                    btn(SDL_GAMEPAD_BUTTON_SOUTH, kCross);
                    btn(SDL_GAMEPAD_BUTTON_EAST, kCircle);
                    btn(SDL_GAMEPAD_BUTTON_WEST, kSquare);
                    btn(SDL_GAMEPAD_BUTTON_NORTH, kTriangle);
                    btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1);
                    btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1);
                    btn(SDL_GAMEPAD_BUTTON_BACK, kSelect);
                    btn(SDL_GAMEPAD_BUTTON_START, kStart);
                    btn(SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3);
                    btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_UP, kUp);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft);
                    btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight);
                    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
                        mine |= kL2;
                    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
                        mine |= kR2;
                    if (adapterGuitar(d))
                    {
                        // A guitar whose d-pad comes as the left stick (see
                        // above; with gamepad = guitar, known by the stick held
                        // to the left with no D-Left): the stick is its d-pad.
                        const Sint16 sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
                        const Sint16 sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
                        if (m_config.gamepadAs == GamepadAs::Guitar && (mine & kLeft) == 0 && sx <= -kStickDpad)
                            d.dpadOnStick = true;
                        if (d.dpadOnStick)
                        {
                            mine |= (sx <= -kStickDpad / 2 ? kLeft : 0) | (sx >= kStickDpad / 2 ? kRight : 0) |
                                    (sy <= -kStickDpad / 2 ? kUp : 0) | (sy >= kStickDpad / 2 ? kDown : 0);
                        }
                    }
                    pressed |= mine;
                    // The sticks come from the gamepad whose buttons were
                    // pressed last: a two-port adapter shows up as two
                    // gamepads, and the empty port's sticks may sit anywhere.
                    if (mine & ~(adapterGuitar(d) ? kLeft : 0))
                        d.lastPress = m_frames + 1u;
                    if (d.lastPress != 0u && (!sticksFrom || d.lastPress > sticksFrom->lastPress))
                        sticksFrom = &d;
                }
                if (sticksFrom)
                {
                    static constexpr SDL_GamepadAxis kAxes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
                                                                 SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY};
                    int *sticks[4] = {&lx, &ly, &rx, &ry};
                    for (int i = 0; i < 4; ++i)
                        *sticks[i] = std::clamp((SDL_GetGamepadAxis(sticksFrom->gamepad, kAxes[i]) + 32768) >> 8, 0, 255);
                }
                if (guitarPort && !(sticksFrom && adapterGuitar(*sticksFrom) && !sticksFrom->dpadOnStick))
                {
                    // PCSX2's PadGuitar: whammy on the left stick's Y, 0x7f at
                    // rest to 0x00, the rest of the sticks at 0x7f.
                    lx = rx = ry = 0x7f;
                    ly = 0x7f - static_cast<int>(whammy * 127.0f + 0.5f);
                }
                else if (keyWhammy)
                {
                    ly = 0x80 - static_cast<int>(whammy * 128.0f + 0.5f); // the left stick worked up
                }

                for (const ScriptStep &st : m_script)
                    if (m_frames >= st.frame && m_frames < st.until)
                    {
                        pressed |= st.buttons;
                        if (st.whammy)
                            ly = guitarPort ? 0x7f - static_cast<int>(wave * 127.0f) : 0x80 - static_cast<int>(wave * 128.0f);
                    }

                if (guitarPort)
                    pressed |= kLeft; // a guitar holds D-Left

                // GH1_HOSTPAD=1 (tests): what port 1 sends, when it changes.
                static const bool tracePad = std::getenv("GH1_HOSTPAD") != nullptr;
                if (tracePad)
                {
                    const uint64_t now = uint64_t(pressed) | (uint64_t(ly) << 16) | (uint64_t(inSong) << 24) |
                                         (uint64_t(guitarPort) << 25) | (uint64_t(keyboardPlays) << 26) |
                                         (uint64_t(keyWhammy) << 27);
                    if (now != m_lastPadTrace)
                        std::fprintf(stderr, "[host pad] frame %llu pressed %04x ly %02x song %d guitar %d kb %d whammy %d\n",
                                     static_cast<unsigned long long>(m_frames), pressed, ly, inSong, guitarPort,
                                     keyboardPlays, keyWhammy);
                    m_lastPadTrace = now;
                }

                const uint16_t buttons = static_cast<uint16_t>(~pressed);
                const uint64_t s = uint64_t(buttons) | (uint64_t(rx) << 16) | (uint64_t(ry) << 24) |
                                   (uint64_t(lx) << 32) | (uint64_t(ly) << 40);
                m_pad.store(s, std::memory_order_release);
            }

            HostConfig m_config;
            keyboard::OptionsWindow m_options{m_config.keyboard, m_config.iniPath};
            PS2Runtime *m_runtime = nullptr;
            SDL_Window *m_window = nullptr;
            std::vector<guitars::Device> m_devices;
            bool m_keysInSong = false;
            bool m_optionsWereOpen = false;
            bool m_fullscreenAfterOptions = false; // F1 left fullscreen: back to it when the options close
            SDL_Joystick *m_testPad = nullptr;     // GH1_TEST_ADAPTER's
            bool m_testPadMade = false;
            uint64_t m_testFlipUntil = 0;
            uint64_t m_lastPadTrace = 0;
            std::array<bool, SDL_SCANCODE_COUNT> m_keyLatched{};
            static constexpr uint64_t kUnplugFrames = 30;
            static constexpr uint64_t kStartupFrames = 120; // 2 s: controllers SDL finds at start
            static constexpr uint32_t kAdapterGuitarFrames = 120; // 2 s
            static constexpr uint32_t kAdapterGraceFrames = 60;   // its first second
            static constexpr uint32_t kAdapterLetGoFrames = 30;   // half a second
            static constexpr int kStickDpad = 24000;              // a d-pad on the stick: near its end
            static constexpr uint32_t kRawLines = 300;            // lines a gamepad of what it sends
            bool m_identityKnown = false;
            bool m_guitarPort = true;
            uint64_t m_unplugUntil = 0;
            std::atomic<uint64_t> m_pad{0x80808080FFFFull};

            std::mutex m_batchMutex;         // EE side: m_batch (taken before m_mutex)
            std::vector<uint8_t> m_batch;
            std::mutex m_mutex;
            std::condition_variable m_work;  // EE -> host: records queued
            std::condition_variable m_space; // host -> EE: vsyncs drained, reads done
            std::vector<uint8_t> m_pending;
            std::vector<uint8_t> m_draining;
            uint32_t m_queuedVsyncs = 0;
            bool m_stopping = false;
            bool m_hostWaiting = false; // the GS thread waits for work (m_mutex)

            uint64_t m_frames = 0;
            uint64_t m_exitAt = 0;
            uint64_t m_traceFrom = 0, m_traceTo = 0;
            std::map<std::string, uint32_t> m_traceItems;
            uint64_t m_traceHash[4] = {};
            uint32_t m_traceBytes[4] = {};
            uint32_t m_tracePackets[4] = {};
            std::vector<ScriptStep> m_script;
            uint64_t m_shotFrom = 0;
            uint64_t m_statsFrames = 0;
            std::chrono::steady_clock::time_point m_statsStart;

            // The hitch log (host side, then the EE's side).
            uint32_t m_paceFrames = 0, m_paceRepeats = 0, m_paceLate = 0, m_paceLines = 0;
            double m_paceGs = 0.0, m_paceGsMax = 0.0, m_pacePresent = 0.0, m_pacePresentMax = 0.0;
            double m_paceEeBusy = 0.0, m_paceEeBusyMax = 0.0;
            double m_paceVif = 0.0, m_paceVifMax = 0.0, m_paceVifWait = 0.0;
            double m_paceSinceMax = 0.0, m_paceEeMax = 0.0, m_paceSince = 0.0, m_paceSince2 = 0.0, m_paceEe = 0.0,
                   m_paceVu = 0.0;
            uint32_t m_paceVuRuns = 0;
            std::atomic<uint32_t> m_vuSlowRuns{0}, m_vuClears{0}, m_vuResets{0};
            std::atomic<uint64_t> m_vuCompiled{0};
            double m_hitchMs = 25.0;
            bool m_hitchAll = false;
            uint32_t m_hitchLines = 0;
            std::chrono::steady_clock::duration m_frameGs{};
            std::chrono::steady_clock::duration m_frameCollect{}; // late-read copies taken at the VBlank
            std::chrono::steady_clock::duration m_frameIdle{};    // this thread waiting for the EE's records
            std::chrono::steady_clock::duration m_frameReads{};
            uint32_t m_frameReadCount = 0;
            std::vector<uint8_t> m_lateScratch; // where late reads land (GS thread)
            std::chrono::steady_clock::time_point m_lastPresent;
            uint64_t m_lastShown = 0;
            uint32_t m_samePicture = 0;
            std::chrono::steady_clock::time_point m_lastPump;
            uint32_t m_slowPumpLines = 0;
            std::chrono::steady_clock::duration m_framePump{};
            std::chrono::steady_clock::duration m_framePumpMax{};
            std::chrono::steady_clock::time_point m_eeLastVblank; // EE thread from here on
            uint32_t m_eeWaitedUs = 0;
            std::atomic<uint32_t> m_eeReadWaitUs{0}; // VRAM reads come from the EE's GS side
            std::atomic<uint32_t> m_eeReads{0};
            std::atomic<uint32_t> m_postedVsyncs{0}; // VBlank records the VIF1 thread has yet to hand over
        };
    }

    void setScreen(Screen screen) { s_screen.store(screen, std::memory_order_release); }

    bool readLocalLate(uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
    {
        SdlHost *host = s_lateReads.load(std::memory_order_acquire);
        if (!host)
            return false;
        host->readLate(dst, bytes, bitbltbuf, trxpos, trxreg);
        return true;
    }

    PortOne portOne()
    {
        const uint32_t v = s_portOne.load(std::memory_order_acquire);
        return {(v & 1u) != 0u, (v & 2u) != 0u};
    }

    HostConfig hostConfigFromEnvironment()
    {
        HostConfig c;
        auto window = [&c](const char *w) {
            unsigned ww = 0, wh = 0;
            if (std::sscanf(w, "%ux%u", &ww, &wh) == 2 && ww >= 320 && wh >= 240)
            {
                c.windowWidth = ww;
                c.windowHeight = wh;
            }
        };
        auto flag = [](const std::string &v) { return !v.empty() && v != "0" && v != "false" && v != "no"; };
        auto trim = [](const std::string &v) {
            const size_t a = v.find_first_not_of(" \t\r\n");
            const size_t b = v.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        auto lower = [](std::string v) {
            for (char &ch : v)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        };
        auto gamepadAs = [&lower](const char *v)
        {
            const std::string g = lower(v);
            return g == "guitar" || g == "guitarra" ? GamepadAs::Guitar
                   : g == "dualshock"               ? GamepadAs::DualShock
                                                    : GamepadAs::Auto;
        };
        // renderer = auto|opengl|d3d11|d3d12|sw (hw is the old name of opengl);
        // the environment's GH1_RENDERER also takes null.
        auto renderer = [&c](const char *r) {
            const std::string v(r);
            c.software = v == "sw";
            c.null = v == "null";
            if (v == "auto")
                c.api = Api::Auto;
            else if (v == "d3d11" || v == "dx11")
                c.api = Api::Direct3D11;
            else if (v == "d3d12" || v == "dx12")
                c.api = Api::Direct3D12;
            else if (v == "opengl" || v == "gl" || v == "hw")
                c.api = Api::OpenGL;
        };

        // GH1Recomp.ini beside the exe: the settings before any [section];
        // the keyboard's (keyboard.h) from all of it.
        std::string text;
        if (const char *base = SDL_GetBasePath())
        {
            c.iniPath = std::string(base) + GHR_INI;
            std::ifstream f(keyboard::utf8Path(c.iniPath), std::ios::binary);
            std::ostringstream all;
            all << f.rdbuf();
            text = all.str();
        }
        std::istringstream in(text);
        std::string line;
        bool inSection = false;
        while (std::getline(in, line))
        {
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            line = trim(line);
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
                inSection = true;
            const size_t eq = line.find('=');
            if (inSection || eq == std::string::npos)
                continue;
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "upscale")
                c.upscale = std::clamp(static_cast<float>(std::atof(value.c_str())), 1.0f, 8.0f);
            else if (key == "renderer")
                renderer(lower(value).c_str());
            else if (key == "vsync")
                c.vsync = flag(value);
            else if (key == "window")
                window(value.c_str());
            else if (key == "fullscreen")
                c.fullscreen = flag(value);
            else if (key == "venue")
                c.venue = lower(value) != "off" && flag(value);
            else if (key == "audio_offset")
                c.audioOffsetMs = std::clamp(std::atoi(value.c_str()), -500, 500);
            else if (key == "vu1_thread")
                c.vu1Thread = lower(value) == "auto" ? -1 : (lower(value) != "off" && flag(value)) ? 1 : 0;
            else if (key == "priority")
                c.raisePriority = lower(value) != "normal";
            else if (key == "gamepad")
                c.gamepadAs = gamepadAs(value.c_str());
        }
        std::string problems;
        keyboard::read(text, c.keyboard, problems);
        if (!problems.empty())
        {
            const std::string message = GHR_INI ": estas teclas nao foram aceitas; essa parte do teclado "
                                        "fica com as teclas padrao (F1 no jogo para mudar).\n\n" + problems;
            std::fprintf(stderr, "[host] %s", message.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, GHR_TITLE, message.c_str(), nullptr);
        }

        c.upscale = std::clamp(envFloat("GH1_UPSCALE", c.upscale), 1.0f, 8.0f);
        if (const char *r = std::getenv("GH1_RENDERER"))
            renderer(r);
        if (std::getenv("GH1_VSYNC"))
            c.vsync = envFlag("GH1_VSYNC");
        if (std::getenv("GH1_FULLSCREEN"))
            c.fullscreen = envFlag("GH1_FULLSCREEN");
        if (const char *v = std::getenv("GH1_VENUE"))
            c.venue = std::strcmp(v, "off") != 0 && envFlag("GH1_VENUE");
        if (const char *v = std::getenv("GH1_AUDIO_OFFSET"))
            c.audioOffsetMs = std::clamp(std::atoi(v), -500, 500);
        if (const char *g = std::getenv("GH1_GAMEPAD"))
            c.gamepadAs = gamepadAs(g);
        if (std::getenv("GH1_VU1_THREAD"))
            c.vu1Thread = envFlag("GH1_VU1_THREAD") ? 1 : 0;
        if (const char *p = std::getenv("GH1_PRIORITY"))
            c.raisePriority = lower(p) != "normal";
        c.stats = envFlag("GH1_STATS");
        c.guitar = !envFlag("GH1_JOYPAD");
        // GH1_KEYBOARD=strum|nostrum: the keyboard on, in that mode (tests).
        if (const char *k = std::getenv("GH1_KEYBOARD"))
        {
            c.keyboard.on = true;
            c.keyboardForced = true;
            c.keyboard.noStrum = std::strcmp(k, "nostrum") == 0 || std::strcmp(k, "dualshock") == 0;
        }
        if (const char *w = std::getenv("GH1_WINDOW"))
            window(w);
        return c;
    }

    std::unique_ptr<PS2Runtime::HostFrontend> makeSdlHost(const HostConfig &config)
    {
        return std::make_unique<SdlHost>(config);
    }
}
