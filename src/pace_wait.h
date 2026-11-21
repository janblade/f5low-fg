// pace_wait.h - the presenter's two Windows timing helpers, shared by the D3D12 presenter (xefg_own.h) and the pacer export (pace_api.h):
// WaitUntilQpc (a high-resolution timer for all but the last half millisecond, then a spin) and RaiseTimer (the 1 ms system timer while
// generating, timer_raise.h). Times are QPC ticks.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "timer_raise.h"

void XLog(const char* fmt, ...);

namespace pacewait
{
inline long long NowQpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline double FreqQpc()
{
    static const double f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return static_cast<double>(x.QuadPart);
    }();
    return f;
}

// Sleeps until a QPC time: a high-resolution waitable timer for all but the last half millisecond, then a
// spin. Sleep() alone would round up to the system timer tick (15.6 ms unless someone raised it).
inline void WaitUntilQpc(long long target)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;)
    {
        const double ms = static_cast<double>(target - NowQpc()) * 1000.0 / FreqQpc();
        if (ms <= 0)
            return;
        if (ms > 0.6 && timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((ms - 0.5) * 10000.0);       // relative, 100 ns units
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(timer, INFINITE);
            else
                Sleep(0);
        }
        else if (ms > 2)
            Sleep(1);
        else
            YieldProcessor();
    }
}

// A 1 ms system timer while generating (timer_raise.h): a game that never raises it has every Sleep(1) in the process
// round up to the 15.6 ms tick, and generated frames are lost with it (NBA 2K27 with the timer forced back: 3.5 shown per rendered at 4x, 4.0 raised). DLSS-G does the same.
// Called without the caller's own locks held: the first call in a process measures, up to some 50 ms. `mu` and `hold` are the caller's (one per context).
inline void RaiseTimer(std::mutex& mu, timerraise::Hold& hold, bool on)
{
    using TimerFn = UINT(WINAPI*)(UINT);
    // Once for the process; winmm stays loaded. By name: a winmm.dll proxy forwards these two.
    static const HMODULE winmm = LoadLibraryW(L"winmm.dll");
    static const TimerFn begin = winmm ? reinterpret_cast<TimerFn>(GetProcAddress(winmm, "timeBeginPeriod")) : nullptr;
    static const TimerFn end = winmm ? reinterpret_cast<TimerFn>(GetProcAddress(winmm, "timeEndPeriod")) : nullptr;
    static const auto sleep1 = [] {
        const long long t0 = NowQpc();
        Sleep(1);
        return static_cast<double>(NowQpc() - t0) * 1000.0 / FreqQpc();
    };
    static std::atomic<bool> measured { false };
    if (!begin || !end)
    {
        if (on && !measured.exchange(true))
            XLog("timer: no timeBeginPeriod in winmm.dll - the system timer stays as it is");
        return;
    }
    std::lock_guard<std::mutex> lk(mu);
    const bool first = on && !measured.exchange(true);
    const double before = first ? (std::max)(sleep1(), sleep1()) : 0.0;
    hold.Set(on, [&] { return begin(1) == 0; }, [&] { end(1); });
    if (first)
        XLog("timer: Sleep(1) took %.1f ms before the raise, %.1f ms with it (%s)", before, sleep1(),
             hold.Held() ? "1 ms requested" : "request refused");
}
} // namespace pacewait
