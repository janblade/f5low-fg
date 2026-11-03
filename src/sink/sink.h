// sink.h - a swap chain a XeSS-FG library presents INTO, and our pacer in front of the real chain: the present-sink shape of
// docs/GENERATORS.md. Used by the dlssg generator (generators/dlssg.h); the intel generator will reuse it.
//
// Intel's libxess_fg presents a burst back to back from a worker thread of its own: the generated frames and the application's
// frame, 6 Present calls within a millisecond or two at 6x, then nothing until the next application frame. The sink takes those
// Present calls and lets them out on the real swap chain one spacing apart.
//
//   library -> SinkFactory::CreateSwapChainForHwnd -> a REAL chain exactly as the application asked for it
//   library -> SinkSwapChain (what the library sees as its swap chain)
//       GetCurrentBackBufferIndex: counts the frames taken, modulo the application's buffer count (the library hands this index on to the
//         application's own GetBuffer, so it must stay below that count - an index up to 11 crashed the application's GetBuffer)
//       GetBuffer(any index): the staging texture of the next frame (one of kSlots, same description as the real chain's buffers) - the
//         library asks for it right before each Present, so every frame of a burst gets a texture of its own
//       stable buffers (SinkFactory(real, true); DLSS-G through Streamline): the library takes its buffers once and then draws into
//         buffer[GetCurrentBackBufferIndex()], so GetBuffer(i) is always staging texture i; its Present returns once the next index's
//         texture is out of the queue and copied (what a real chain guarantees), so it never draws over a frame not yet shown
//       Present: queues the frame (sync, flags) and returns; blocks only while the queue is full
//       everything else: forwarded to the real chain
//   pacer thread: for each queued frame, a spacing after the last one: copy the staging texture into the real chain's current back
//       buffer (on the chain's queue, behind the library's work for it), then the real Present
//
// The spacing: the library's frames come in bursts of SetBurst() frames (generated + application); the time between burst starts is
// smoothed (1/8, one 1.5x hitch ignored at a time, three in a row off by 1.3x = a new pace) and divided by the burst size. With a burst
// of 1 (frame generation off) every frame goes out as it arrives. A burst starts when a frame comes a while after the library's last
// Present returned (not by counting frames): a game in a menu stops tagging and the library then presents one frame per application
// frame with frame generation still on (Cyberpunk); such short bursts go out as they arrive too and do not steer the pace.
//
// Schedule mode (Generator=intel or dlssg, F5LOW_SINK_PACE=schedule): instead of spacing a burst by its smoothed burst interval, the frames are
// released on the own generator's D3D12 schedule (pace_sched.h, pace_lead.h): the game side stamps every application Present
// (AppPresent), the stamps give the smoothed interval and each batch's base; frame j of a batch (generated 1..n, then the real one) goes out
// at base + pacelead::Offset(lead, step, j + 1), the lead frozen per batch and raised by generated frames whose staging copy was still
// running at their time (the same "late" the own generator feeds it). A frame past its time is presented on arrival and followed no
// sooner than half a step (Bound). Without stamps (no game-side wrapper) or before enough are seen, the burst spacing below applies.
//
// Only what Intel's 1.3.4.95 uses is implemented with care (see the plan, Story 3 step 1); the rest is forwarded.
#pragma once
#include "scproxy.h"
#include "../pace_lead.h"
#include "../pace_sched.h"
#include "release_rule.h"

#include <d3d12.h>

#include <condition_variable>
#include <deque>
#include <thread>

namespace xsink
{
inline LONGLONG Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline double Freq()
{
    static const double f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return static_cast<double>(q.QuadPart);
    }();
    return f;
}

// A test-harness trace switch (SINK_TRACE, SINK_TRACE2): set in the environment.
inline bool EnvSet(const char* name)
{
    char v[8];
    return GetEnvironmentVariableA(name, v, sizeof(v)) != 0;
}

// High-resolution timer for all but the last ~1 ms, then a spin.
inline void WaitUntil(LONGLONG target)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;)
    {
        const double ms = static_cast<double>(target - Now()) * 1000.0 / Freq();
        if (ms <= 0)
            return;
        if (ms > 1.5 && timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((ms - 1.0) * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }
        else
            YieldProcessor();
    }
}

// Statistics (gaps between the real Presents, GPU time per staging copy, SINK_PROBE) are collected when the harness turned the proxies
// on (scproxy::g_enabled) or F5LOW_SINK_STATS=1 is set (g_stats: the DLL logs them when its context is destroyed). Off in a game.
inline bool g_stats = false;
inline bool Collect() { return scproxy::g_enabled || g_stats; }

inline std::vector<double> g_outGaps;       // ms between the real Present calls, over the whole run (survives the chain)
inline std::mutex g_outMu;
inline std::mutex g_linkMu;                 // the factory <-> sink link (SinkFactory::last, SinkSwapChain::owner)
inline std::vector<double> g_outTimes;      // QPC ms of each real Present call's return (XEFG_CUDA_PACELOG lists them; tests/pacer_ab.ps1)
inline std::vector<double> g_copyGpuMs;     // GPU ms of each staging copy (timestamps around it)
inline std::vector<std::pair<uint64_t, int>> g_probe;    // SINK_PROBE (harness): frame number, x of the first bright-yellow pixel in row 540 (-1 none)
// SINK_PROBE_DUMP=<file> (harness): row 540 of the first frames shown, to compare two runs pixel by pixel (written with the statistics):
// u32 rows, u32 bytes per row, then per row u64 frame number + the row bytes. At most kProbeRows rows.
inline constexpr size_t kProbeRows = 1500;
inline std::vector<std::pair<uint64_t, std::vector<uint8_t>>> g_probeRows;

