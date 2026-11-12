# TargetFps: frame generation on a fixed output clock

`TargetFps=N` in f5low.ini (`XEFG_CUDA_TARGETFPS`, 30-500) runs the own generator's presenter on a fixed output clock of
N Hz, whatever the game's frame rate. Each output slot shows the frame interpolated at that slot's own time t between
the two latest real frames, so the ratio of shown to rendered frames can be fractional (for example a game capped at
45 fps shown at 116 Hz). Off (0) by default; with it off nothing changes.

Scope: own generator, plain D3D12 path (`Show`: each pass is recorded just before its present), `Gate=0`,
`GenQueue=off`. TargetFps turns `GenBlock` off (GenAhead records the passes before the slots exist). The game's sync
interval may be 0 or 1. Frames are generated only between two real frames (no extrapolation).

## Settings

| Key | Values | Meaning |
|---|---|---|
| `TargetFps` | 30-500, 0 off | Output clock rate. With V-Sync the slots stay at most 97% of the refresh rate (116 at 120 Hz), where VRR shows each present at once. |
| `TargetT` | `exact` (default), `even` | `exact`: each slot at its own t (about one pass per output frame). `even`: t = i/m and the real frame in the last slot (cheaper, uneven motion steps when m alternates). |
| `TargetPacing` | `timer` (default), `display` | `display`: at the refresh with V-Sync the display spaces the presents. Only useful on fixed-refresh displays: VRR has no vblank grid below its top rate. |
| `TargetPresent` | `vsync` (default), `tearing` | `tearing`: timer slots are presented with sync interval 0 and the tearing flag (swap chain allows tearing, not exclusive fullscreen), each Present call at least one refresh after the last, so VRR shows them without tearing. Slots are then planned no closer than a refresh. Decided once per game frame. |
| `TargetPassMs` | 0.5-10, default 3 | Late latch: a pass recorded too late to be ready at its slot is shown at now + this, with t for that moment. |

## How it works

- **Slots.** A fixed grid `epoch + i * P` (P = 1/N). A frame pair (previous real, this real) spans content times c0..c1:
  c0 is the previous pair's c1 (no jump at the boundary), c1 = base + I, with I the app's interval measured from its
  Present calls. The pair takes the free slots up to c1 (at least one, at most 5); t = (slot - c0) / (c1 - c0).
  Slots already past when the pair is planned are skipped and counted. All but the last are generated frames; the last
  is the frame's real-role present, which shows the real frame when a slot lands within 0.05 of it, else the frame
  generated at its t.
- **Off near the target.** When the game's interval drops below 1.03 slots, generation stops (the real frames pass
  through); it resumes above 1.10.
- **Lag.** When our pass is still running on the GPU at its slot (it queues behind the game's work), every later slot
  moves later by that amount, up to half a slot, falling back by 0.2% per app frame.
- **Late latch.** In exact mode, a pass that cannot be ready at its slot takes the moment it can be shown as both its
  present time and its t. On VRR a frame is shown when presented, so a late frame with the slot's t would show content
  older than its moment (visible as lumps during fast camera turns).
- **Own pacing only.** TargetFps frames are never flip metered, burst or let out early (FlipLead), and LateGen's time
  rules (stale, compress, drop) do not apply to them: missed slots and the late latch handle lateness. A pass whose GPU
  wait ran out is still left out.
- **Counts.** XeLL is told no frames are generated. The app's `GetLastPresentStatus` count and the Reflex Sync
  multiplier use the rounded average of planned presents per app frame (a present later dropped still counts). In WoW, OptiScaler's low-latency input still held the game
  at refresh / 2 with frame generation on; `LowLatencyInput=off` in OptiScaler.ini removed that cap.
- **Log** (every 600 app frames, `Log=1`): planned presents per app frame, real frames shown, planned t histogram, slots
  missed, lag, late latches, and gaps over 1.5 slots by cause: the game's frame came late, the presenter was late, our
  GPU work, the swap chain's wait, the Present call, planned so, other. The first five gaps a second are logged with
  every measured part.

## Results (World of Warcraft, DX12 via OptiScaler, RTX 4090 Laptop, 120 Hz VRR, 3840x2400 output)

| Setup | Game | Shown | Present spacing |
|---|---|---|---|
| Fixed 2x, V-Sync, cap 60 | 60 | 120 | sd 0.04 ms |
| TargetFps 120, display pacing, uncapped | 48-85 | 94-117 | sd 2-3.3 ms |
| TargetFps 116, timer, even t, cap 50 | 50 | ~113 | sd 1.2-1.75 ms |
| TargetFps 116, exact t, lag, cap 45 | 45 | ~115 | sd 0.25-0.8 ms |
| Same, late latch, tearing | 45 | ~115 | sd 0.19-0.35 ms; 0.7% of presents over 1 ms late |

- The game needs GPU headroom: uncapped (or capped too high) the passes queue behind its frame and come out late. At
  this output size a pass took 0.94 ms once per real frame plus 2.1 ms per generated frame.
- The remaining gaps (about 30 a minute) were the game's own: PresentMon showed the same rate with frame generation
  off.
- Exact t with the late latch looked best; even t showed uneven motion when two and three slots alternated.
