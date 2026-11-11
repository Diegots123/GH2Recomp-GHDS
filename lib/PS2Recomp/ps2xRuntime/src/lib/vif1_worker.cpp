#include "runtime/vif1_worker.h"

#include "runtime/ps2_memory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace ps2x::vif1
{
    namespace
    {
        struct Job
        {
            std::vector<uint8_t> data;
            std::function<void()> fn;
        };

        std::mutex s_mutex;
        std::condition_variable s_work; // the thread: something queued
        std::condition_variable s_done; // drain(): something finished
        std::deque<Job> s_queue;
        uint64_t s_posted = 0, s_finished = 0;
        bool s_stopping = false;
        std::thread s_thread;
        std::atomic<bool> s_active{false};
        std::atomic<uint64_t> s_busyNs{0};
        std::atomic<uint64_t> s_drainNs{0}; // other threads waiting in drain()
        PS2Memory *s_memory = nullptr;
        thread_local bool t_onWorker = false;

        void run()
        {
            t_onWorker = true;
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lock(s_mutex);
                    s_work.wait(lock, [] { return !s_queue.empty() || s_stopping; });
                    if (s_queue.empty())
                        break; // stopping, all done
                    job = std::move(s_queue.front());
                    s_queue.pop_front();
                }
                const auto t0 = std::chrono::steady_clock::now();
                try
                {
                    if (!job.data.empty())
                        s_memory->processVIF1Data(job.data.data(), static_cast<uint32_t>(job.data.size()));
                    if (job.fn)
                        job.fn();
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[vif1] %s\n", e.what());
                }
                const auto took = std::chrono::steady_clock::now() - t0;
                s_busyNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count()),
                                   std::memory_order_relaxed);
                // A job that held the picture up (the VU1 programs in it log
                // their own part, see the host's VU1 backend).
                static unsigned slowLines = 0;
                if (took > std::chrono::milliseconds(100) && slowLines < 100)
                {
                    ++slowLines;
                    size_t queued;
                    {
                        std::lock_guard lock(s_mutex);
                        queued = s_queue.size();
                    }
                    std::fprintf(stderr, "[vif1] a job took %.1f ms (%s, %zu bytes; %zu queued behind it)\n",
                                 std::chrono::duration<double, std::milli>(took).count(),
                                 job.data.empty() ? "a call" : "VIF1 data", job.data.size(), queued);
                }
                {
                    std::lock_guard lock(s_mutex);
                    ++s_finished;
                }
                s_done.notify_all();
            }
        }

        void enqueue(Job job)
        {
            {
                std::lock_guard lock(s_mutex);
                s_queue.push_back(std::move(job));
                ++s_posted;
            }
            s_work.notify_one();
        }
    }

    void start(PS2Memory &memory)
    {
        if (s_active.load())
            return;
        s_memory = &memory;
        s_stopping = false;
        s_thread = std::thread(run);
        s_active.store(true);
    }

    void stop()
    {
        if (!s_active.load() || t_onWorker)
            return;
        {
            std::lock_guard lock(s_mutex);
            s_stopping = true;
        }
        s_work.notify_one();
        if (s_thread.joinable())
            s_thread.join();
        s_active.store(false);
    }

    bool active() noexcept { return s_active.load(std::memory_order_acquire); }

    bool onWorker() noexcept { return t_onWorker; }

    void submit(std::vector<uint8_t> data)
    {
        if (data.empty())
            return;
        enqueue(Job{std::move(data), {}});
    }

    void post(std::function<void()> job)
    {
        if (!active() || t_onWorker)
        {
            job();
            return;
        }
        enqueue(Job{{}, std::move(job)});
    }

    void drain()
    {
        if (!active() || t_onWorker)
            return;
        std::unique_lock lock(s_mutex);
        const uint64_t target = s_posted;
        if (s_finished >= target || s_stopping)
            return;
        const auto from = std::chrono::steady_clock::now();
        s_done.wait(lock, [target] { return s_finished >= target || s_stopping; });
        s_drainNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - from)
                                                      .count()),
                            std::memory_order_relaxed);
    }

    uint64_t takeBusyNs() noexcept { return s_busyNs.exchange(0, std::memory_order_relaxed); }
    uint64_t takeDrainWaitNs() noexcept { return s_drainNs.exchange(0, std::memory_order_relaxed); }
}