// PaceMode and the per-frame release decision (paced or on arrival) live in release_rule.h (host-tested).
struct PaceCfg
{
    PaceMode mode = PaceMode::Burst;
    bool leadAuto = true;               // PaceLead: automatic, else leadFixedMs (0 = off)
    double leadFixedMs = 0;
    bool wideCap = false;               // test: the lead may reach a whole app frame minus one step (n steps), not one step
    double testLeadMs = 0;              // test: a fixed lead, not clamped to a step (at most two intervals)
};
inline constexpr uint32_t kAutoMaxBurst = 2;    // Auto: the schedule only at 2x (A/B 2026-11-03: from 3x the first generated frame of a burst is late and gap sd/pm_sd are worse than the burst spacing)
inline void (*g_log)(const char* fmt, ...) = nullptr;
inline std::atomic<double> g_arrSum[8], g_arrMax[8];            // schedule mode: arrival after the app Present, per burst frame j
inline std::atomic<uint64_t> g_arrN[8], g_lateJ[8];
inline std::atomic<double> g_lateJSum[8];     // schedule mode: late generated frames per burst frame j, and how late (ms)

// Schedule mode statistics (the harness reads them with the sink's gaps)
inline std::atomic<uint64_t> g_schedBatches { 0 }, g_schedNoStamp { 0 }, g_schedFrames { 0 }, g_schedLate { 0 }, g_schedStamps { 0 };
inline double g_leadMin = 1e9, g_leadMax = 0;       // under g_outMu: the frozen lead's range (ms)

inline size_t g_depthStat = 0;              // the last sink's deepest queue and how often the library had to wait for room (set when it is released)
inline unsigned g_fullStat = 0;

inline void ResetGaps()
{
    std::lock_guard<std::mutex> l(g_outMu);
    g_outGaps.clear();
    g_outTimes.clear();
    g_copyGpuMs.clear();
    g_probe.clear();
    g_schedBatches = g_schedNoStamp = g_schedFrames = g_schedLate = g_schedStamps = 0;
    for (int i = 0; i < 8; ++i)
    {
        g_arrSum[i] = 0;
        g_arrMax[i] = 0;
        g_arrN[i] = 0;
        g_lateJ[i] = 0;
        g_lateJSum[i] = 0;
    }
    g_leadMin = 1e9;
    g_leadMax = 0;
    g_probeRows.clear();
}

