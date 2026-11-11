#include "stall_probe.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Where the thread was (its instruction pointer and stack, read with the
// thread suspended) only in builds made with GHR_STALL_SAMPLES: antivirus
// heuristics take an unsigned exe that reads other threads' contexts for
// malware, so the release build only times the thread and the memory.
#if defined(_WIN32) && defined(GHR_STALL_SAMPLES)
#define GHR_SAMPLES 1
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace stallprobe
{
    namespace
    {
        constexpr int kMaxLooks = 16;
        constexpr int kMaxFrames = 8;
        constexpr int64_t kFirstLookNs = 100'000'000;
        constexpr int64_t kLookEveryNs = 50'000'000;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        struct Look
        {
            int64_t atNs = 0; // into the section
            int64_t userNs = -1, kernelNs = -1; // the thread's processor time so far (all of it)
            uintptr_t pc = 0;
            uint32_t frames[kMaxFrames] = {}; // return addresses in the exe, as offsets from its base
            int frameCount = 0;
            int64_t pageFaults = -1; // the whole process's, so far
            int memoryLoad = -1;     // percent of the computer's memory in use
            int64_t freeMB = -1;     // physical memory available
        };
    }

    struct Slot
    {
        const char *name = "";
#if defined(_WIN32)
        HANDLE thread = nullptr;
        uintptr_t stackHigh = 0; // GHR_SAMPLES
#else
        clockid_t clock{};
        bool hasClock = false;
#endif
        std::atomic<int64_t> began{0}; // steady clock, 0 between sections
        std::atomic<uint64_t> serial{0};
        std::mutex lookMutex;          // the watchdog's looks at the section numbered lookSerial
        uint64_t lookSerial = 0;
        int lookCount = 0;
        int64_t lastLook = 0;
        Look looks[kMaxLooks];
    };

    namespace
    {
        std::mutex s_registry;
        std::vector<Slot *> s_slots;
        bool s_watching = false;

#if defined(_WIN32)
#if defined(GHR_SAMPLES)
        uintptr_t s_exeBase = 0, s_textLo = 0, s_textHi = 0;

        void findExeText()
        {
            const auto base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
            if (!base)
                return;
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
            s_exeBase = reinterpret_cast<uintptr_t>(base);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
                if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                {
                    const uintptr_t lo = s_exeBase + sec->VirtualAddress;
                    const uintptr_t hi = lo + sec->Misc.VirtualSize;
                    if (s_textLo == 0 || lo < s_textLo)
                        s_textLo = lo;
                    if (hi > s_textHi)
                        s_textHi = hi;
                }
        }

        // An address in the exe's code just after a call instruction: a
        // return address, most likely (call rel32, call [rip+x], call reg,
        // call [reg+x], with or without a REX prefix).
        bool returnAddress(uintptr_t a)
        {
            if (a < s_textLo + 8 || a >= s_textHi)
                return false;
            const auto p = reinterpret_cast<const uint8_t *>(a);
            if (p[-5] == 0xE8)
                return true;
            if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
                return true;
            if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
                return true;
            if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
                return true;
            if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)
                return true;
            return false;
        }

#endif

        void cpuTimes(Slot &s, int64_t &user, int64_t &kernel)
        {
            FILETIME c, e, k, u;
            if (s.thread && GetThreadTimes(s.thread, &c, &e, &k, &u))
            {
                kernel = static_cast<int64_t>((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) * 100;
                user = static_cast<int64_t>((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime) * 100;
            }
        }

#if defined(GHR_SAMPLES)
        // The thread stopped for a moment: where it is and what called it.
        // Nothing here may allocate or lock (the thread may hold the heap's
        // lock or the console's).
        void lookInside(Slot &s, Look &look)
        {
            static thread_local uint8_t stack[16384];
            if (SuspendThread(s.thread) == static_cast<DWORD>(-1))
                return;
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            size_t copied = 0;
            if (GetThreadContext(s.thread, &ctx))
            {
                look.pc = ctx.Rip;
                if (s.stackHigh > ctx.Rsp)
                {
                    copied = static_cast<size_t>(s.stackHigh - ctx.Rsp);
                    if (copied > sizeof(stack))
                        copied = sizeof(stack);
                    std::memcpy(stack, reinterpret_cast<const void *>(ctx.Rsp), copied);
                }
            }
            ResumeThread(s.thread);
            for (size_t at = 0; at + 8 <= copied && look.frameCount < kMaxFrames; at += 8)
            {
                uintptr_t v;
                std::memcpy(&v, stack + at, 8);
                if (returnAddress(v))
                    look.frames[look.frameCount++] = static_cast<uint32_t>(v - s_exeBase);
            }
        }

#else
        void lookInside(Slot &, Look &) {}
#endif

        // How hard the computer's memory is pressed: page faults (from the
        // disk, when memory runs short) stall a thread without it running.
        void memoryState(Look &look)
        {
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                look.pageFaults = pmc.PageFaultCount;
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms))
            {
                look.memoryLoad = static_cast<int>(ms.dwMemoryLoad);
                look.freeMB = static_cast<int64_t>(ms.ullAvailPhys >> 20);
            }
        }

#if defined(GHR_SAMPLES)
        void describePc(uintptr_t pc, char *out, size_t size)
        {
            HMODULE module = nullptr;
            if (pc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(pc), &module))
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(module, path, MAX_PATH);
                const char *name = std::strrchr(path, '\\');
                std::snprintf(out, size, "%s+%llx", name ? name + 1 : path,
                              static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(module)));
            }
            else
                std::snprintf(out, size, "%llx (generated code)", static_cast<unsigned long long>(pc));
        }
