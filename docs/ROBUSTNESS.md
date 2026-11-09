# Bounded waits

## What is guaranteed

The game thread's CPU waits on the presenter / generator path end within a bound for any finite stall or stuck GPU lane. On a timeout
f5low logs once ("waits: ..."), stops generating for that context (the game's frames are shown as they are), does not reset a command
allocator the GPU may still use (a spare allocator, or after a long wait a fresh one, takes its place; a busy one is parked until
teardown) and generates again by itself once the stall has cleared: 8 frames with room in a row and at least 300 ms since the last
timeout.

What is not guaranteed:
- While stalled the game still runs at only a few frames per second (its existing 200 ms retire wait plus one 16 ms slice per frame).
- A presenter thread that never returns can still stop the game's GPU queue through `consumedFence` / `blockFence`. That is on purpose:
  those fences keep the game from overwriting a picture the presenter has still to copy.
- The waits marked "unbounded / long by design" below.
- A resize or rebuild in progress is not a fault: the game's wait uses the long bound and logs nothing, and no frames are left out.

Keys in f5low.ini: `WaitBoundMs=` (GPU fence and allocator waits, default 500, 50-5000), `GameWaitMs=` (the game's Present waiting for
room in the presenter's queue, default 500, 10-2000; while a stall is on the wait is a short 16 ms slice). Not a number, 0 or negative
means the default. The sink (Generator=intel) uses a fixed 500 ms (the short slice while stalled), not `WaitBoundMs`.

After a stall the presenter leaves out pass-through frames older than the newest appJobs + 2 (they are consumed unshown, every signal
they owe is still sent, counted in the stats line), so the stall does not end in a burst of stale frames.

## Inventory

| Where | Bound | After a timeout |
|---|---|---|
| xefg_own.h OnPresent, wait for room in the presenter's queue | GameWaitMs (was none); 16 ms while stalled; 10 s during a resize / rebuild | frame queued as pass-through, stall episode starts |
| BeginP / BeginGen / Begin (allocator fence) | WaitBoundMs (was 10 s), looping to the deadline (a late event of an earlier wait does not end it) | spare allocator, no reset of the busy one; stall episode. No spare: 10 s, then a fresh allocator replaces the busy one (parked) |
| WaitIdle / WaitApp (resize, teardown) | 10 s, long on purpose, looping, now logged | goes on (what follows frees resources the GPU may still use; only a lost device ends it this way) |
| WaitGpu (200 ms, 1000 ms) | unchanged | result kept (`lastGpuWaitOk`, `statGpuWaitTimeouts`, the latter in the stats line); nothing resets on it, nothing uses `lastGpuWaitOk` yet |
| retire wait 200 ms, token waits 100 ms, watcher / block-watch 50 ms, 250 ms gate wait, tag wait 2 s | unchanged (already bounded) | as before |
| sink.h Enqueue: room in the queue | 500 ms fixed (was none); 16 ms while stalled | frame dropped, call returns (stable buffers: the library's buffer index still advances) |
| sink.h Enqueue: library texture release, copy fence | 500 ms fixed (was none / 2 s); 16 ms while stalled | logged, library goes on |
| sink.h copy allocator fence | 500 ms (was 2 s) | frame not copied or shown, allocator not reset |
| sink.h Drain (ResizeBuffers) | 10 s (was none) | ResizeBuffers returns a failure, logs why, frees nothing |
| generators/own.h 2 s CUDA-to-D3D fence, xefg_pack_d3d.h 20 s | unchanged | as before |
| DrainPresenter, the generation thread's wait for the presenter at a rebuild | unbounded on purpose | a rebuild frees what the presenter may be reading; the presenter's own waits are bounded |
| presenter thread join at StopPresenter | unbounded on purpose | what follows destroys what the presenter uses |
| deferThread INFINITE join, WaitUntil on a finite timer | unchanged on purpose | - |

## Diagnostics: forced stalls

Off by default; for checking the bounds in a game, not for play. One stall, once, at the first app frame at or after `TestStallFrame`:

```
TestStallFrame=600
TestStallMs=1500
TestStallKind=presenter
```

`presenter`: the presenter thread sleeps `TestStallMs`. `genlane` / `presentlane`: that lane's queue waits on a private fence a helper thread
signals after `TestStallMs`, so the allocator bound and the spare allocators run for real (`presentlane` needs the gated present lane).
One "TestStall:" line is logged when it fires. There is no forced stall for the sink.

A forced failed list (debug only): `TestCloseFail=<n>` in f5low.ini, or the environment variable `XEFG_CUDA_TESTCLOSEFAIL=<n>`, makes the n-th Close
of one of the own generator's lists count as failed (the list is closed for real and then not executed). A "WARNING: ... TestCloseFail" line is logged
at start and one when it fires; generation then goes off and the game's frames pass through until the swap chain is rebuilt (docs/GENERATORS.md).