// The statistics as text lines: the harness prints them, the DLL logs them.
inline std::vector<std::string> StatsLines()
{
    std::vector<std::string> lines;
    char buf[512];
    std::lock_guard<std::mutex> l(g_outMu);
    snprintf(buf, sizeof(buf), "sink: deepest queue %zu, the library waited for room %u times", g_depthStat, g_fullStat);
    lines.push_back(buf);
    if (!scproxy::GapText(buf, sizeof(buf), "sink: gaps between the real chain's Present calls", g_outGaps).empty())
        lines.push_back(buf);
    if (!scproxy::GapText(buf, sizeof(buf), "sink: GPU time per staging copy (values, not gaps)", g_copyGpuMs).empty())
        lines.push_back(buf);
    if (EnvSet("XEFG_CUDA_PACELOG"))
        for (const double t : g_outTimes)
        {
            snprintf(buf, sizeof(buf), "sink: pace t=%.3f", t);
            lines.push_back(buf);
        }
    if (g_schedBatches || g_schedStamps)
    {
        snprintf(buf, sizeof(buf), "sink: schedule: %llu app Presents stamped, %llu batches (%llu without a stamp), lead %.1f..%.1f ms, %llu late generated frames of %llu",
                 static_cast<unsigned long long>(g_schedStamps.load()), static_cast<unsigned long long>(g_schedBatches.load()), static_cast<unsigned long long>(g_schedNoStamp.load()), g_leadMin, g_leadMax,
                 static_cast<unsigned long long>(g_schedLate.load()), static_cast<unsigned long long>(g_schedFrames.load()));
        lines.push_back(buf);
    }
    if (g_schedLate)
    {
        std::string a = "sink: late generated frames per burst frame j (count, mean ms late):";
        for (int i = 0; i < 8 && g_arrN[i]; ++i)
        {
            snprintf(buf, sizeof(buf), "  j%d %llu (%.2f)", i, static_cast<unsigned long long>(g_lateJ[i].load()), g_lateJ[i] ? g_lateJSum[i].load() / static_cast<double>(g_lateJ[i].load()) : 0.0);
            a += buf;
        }
        lines.push_back(a);
    }
    if (g_arrN[0])
    {
        std::string a = "sink: arrival after the app Present, mean (max) ms per burst frame j:";
        for (int i = 0; i < 8 && g_arrN[i]; ++i)
        {
            snprintf(buf, sizeof(buf), "  j%d %.2f (%.2f)", i, g_arrSum[i].load() / static_cast<double>(g_arrN[i].load()), g_arrMax[i].load());
            a += buf;
        }
        lines.push_back(a);
    }
    if (g_probe.empty())
        return lines;
    if (char dumpPath[MAX_PATH]; !g_probeRows.empty() && GetEnvironmentVariableA("SINK_PROBE_DUMP", dumpPath, sizeof dumpPath))
    {
        std::sort(g_probeRows.begin(), g_probeRows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (FILE* fp = nullptr; fopen_s(&fp, dumpPath, "wb") == 0 && fp)
        {
            const uint32_t rows = static_cast<uint32_t>(g_probeRows.size()), bytes = static_cast<uint32_t>(g_probeRows[0].second.size());
            fwrite(&rows, 4, 1, fp);
            fwrite(&bytes, 4, 1, fp);
            for (const auto& r : g_probeRows)
            {
                fwrite(&r.first, 8, 1, fp);
                fwrite(r.second.data(), 1, r.second.size(), fp);
            }
            fclose(fp);
            lines.push_back("sink probe: " + std::to_string(rows) + " rows of row 540 written to " + dumpPath);
        }
    }
    // The test pattern's bar moves the same distance each frame: the steps between shown frames say whether they are in order
    std::sort(g_probe.begin(), g_probe.end());
    std::map<int, unsigned> steps;
    unsigned none = 0;
    for (size_t i = 1; i < g_probe.size(); ++i)
    {
        if (g_probe[i].second < 0 || g_probe[i - 1].second < 0)
        {
            ++none;
            continue;
        }
        int d = g_probe[i].second - g_probe[i - 1].second;
        if (d < -960)
            d += 1920;      // the bar wrapped round
        ++steps[d];
    }
    std::string t = "sink probe: " + std::to_string(g_probe.size()) + " frames, " + std::to_string(none) + " without the bar; steps between shown frames (px: count):";
    for (const auto& [d, c] : steps)
        if (c * 200 >= g_probe.size())
            t += "  " + std::to_string(d) + ": " + std::to_string(c);
    t += "  (steps under 0.5% left out)";
    lines.push_back(t);
    if (EnvSet("SINK_PROBE_DUMP"))      // tests: the bar's x of every frame shown, in order (which k of a burst a deviation hits)
    {
        std::string seq = "sink probe x:";
        for (const auto& pr : g_probe)
            seq += " " + std::to_string(pr.second);
        lines.push_back(seq);
    }
    return lines;
}

inline void PrintStats(FILE* out)
{
    for (const std::string& t : StatsLines())
        fprintf(out, "%s\n", t.c_str());
}

class SinkSwapChain final : public scproxy::LogSwapChain
{
    struct Item
    {
        UINT sync, flags, slot;     // slot: the staging texture holding the frame
        // Schedule mode: its planned release (QPC) and the batch it belongs to
        bool sched = false;
        LONGLONG due = 0, step = 0;
        uint32_t j = 0, b = 1;      // frame j (0 = first generated) of a batch of b
        double leadMs = 0, stepMs = 0;
        bool direct = false;        // on arrival: SinkPace=off or a display-paced present (release_rule.h); no timer wait, no lead rule
    };

    static constexpr UINT kSlots = 12;      // staging textures: a 6x burst + the one being shown + the one being filled + slack
    static constexpr UINT kMaxQueue = kSlots - 3;

    const UINT appBuffers;
    const bool stable;                  // GetBuffer(i) = staging texture i (see the top)
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12Resource* slot[kSlots] {};
    ID3D12CommandAllocator* alloc[kSlots] {};
    UINT64 allocFence[kSlots] {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    HANDLE libEvent = nullptr;          // the library's thread waiting for a copy (stable buffers); fenceEvent is the pacer's
    HANDLE alwaysSignaled = nullptr;    // what the library gets for GetFrameLatencyWaitableObject (see there)
    // GPU time of each copy (timestamps around it, read back when its allocator comes round again): g_copyGpuMs.
    ID3D12QueryHeap* qheap = nullptr;
    ID3D12Resource* qread = nullptr;
    UINT64 tsFreq = 0;
    bool tsPending[kSlots] {};
    ID3D12Resource* probe = nullptr;    // SINK_PROBE (harness): row 540 of each frame shown, read back when its slot comes round again
    UINT probePitch = 0;
    bool probePending[kSlots] {};
    uint64_t probeN[kSlots] {};

    std::mutex mu;
    std::condition_variable cvItem, cvSpace;
    std::deque<Item> q;
    bool stop = false;
    uint64_t in = 0;                    // frames accepted from the library
    uint64_t presented = 0;             // frames released on the real chain
    uint32_t burst = 1;
    uint32_t curCount = 0;              // frames of the burst coming in now
    bool lastFull = false;              // the last finished burst had all `burst` frames: only then are frames spaced
    LONGLONG lastReturn = 0;            // when the library's last Present returned (after any wait for room)
    LONGLONG lastBurstStart = 0;
    double interval = 0;                // ticks between burst starts, smoothed
    unsigned hitchRun = 0, offRun = 0;
    LONGLONG lastRelease = 0;
    HRESULT lastHr = S_OK;
    size_t maxDepth = 0;
    unsigned fullWaits = 0;             // times the library had to wait for room in the queue
    bool ok = false;
    bool blockedInBurst = false;        // the library waited for room in the queue since the last burst start (Observe)
    // The spacing between releases, ticks (0 = not known yet: frames go out as they come). Seeded from the burst interval, then steered by
    // the queue alone: a burst arriving while the last one is still queued (2+ frames) means too slow, 3% faster; the queue empty for more
    // than half a spacing before the burst arrives means too fast, 2% slower. The arrival times themselves cannot steer it: once the queue
    // is full the library is held up and its pace is ours.
    double sTicks = 0;
    unsigned cleanSamples = 0;
    LONGLONG emptyAt = 0;
    std::thread worker;

    // Schedule mode (see the top)
    PaceCfg cfg;
    bool sched = false;                 // schedule pacing in effect now (cfg.mode, and for Auto the burst size)
    pacelead::Lead lead;
    pacesched::ReadyInterval ri;
    std::deque<LONGLONG> stamps;        // application Presents not yet matched to a burst
    LONGLONG lastStamp = 0, schedBase = 0;
    uint64_t stampsSeen = 0;
    bool batchValid = false;
    LONGLONG bBase = 0, bStep = 0, bLead = 0, bCap = 0, bStamp = 0;
    double bLeadMs = 0, bStepMs = 0, bCapMs = 0;
    bool leadOpen = false;              // a batch's lead is waiting for its EndFrame
    double openFrozen = 0, openStep = 0;

    // Called with mu held, right after Observe: the first frame of a burst takes the oldest application stamp and opens a batch
    // (base smoothed as the own pacer's, step from the smoothed interval, lead frozen); every frame gets its planned time.
    void Plan(LONGLONG arrived, Item& it)
    {
        if (curCount == 1)
        {
            batchValid = false;
            if (stampsSeen >= 6)
            {
                // The newest application Present not later than this burst's first frame; older ones are dropped (an extra stamp must not
                // shift the schedule by a frame for good). None: the arrival is the base.
                LONGLONG s = arrived;
                long long picked = 0;
                if (pacesched::PickStamp(stamps, arrived, picked))
                    s = picked;
                else
                    ++g_schedNoStamp;
                const LONGLONG iv = static_cast<LONGLONG>(ri.ms * Freq() / 1000.0);
                schedBase = pacesched::NextBase(schedBase, s, 0, iv);
                bStamp = s;
                bStepMs = ri.ms / static_cast<double>(burst);
                bStep = static_cast<LONGLONG>(bStepMs * Freq() / 1000.0);
                // the lead is at most a step (the own generator's rule); wideCap (test): n steps = an app frame minus one step
                bCapMs = cfg.wideCap ? bStepMs * static_cast<double>(burst - 1) : bStepMs;
                bCap = static_cast<LONGLONG>(bCapMs * Freq() / 1000.0);
                bLeadMs = cfg.testLeadMs > 0 ? (std::min)(cfg.testLeadMs, 2.0 * ri.ms) : lead.Freeze(bCapMs);
                bLead = static_cast<LONGLONG>(bLeadMs * Freq() / 1000.0);
                bCap = pacesched::LeadCap(bCap, bLead, cfg.testLeadMs > 0);     // a fixed test lead is not cut to a step
                bBase = schedBase;
                batchValid = true;
                ++g_schedBatches;
                if (Collect())
                {
                    std::lock_guard<std::mutex> l(g_outMu);
                    g_leadMin = (std::min)(g_leadMin, bLeadMs);
                    g_leadMax = (std::max)(g_leadMax, bLeadMs);
                }
            }
        }
        if (!batchValid)
            return;
        it.sched = true;
        it.j = curCount - 1;
        it.b = burst;
        it.step = bStep;
        it.leadMs = bLeadMs;
        it.stepMs = bCapMs;             // the lead's cap: EndFrame limits the lead to it
        it.due = bBase + pacelead::Offset(bLead, bStep, it.j + 1, bCap);
        if (it.j < 8 && Collect())      // how long after the game's Present each frame of the burst arrives (is Intel's own spacing wider than our step?)
        {
            const double off = static_cast<double>(arrived - bStamp) * 1000.0 / Freq();
            g_arrSum[it.j] = g_arrSum[it.j].load() + off;
            g_arrMax[it.j] = (std::max)(g_arrMax[it.j].load(), off);
            ++g_arrN[it.j];
        }
    }

    // mu held, after the mode or the burst size changed: whether schedule pacing is in effect now (Auto: where it is not worse than the burst spacing)
    void UpdateSched()
    {
        const bool was = sched;
        sched = burst > 1 && cfg.mode != PaceMode::Off && (cfg.mode == PaceMode::Schedule || (cfg.mode == PaceMode::Auto && burst <= kAutoMaxBurst));
        if ((cfg.mode == PaceMode::Schedule || cfg.mode == PaceMode::Auto) && burst > 1 && (sched != was || !logged) && g_log)
        {
            logged = true;
            g_log("sink: %u frames per application frame: pacing by %s (%s)", burst, sched ? "the schedule" : "the burst interval",
                  cfg.mode == PaceMode::Auto ? "auto" : "forced");
        }
    }
    bool logged = false;

    // Display-paced frames (release_rule.h): the committed state, for the one log line per change. mu held.
    bool displayPaced = false;
    unsigned dpRun = 0;                 // consecutive queued frames that disagree with the committed state
    LONGLONG lastDpLog = 0;
    unsigned dpLogSkipped = 0;

    // mu held: the frame about to be queued has sync/flags. Returns true when it goes out on arrival (Off, or paced by the display).
    // Such a frame is taken out of ALL the pacing state: Observe (burst interval, spacing) and Plan (schedule, stamps, lead) never see
    // it, and Run gives it no lateness for the lead rule. The per-frame decision is always taken from the frame's own sync/flags.
    // The STATE change (reset of the burst/schedule state, which was measured on a different stream, and the log line) has hysteresis:
    // a stream that mixes the two per frame (generated frames sync 0, the real frame sync 1) would otherwise reset on every flip, so
    // the new state must hold for burst + 1 queued frames in a row (at least 2) before it counts.
    bool NoteRelease(UINT sync, UINT flags)
    {
        const ReleaseKind rel = ReleaseRule(sync, flags, cfg.mode);
        const bool dp = rel == ReleaseKind::OnArrivalDisplay;
        const LONGLONG now = Now();
        if (dp == displayPaced)
            dpRun = 0;
        else if (++dpRun >= (std::max)(2u, burst + 1))
        {
            dpRun = 0;
            displayPaced = dp;
            ResetBurstState();
            ResetSchedule();
            LogDisplayPaced(now, sync, flags);
        }
        if (dpLogSkipped && now - lastDpLog >= Freq())
            LogDisplayPaced(now, sync, flags);      // a change was not logged (too soon after the last line): say what holds now
        return rel != ReleaseKind::Paced;
    }
    // One line per change, and not more than one per second (a game that alternates quickly must not flood the log); a line that was
    // held back is written, with the state that holds by then, on the first frame a second after the last line.
    void LogDisplayPaced(LONGLONG now, UINT sync, UINT flags)
    {
        if (!g_log)
            return;
        if (lastDpLog && now - lastDpLog < Freq())
        {
            ++dpLogSkipped;
            return;
        }
        lastDpLog = now;
        dpLogSkipped = 0;
        if (displayPaced)
            g_log("sink: display-paced (sync interval %u, flags 0x%x): frames go out as they come", sync, flags);
        else
            g_log("sink: paced again (sync interval %u, flags 0x%x)", sync, flags);
    }

    void ResetBurstState()      // mu held: forget the measured burst interval and spacing (a new burst size, or the stream changed)
    {
        curCount = 0;
        lastFull = false;
        lastBurstStart = 0;
        interval = 0;
        hitchRun = offRun = 0;
        sTicks = 0;
        cleanSamples = 0;
        emptyAt = 0;
    }

    void ResetSchedule()        // mu held: a new burst size, a new chain size
    {
        stamps.clear();
        batchValid = false;
        schedBase = 0;
        leadOpen = false;
    }

    IDXGISwapChain4* real() const { return r; }

    // Called with mu held, as a frame comes in.
    void Observe(LONGLONG t)
    {
        if (burst <= 1)
            return;
        // Within a burst the library's frames come at most interval / burst apart (1.3.4.95 at 30 fps 6x: ~5.4 ms each, sometimes a
        // few back to back; measured from the last return so a wait for room does not split it); a frame of its own an interval
        // apart. The split is halfway between the two; until the interval is known, bursts are counted only.
        const double split = interval * (1.0 + 1.0 / burst) / 2;
        if (lastBurstStart && curCount < burst && (interval <= 0 || static_cast<double>(t - lastReturn) <= split))
        {
            ++curCount;
            return;
        }
        const bool full = curCount >= burst;   // the burst that ends here
        curCount = 1;
        // A burst during which the library had to wait for room in the queue says how fast WE release, not how fast the game
        // delivers: taken as the pace it made the spacing ever longer (harness 30 fps: interval 33 -> 160 ms). Left out; the last
        // good value stays, and the queue then drains because the spacing is shorter than the arrival.
        const bool blocked = blockedInBurst;
        blockedInBurst = false;
        if (sTicks > 0 && full && lastFull)     // only a burst that was spaced says whether the spacing is right
        {
            if (q.size() >= 2)
                sTicks *= 0.97;
            else if (q.empty() && emptyAt && static_cast<double>(t - emptyAt) > 0.5 * sTicks)
                sTicks *= 1.02;
        }
        if (lastBurstStart && Collect() && EnvSet("SINK_TRACE2"))
            fprintf(stdout, "sink: burst start +%.2f ms, blocked %d, queued %zu\n", static_cast<double>(t - lastBurstStart) * 1000.0 / Freq(), blocked ? 1 : 0, q.size());
        if (lastBurstStart && !blocked)
        {
            const double dt = static_cast<double>(t - lastBurstStart);
            const double ms = dt * 1000.0 / Freq();
            if (ms > 0.3 && ms < 250)
            {
                if (interval <= 0)
                    interval = dt;
                else if (dt > 1.5 * interval && hitchRun < 2)
                    ++hitchRun;                 // one stutter does not stretch the spacing
                else
                {
                    hitchRun = 0;
                    if (dt > 1.3 * interval || dt * 1.3 < interval)
                    {
                        if (++offRun >= 3)
                        {
                            interval = dt;      // the game's pace changed for good
                            offRun = 0;
                        }
                    }
                    else
                    {
                        offRun = 0;
                        interval += (dt - interval) / 8;
                    }
                }
            }
            else
                interval = 0;                   // a pause: start over
        }
        if (lastBurstStart && !blocked && full)
            ++cleanSamples;
        if (sTicks <= 0 && cleanSamples >= 4 && interval > 0)
            sTicks = interval / burst;
        lastFull = full;
        lastBurstStart = t;
    }

    void WaitFence(UINT64 v)
    {
        if (fence->GetCompletedValue() < v)
        {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, 2000);
        }
    }

    void ReleaseSlots()
    {
        if (fence && fenceValue)
            WaitFence(fenceValue);
        for (UINT i = 0; i < kSlots; ++i)
            if (slot[i])
                slot[i]->Release(), slot[i] = nullptr;
    }

    // Staging textures with the description of the real chain's buffers (so the library's work on them is the same as on the real ones).
    bool MakeSlots()
    {
        ID3D12Resource* b0 = nullptr;
        if (FAILED(r->GetBuffer(0, IID_PPV_ARGS(&b0))) || !b0)
            return false;
        D3D12_RESOURCE_DESC rd = b0->GetDesc();
        D3D12_HEAP_PROPERTIES hp {};
        b0->GetHeapProperties(&hp, nullptr);
        b0->Release();
        hp = { D3D12_HEAP_TYPE_DEFAULT };
        for (UINT i = 0; i < kSlots; ++i)
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot[i]))))
                return false;
        return true;
    }

    bool Init(IUnknown* queueUnknown)
    {
        if (stable && appBuffers > kSlots)
            return false;
        if (!queueUnknown || FAILED(queueUnknown->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
            return false;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))))
            return false;
        for (UINT i = 0; i < kSlots; ++i)
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]))))
                return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list))))
            return false;
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        libEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        alwaysSignaled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kSlots, 0 };
        D3D12_HEAP_PROPERTIES rb { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 2 * kSlots * sizeof(UINT64);
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qheap))) ||
            FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&qread))) ||
            FAILED(queue->GetTimestampFrequency(&tsFreq)))
            qheap = nullptr;        // no timing, the copy still works
        if (!fenceEvent || !libEvent || !MakeSlots())
            return false;
        const DXGI_FORMAT f = slot[0]->GetDesc().Format;
        if (Collect() && EnvSet("SINK_PROBE") && (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
        {
            probePitch = (static_cast<UINT>(slot[0]->GetDesc().Width) * 4 + 255) & ~255u;
            D3D12_RESOURCE_DESC pd = bd;
            pd.Width = static_cast<UINT64>(probePitch) * kSlots;
            if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&probe))))
                probe = nullptr;
        }
        return true;
    }

    // Staging texture `a` (frame `n`) into the real chain's current back buffer, on the chain's queue.
    bool CopyToBackBuffer(UINT a, uint64_t n)
    {
        if (allocFence[a])
            WaitFence(allocFence[a]);
        if (probePending[a])
        {
            void* p = nullptr;
            const D3D12_RANGE rr { a * probePitch, (a + 1) * probePitch }, none { 0, 0 };
            int x = -1;
            if (SUCCEEDED(probe->Map(0, &rr, &p)))
            {
                const uint8_t* px = static_cast<const uint8_t*>(p) + a * probePitch;
                for (UINT i = 0; i < probePitch / 4 && x < 0; ++i)
                    if (px[4 * i] > 180 && px[4 * i + 1] > 150 && px[4 * i + 2] < 110)
                        x = static_cast<int>(i);
                probe->Unmap(0, &none);
            }
            std::lock_guard<std::mutex> l(g_outMu);
            if (g_probeRows.size() < kProbeRows && EnvSet("SINK_PROBE_DUMP") && SUCCEEDED(probe->Map(0, &rr, &p)))
            {
                const uint8_t* row = static_cast<const uint8_t*>(p) + a * probePitch;
                g_probeRows.push_back({ probeN[a], std::vector<uint8_t>(row, row + probePitch) });
                probe->Unmap(0, &none);
            }
            g_probe.push_back({ probeN[a], x });
            probePending[a] = false;
        }
        if (tsPending[a] && qread && tsFreq)
        {
            void* p = nullptr;
            const D3D12_RANGE rr { a * 16, a * 16 + 16 }, none { 0, 0 };
            if (SUCCEEDED(qread->Map(0, &rr, &p)))
            {
                const UINT64* ts = reinterpret_cast<const UINT64*>(static_cast<const uint8_t*>(p) + a * 16);
                if (ts[1] > ts[0])
                {
                    std::lock_guard<std::mutex> l(g_outMu);
                    g_copyGpuMs.push_back(static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(tsFreq));
                }
                qread->Unmap(0, &none);
            }
            tsPending[a] = false;
        }
        ID3D12Resource* bb = nullptr;
        if (FAILED(r->GetBuffer(r->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) || !bb)
            return false;
        alloc[a]->Reset();
        list->Reset(alloc[a], nullptr);
        const bool timed = qheap && Collect();     // GPU timing in the test harness only
        if (timed)
            list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a);
        D3D12_RESOURCE_BARRIER b[2] {};
        b[0].Transition = { slot[a], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE };
        b[1].Transition = { bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        list->ResourceBarrier(2, b);
        list->CopyResource(bb, slot[a]);
        if (probe)
        {
            const D3D12_RESOURCE_DESC d = slot[a]->GetDesc();
            D3D12_TEXTURE_COPY_LOCATION dst { probe, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src { slot[a], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dst.PlacedFootprint = { static_cast<UINT64>(a) * probePitch, { d.Format, static_cast<UINT>(d.Width), 1, 1, probePitch } };
            src.SubresourceIndex = 0;
            const UINT row = d.Height / 2;
            const D3D12_BOX box { 0, row, 0, static_cast<UINT>(d.Width), row + 1, 1 };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            probePending[a] = true;
            probeN[a] = n;
        }
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        list->ResourceBarrier(2, b);
        if (timed)
        {
            list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a + 1);
            list->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * a, 2, qread, a * 16);
            tsPending[a] = true;
        }
        list->Close();
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fenceValue);
        allocFence[a] = fenceValue;
        bb->Release();
        return true;
    }

    void Run()
    {
        for (;;)
        {
            Item it {};
            long long target;
            uint64_t n;
            {
                std::unique_lock<std::mutex> l(mu);
                cvItem.wait(l, [&] { return stop || !q.empty(); });
                if (q.empty())
                    return;
                it = q.front();
                n = presented;
                const LONGLONG now = Now();
                const double spacing = burst > 1 && lastFull ? sTicks : 0;
                if (it.direct)
                    target = now;               // SinkPace=off or display-paced: on arrival (its copy still comes first, below)
                else if (it.sched)
                    target = pacesched::Bound(it.due, it.step, lastRelease);   // a late frame: on arrival, never closer than half a step to the last
                else if (spacing <= 0)
                    target = now;               // frame generation off or not running (short bursts), or the pace not known yet: as it comes
                else
                {
                    const double s = spacing;
                    target = lastRelease ? lastRelease + static_cast<LONGLONG>(s) : now;
                    if (target < now - static_cast<LONGLONG>(2 * s))
                        target = now;           // after a stall do not catch up in a bunch
                }
            }
            if (scproxy::g_enabled && n % 60 == 0 && EnvSet("SINK_TRACE"))
            {
                std::lock_guard<std::mutex> l(mu);
                fprintf(stdout, "sink: released %llu, queued %zu, burst %u, interval %.2f ms, wait %.2f ms\n", static_cast<unsigned long long>(n), q.size(), burst,
                        interval * 1000.0 / Freq(), static_cast<double>(target - Now()) * 1000.0 / Freq());
            }
            WaitUntil(target);
            HRESULT hr = S_OK;
            const LONGLONG c0 = scproxy::Qpc();
            const bool copied = CopyToBackBuffer(it.slot, n);
            scproxy::AddTime("sink.copy", c0);
            // Only our own GPU wait counts as late for the lead rule (as in the own generator): a frame that ARRIVED after its time (the library
            // or the game was late) is presented on arrival and does not raise the lead.
            // Schedule mode: the Present waits for the frame's own GPU work (so it never queues behind it in the swap chain, as the own
            // generator's PresentReal); a generated frame whose work was still running at its time is "late" for the lead rule
            double lateMs = 0;
            if (copied && it.sched)
            {
                const LONGLONG w0 = Now();
                WaitFence(allocFence[it.slot]);
                const LONGLONG w1 = Now();
                lateMs = (std::min)(static_cast<double>(w1 - w0) * 1000.0 / Freq(), (std::max)(0.0, static_cast<double>(w1 - target) * 1000.0 / Freq()));
            }
            if (!copied)
                hr = DXGI_ERROR_INVALID_CALL;
            else
            {
                const LONGLONG p0 = scproxy::Qpc();
                hr = real()->Present(it.sync, it.flags);
                scproxy::AddTime("sink.realPresent", p0);
            }
            const LONGLONG t = Now();
            if (lastRelease && Collect())
            {
                std::lock_guard<std::mutex> l(g_outMu);
                g_outGaps.push_back(static_cast<double>(t - lastRelease) * 1000.0 / Freq());
            }
            if (Collect() && EnvSet("XEFG_CUDA_PACELOG"))
            {
                std::lock_guard<std::mutex> l(g_outMu);
                g_outTimes.push_back(static_cast<double>(t) * 1000.0 / Freq());
            }
            lastRelease = t;
            std::lock_guard<std::mutex> l(mu);
            if (it.sched)
            {
                if (it.j == 0)
                {
                    if (leadOpen)
                        lead.EndFrame(openFrozen, openStep);    // the batch before it was short: close its lead first
                    leadOpen = it.leadMs > 0;
                    openFrozen = it.leadMs;
                    openStep = it.stepMs;
                }
                if (it.j + 1 < it.b)
                {
                    ++g_schedFrames;
                    if (leadOpen && lateMs > pacelead::kLateMs)
                    {
                        ++g_schedLate;
                        if (it.j < 8)
                        {
                            ++g_lateJ[it.j];
                            g_lateJSum[it.j] = g_lateJSum[it.j].load() + lateMs;
                        }
                        lead.Late(lateMs);
                    }
                }
                else if (leadOpen)
                {
                    lead.EndFrame(openFrozen, openStep);
                    leadOpen = false;
                }
            }
            ++presented;
            if (FAILED(hr))
                lastHr = hr;
            q.pop_front();
            if (q.empty())
                emptyAt = Now();                // drained: how long it stays so before the next burst is the slack (Observe)
            cvSpace.notify_all();
        }
    }

    void Drain()
    {
        std::unique_lock<std::mutex> l(mu);
        cvSpace.wait(l, [&] { return q.empty(); });
    }