#endif
#else
        void cpuTimes(Slot &s, int64_t &user, int64_t &kernel)
        {
            timespec ts;
            if (s.hasClock && clock_gettime(s.clock, &ts) == 0)
            {
                user = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
                kernel = 0;
            }
        }

        void lookInside(Slot &, Look &) {}
        void memoryState(Look &) {}
#endif

        void watch()
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                std::lock_guard registry(s_registry);
                for (Slot *s : s_slots)
                {
                    const int64_t began = s->began.load(std::memory_order_acquire);
                    const int64_t now = nowNs();
                    if (began == 0 || now - began < kFirstLookNs)
                        continue;
                    std::lock_guard lock(s->lookMutex);
                    const uint64_t serial = s->serial.load(std::memory_order_acquire);
                    if (s->lookSerial != serial)
                    {
                        s->lookSerial = serial;
                        s->lookCount = 0;
                        s->lastLook = 0;
                    }
                    if (s->lookCount >= kMaxLooks || (s->lastLook != 0 && now - s->lastLook < kLookEveryNs))
                        continue;
                    Look &look = s->looks[s->lookCount];
                    look = Look{};
                    look.atNs = now - began;
                    cpuTimes(*s, look.userNs, look.kernelNs);
                    memoryState(look);
                    lookInside(*s, look);
                    s->lastLook = now;
                    ++s->lookCount;
                }
            }
        }
    }

    Slot *slot(const char *name)
    {
        thread_local std::vector<Slot *> mine;
        for (Slot *s : mine)
            if (std::strcmp(s->name, name) == 0)
                return s;
        Slot *s = new Slot();
        s->name = name;
#if defined(GHR_SAMPLES)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s->thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        s->stackHigh = high;
#elif defined(_WIN32)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s->thread,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
#else
        s->hasClock = pthread_getcpuclockid(pthread_self(), &s->clock) == 0;
#endif
        mine.push_back(s);
        std::lock_guard registry(s_registry);
        s_slots.push_back(s);
        if (!s_watching)
        {
#if defined(GHR_SAMPLES)
            findExeText();
#endif
            std::thread(watch).detach();
            s_watching = true;
        }
        return s;
    }

    void begin(Slot *s)
    {
        s->serial.fetch_add(1, std::memory_order_relaxed);
        s->began.store(nowNs(), std::memory_order_release);
    }

    double end(Slot *s, double minMs, std::string &report)
    {
        const int64_t began = s->began.exchange(0, std::memory_order_acq_rel);
        const int64_t took = nowNs() - began;
        if (began == 0 || took < static_cast<int64_t>(minMs * 1e6))
            return 0.0;
        int64_t user = -1, kernel = -1;
        cpuTimes(*s, user, kernel);
        char line[256];
        report.clear();
        std::lock_guard lock(s->lookMutex);
        if (s->lookSerial != s->serial.load(std::memory_order_relaxed) || s->lookCount == 0)
        {
            report = "no looks";
            return took / 1e6;
        }
        const Look &first = s->looks[0];
        if (first.userNs >= 0 && user >= 0)
        {
            const double wall = (took - first.atNs) / 1e6;
            std::snprintf(line, sizeof(line), "the thread ran %.0f of the last %.0f ms (user %.0f, kernel %.0f)",
                          ((user - first.userNs) + (kernel - first.kernelNs)) / 1e6, wall,
                          (user - first.userNs) / 1e6, (kernel - first.kernelNs) / 1e6);
            report += line;
        }
#if defined(_WIN32)
        Look last{};
        memoryState(last);
        if (first.pageFaults >= 0 && last.pageFaults >= 0)
        {
            std::snprintf(line, sizeof(line), "; %lld page faults in the process meanwhile, memory %d%% used (%lld MB free)",
                          static_cast<long long>(last.pageFaults - first.pageFaults), last.memoryLoad,
                          static_cast<long long>(last.freeMB));
            report += line;
        }
#if defined(GHR_SAMPLES)
        report += "; at";
        for (int i = 0; i < s->lookCount; ++i)
        {
            const Look &look = s->looks[i];
            char pc[160];
            describePc(look.pc, pc, sizeof(pc));
            std::snprintf(line, sizeof(line), " [+%lld ms %s <", static_cast<long long>(look.atNs / 1'000'000), pc);
            report += line;
            for (int f = 0; f < look.frameCount; ++f)
            {
                std::snprintf(line, sizeof(line), " %x", look.frames[f]);
                report += line;
            }
            report += "]";
        }
#endif
#endif
        return took / 1e6;
    }
}
