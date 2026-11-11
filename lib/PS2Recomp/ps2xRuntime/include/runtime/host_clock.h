#pragma once

// The host time guest deadlines are paced against: steady_clock, or a
// multiple of it for unattended runs faster than real time. Everything that
// paces the guest reads now() and waits until real(), so all of it speeds
// up together.

#include <chrono>

namespace ps2x::host_clock
{
    using Clock = std::chrono::steady_clock;

    namespace detail
    {
        inline double speed = 1.0;
        inline Clock::time_point origin = Clock::now();
    }

    // Before the runtime starts; time keeps running from now.
    inline void setSpeed(double speed)
    {
        detail::origin = Clock::now();
        detail::speed = speed;
    }

    inline double speed() { return detail::speed; }

    inline Clock::time_point now()
    {
        const Clock::time_point real = Clock::now();
        if (detail::speed == 1.0)
            return real;
        return detail::origin + std::chrono::duration_cast<Clock::duration>((real - detail::origin) * detail::speed);
    }

    // The real time at which now() reaches t, to wait until. Rounded up, so
    // now() on waking is never short of t.
    inline Clock::time_point real(Clock::time_point t)
    {
        if (detail::speed == 1.0 || t == Clock::time_point::max())
            return t;
        return detail::origin + std::chrono::ceil<Clock::duration>((t - detail::origin) / detail::speed);
    }
}