public:
    // queueUnknown: what the library gave CreateSwapChainForHwnd (the chain's command queue). ok() false: not a D3D12 queue, use the real chain as it is.
    SinkSwapChain(IDXGISwapChain4* realChain, UINT appCount, IUnknown* queueUnknown, bool stableBuffers)
        : scproxy::LogSwapChain(realChain), appBuffers(appCount), stable(stableBuffers)
    {
        ok = Init(queueUnknown);
        if (ok)
            worker = std::thread([this] { Run(); });
    }
    bool Ok() const { return ok; }

    // Frames per application frame (generated + its own); 1 = frame generation off.
    void SetBurst(uint32_t count)
    {
        std::lock_guard<std::mutex> l(mu);
        const uint32_t nb = count ? count : 1;
        if (nb == burst)
            return;         // the pacing state (interval, spacing) is only reset by a real change
        burst = nb;
        UpdateSched();
        ResetSchedule();
        ResetBurstState();
    }

    // The pacing mode and the lead rule's setting (PaceLead: automatic, else fixedMs, 0 = off)
    void SetPace(const PaceCfg& c)
    {
        std::lock_guard<std::mutex> l(mu);
        cfg = c;
        displayPaced = false;           // the next frame decides again (Off must not log "paced again" for a state it never had)
        dpRun = 0;
        dpLogSkipped = 0;
        lead.automatic = c.leadAuto;
        lead.fixedMs = c.leadFixedMs;
        UpdateSched();
        ResetSchedule();
    }
    // The game side's Present call at `t` (QPC): the base of the next batch (see the top)
    void AppPresent(LONGLONG t)
    {
        std::lock_guard<std::mutex> l(mu);
        if (cfg.mode == PaceMode::Burst || cfg.mode == PaceMode::Off)
            return;                 // no schedule: no stamps (they would only pile up unused)
        if (lastStamp)
            ri.Observe(static_cast<double>(t - lastStamp) * 1000.0 / Freq());
        lastStamp = t;
        ++stampsSeen;
        ++g_schedStamps;
        if (sched)
        {
            stamps.push_back(t);
            if (stamps.size() > 8)
                stamps.pop_front();
        }
        else
            stamps.clear();
    }

    size_t MaxDepth() const { return maxDepth; }
    unsigned FullWaits() const { return fullWaits; }

    SinkSwapChain** owner = nullptr;    // the factory's pointer to this sink, cleared when it goes (under g_linkMu)

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --rc;
        if (!n)
        {
            {
                std::lock_guard<std::mutex> g(g_linkMu);   // a SinkFactory::SetBurst in flight finishes first
                if (owner && *owner == this)
                    *owner = nullptr;
            }
            {
                std::lock_guard<std::mutex> l(mu);
                stop = true;
            }
            cvItem.notify_all();
            cvSpace.notify_all();
            if (worker.joinable())
                worker.join();
            g_depthStat = maxDepth;
            g_fullStat = fullWaits;
            ReleaseSlots();
            for (UINT i = 0; i < kSlots; ++i)
                if (alloc[i])
                    alloc[i]->Release();
            if (list)
                list->Release();
            if (fence)
                fence->Release();
            if (qheap)
                qheap->Release();
            if (qread)
                qread->Release();
            if (probe)
                probe->Release();
            if (fenceEvent)
                CloseHandle(fenceEvent);
            if (libEvent)
                CloseHandle(libEvent);
            if (alwaysSignaled)
                CloseHandle(alwaysSignaled);
            if (queue)
                queue->Release();
            if (dev)
                dev->Release();
            r->Release();
            delete this;
        }
        return n;
    }

    HRESULT Enqueue(UINT sync, UINT flags)
    {
        const LONGLONG arrived = Now();             // before any wait for room: the library's own pace, not ours
        std::unique_lock<std::mutex> l(mu);
        if (q.size() >= kMaxQueue)
        {
            ++fullWaits;
            blockedInBurst = true;
        }
        cvSpace.wait(l, [&] { return stop || q.size() < kMaxQueue; });
        const bool direct = NoteRelease(sync, flags);
        if (!direct)
            Observe(arrived);
        if (static const bool trace3 = Collect() && EnvSet("SINK_TRACE3"); trace3)    // tests: when each frame of a burst arrives
            fprintf(stdout, "sink: arrive %.3f\n", static_cast<double>(arrived) * 1000.0 / Freq());
        Item item { sync, flags, stable ? static_cast<UINT>(in % appBuffers) : static_cast<UINT>(in % kSlots) };
        item.direct = direct;
        if (!direct && sched && burst > 1)
            Plan(arrived, item);
        q.push_back(item);
        ++in;
        maxDepth = (std::max)(maxDepth, q.size());
        const HRESULT h = lastHr;
        lastHr = S_OK;
        if (stable)
        {
            // The library draws the next frame into texture in % appBuffers as soon as this returns: the frame that held it last
            // (appBuffers frames back) must be out of the queue and its copy done on the GPU.
            cvItem.notify_one();
            cvSpace.wait(l, [&] { return stop || q.size() < appBuffers; });
            const UINT64 v = allocFence[in % appBuffers];
            l.unlock();
            if (v && fence->GetCompletedValue() < v)
            {
                fence->SetEventOnCompletion(v, libEvent);
                WaitForSingleObject(libEvent, 2000);
            }
            l.lock();
        }
        lastReturn = Now();
        l.unlock();
        cvItem.notify_one();
        return h;           // a failed real Present is reported on the library's next one
    }

    HRESULT STDMETHODCALLTYPE Present(UINT si, UINT fl) override
    {
        scproxy::Log("SINK.Present", "sync %u flags 0x%x", si, fl);
        if (fl & DXGI_PRESENT_TEST)         // shows nothing: straight to the real chain, never queued (as the own generator does)
            return real()->Present(si, fl);
        return Enqueue(si, fl);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* pp) override
    {
        scproxy::Log("SINK.Present1", "sync %u flags 0x%x", si, fl);
        if (fl & DXGI_PRESENT_TEST)
            return real()->Present1(si, fl, pp);
        return Enqueue(si, fl);
    }
    // Intel's library paces its presents on the real chain's frame-latency waitable object (it waits on it before the next frame): with
    // the real one, the library's pace follows OUR release times and the frames never come in a burst the pacer could space. This one
    // is always signaled; the only limit on the library is the room in the queue (Enqueue).
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        scproxy::Log("SINK.GetFrameLatencyWaitableObject");
        return alwaysSignaled;
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
    {
        std::lock_guard<std::mutex> l(mu);
        return static_cast<UINT>(in % appBuffers);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID id, void** s) override
    {
        std::lock_guard<std::mutex> l(mu);
        const UINT a = stable && b < appBuffers ? b : static_cast<UINT>(in % kSlots);
        scproxy::Log("SINK.GetBuffer", "asked %u, frame %llu -> slot %u", b, in, a);
        return slot[a]->QueryInterface(id, s);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
    {
        scproxy::Log("SINK.ResizeBuffers", "count %u %ux%u", c, w, h);
        Drain();
        std::lock_guard<std::mutex> l(mu);
        ReleaseSlots();
        const HRESULT hr = r->ResizeBuffers(c, w, h, f, fl);
        in = presented = 0;             // the real chain starts again at buffer 0
        ResetSchedule();
        ResetBurstState();
        if (SUCCEEDED(hr) && !MakeSlots())
            return E_FAIL;
        return hr;
    }
};

