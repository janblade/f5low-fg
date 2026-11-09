# Bounded waits

No wait on the presenter / generator path may hang the game. On a timeout f5low logs once ("waits: ..."), stops generating for that
context (the game's frames are shown as they are), never resets a command allocator the GPU may still use (a spare allocator takes its
place), and generates again by itself after 20 frames in a row without a timeout. Keys in f5low.ini: `WaitBoundMs=` (GPU fence and
allocator waits, default 500, 50-5000), `GameWaitMs=` (the game's Present waiting for room in the presenter's queue, default 500, 10-2000; while a stall is on the wait is a short 16 ms slice).

| Where | Bound | After a timeout |
|---|---|---|
| xefg_own.h OnPresent, wait for room in the presenter's queue | GameWaitMs (was none), 16 ms while stalled | frame queued as pass-through, stall episode starts |
| BeginP / BeginGen / Begin (allocator fence) | WaitBoundMs (was 10 s) | spare allocator, no reset of the busy one; stall episode |
| WaitIdle / WaitApp (resize, teardown) | 10 s, kept on purpose, now logged | goes on (what follows frees resources; a lost device) |
| WaitGpu (200 ms, 1000 ms) | unchanged | result kept (lastGpuWaitOk, statGpuWaitTimeouts) for the late-frame decision; nothing reset |
| retire wait 200 ms, token waits 100 ms, watcher / block-watch 50 ms, 250 ms gate wait, tag wait 2 s | unchanged (already bounded) | as before |
| sink.h Enqueue: room in the queue | 500 ms (was none) | frame dropped, call returns |
| sink.h Enqueue: library texture release, copy fence | 500 ms (was none / 2 s) | logged, library goes on |
| sink.h copy allocator fence | 500 ms (was 2 s) | frame not copied or shown, allocator not reset |
| sink.h Drain (resize) | 2 s (was none) | logged, goes on |
| generators/own.h 2 s CUDA-to-D3D fence, xefg_pack_d3d.h 20 s | unchanged | as before |
| deferThread INFINITE join, WaitUntil on a finite timer | unchanged on purpose | - |

After a stall the presenter leaves out pass-through frames older than the newest appJobs + 2 (they are consumed unshown, every signal
they owe is still sent, counted in the stats line), so the stall does not end in a burst of stale frames.

## Diagnostics: forced stalls

Off by default; for checking the bounds in a game, not for play. One stall, once, at the first app frame at or after `TestStallFrame`:

```
TestStallFrame=600
TestStallMs=1500
TestStallKind=presenter
```

`presenter`: the presenter thread sleeps `TestStallMs`. `genlane` / `presentlane`: that lane's queue waits on a private fence a helper thread
signals after `TestStallMs`, so the allocator bound and the spare allocators run for real (`presentlane` needs the gated present lane). One
"TestStall:" line is logged when it fires.