class SinkFactory final : public scproxy::LogFactory
{
    SinkSwapChain* last = nullptr;
    const bool stable;
    PaceCfg pace;           // under g_linkMu: the pacing mode (SetPace), applied to every chain made
    uint32_t burst = 1;     // under g_linkMu: the frames per application frame set last, so a chain made later starts with it

public:
    // stableBuffers: for a library that takes its buffers once (DLSS-G through Streamline), see SinkSwapChain
    explicit SinkFactory(IDXGIFactory4* real, bool stableBuffers = false) : scproxy::LogFactory(real), stable(stableBuffers) {}
    ~SinkFactory() override
    {
        std::lock_guard<std::mutex> g(g_linkMu);
        if (last)
            last->owner = nullptr;
    }

    // The sink the library was given; null once the library let go of it. Test use only (no lock): use SetBurst.
    SinkSwapChain* Sink() const { return last; }
    bool HasSink()
    {
        std::lock_guard<std::mutex> g(g_linkMu);
        return last != nullptr;
    }

    // The pacing mode for the chains made from now on (and the current one): see the top of this file
    void SetPace(const PaceCfg& c)
    {
        std::lock_guard<std::mutex> g(g_linkMu);
        pace = c;
        if (last)
            last->SetPace(c);
    }
    // The game's Present call (QPC), for schedule mode
    void AppPresent(LONGLONG t)
    {
        std::lock_guard<std::mutex> g(g_linkMu);
        if (last)
            last->AppPresent(t);
    }

    // Frames per application frame for the current sink, if there is one (safe against the library releasing it meanwhile).
    void SetBurst(uint32_t count)
    {
        std::lock_guard<std::mutex> g(g_linkMu);
        burst = count ? count : 1;
        if (last)
            last->SetBurst(burst);
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                     IDXGIOutput* o, IDXGISwapChain1** s) override
    {
        scproxy::Log("SINK.CreateSwapChainForHwnd", "%u buffers", desc ? desc->BufferCount : 0);
        IDXGISwapChain1* real1 = nullptr;
        const HRESULT h = r->CreateSwapChainForHwnd(d, w, desc, fs, o, &real1);
        if (FAILED(h) || !real1)
        {
            *s = real1;
            return h;
        }
        IDXGISwapChain4* real4 = nullptr;
        if (FAILED(real1->QueryInterface(IID_PPV_ARGS(&real4))))
        {
            *s = real1;
            return h;
        }
        real1->Release();
        auto* sink = new SinkSwapChain(real4, desc->BufferCount, d, stable);
        if (!sink->Ok())
        {
            scproxy::Log("SINK.unusable", "not a D3D12 queue or no staging textures - the real chain as it is");
            sink->Release();            // releases real4 too; take a fresh reference to hand over
            return r->CreateSwapChainForHwnd(d, w, desc, fs, o, s);
        }
        {
            std::lock_guard<std::mutex> g(g_linkMu);
            if (last)
                last->owner = nullptr;
            last = sink;
            sink->owner = &last;
            sink->SetPace(pace);
            sink->SetBurst(burst);      // a chain made after a SetBurst (a re-created chain) starts with the current burst
        }
        *s = sink;
        return h;
    }
};
} // namespace xsink
