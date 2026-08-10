# P.A.R.Main Flow

## Hardware in 5 seconds

| Thing | What it does |
|---|---|
| Arduino Nano RP2040 | Main brain — WiFi, HTTP, servo, color sensor |
| Arduino Mega + GRBL | Motion controller — moves the gantry via Serial1 |
| Servo (D9) | Flips individual discs (black ↔ blue) |
| TCS3200 (D4–D8) | Color sensor — reads which side of a disc is showing |

---

## Boot (`setup`)

1. **Init grid coords** — 37×18 = 666 cells, each mapped to a physical (X, Y) in mm (negative coords because GRBL homes to full-negative). Starting X offset = 25 mm.
2. **Attach servo**, set to 0°
3. **Configure TCS3200** — S0/S1 = HIGH/LOW → 20% freq scaling (full speed is too fast to measure)
4. **Wait 2s** for GRBL to boot, flush any junk bytes
5. **Send G-code init** — `G21` (mm), `G90` (absolute), then `$H` to home the CNC
6. **`scanGrid()`** — move sensor over every cell, read color, store current disc states in `gridState[]`
7. **Connect WiFi** → ready to poll

---

## Main Loop (`loop`)

```
poll server → got bitmap? → re-scan board → display it → check pass → snapshot request → mark complete → 10 min sleep → repeat
             no bitmap?  → wait 10s → repeat
```

### Step-by-step

**1. Poll** — `fetchNext()` opens a raw TLS socket to the server, `GET /next.php`

**2. Parse response**
- Status 200 + body = new bitmap (base64, 112 chars max)
- Body `"NONE"` or non-200 = queue empty, sleep 10s

**3. Decode** — base64 → 84 bytes = 666 bits (last 6 bits are unused padding), one bit per disc

**4. `scanGrid()`** — re-home and re-read every cell with the TCS3200. The 10-min idle disables steppers, so position can drift and discs may have been moved; this reseeds `gridState[]` so `displayBitmap` only flips cells that actually differ. The reading sweep runs with the **flip arm dropped to SCAN (`SERVO_US_SCAN` = 889 µs, ~33.5°)** instead of parked at REST: the sensor trails the flip head by `SCAN_OFFSET_X` (−24.005 mm ≈ one cell pitch), so the lowered arm brushes the whole board during the serpentine and pushes through any squisk accidentally left at stage 1 (90°, half-rotated). Homing moves still run with the arm at REST (the initial home is before the drop; the mid-scan rehome lifts it first) — a dropped arm dragged diagonally across the populated board is what snapped the flip arm before. The arm is parked back at REST as soon as the scan completes. Each cell is read with **LED ambient-subtraction** and classified by a simple clear-channel threshold (see Color Classification below); ambient subtraction removed the old room-light "blown regime", so the former `SCAN_C_CEILING` guard / sensor re-init recovery is gone.

**5. `displayBitmap()`** — over the band of rows that contain at least one differing cell, flip cells where `desired bit ≠ gridState`. Rows with no differing cells are **skipped entirely** (no move to them — the next flipping row is still entered via `moveToYSafe`, so Y travel stays at an X soft-limit). Returns the number of cells flipped (= cells that were wrong vs the target), used to gate the check pass.

**5b. Check pass** — after the first `displayBitmap()` completes, conditionally re-scan + re-fix. Two short-circuits, both keyed on `CHECK_FIX_MAX_SKIP` (=5):
- **First draw flipped ≤5 cells** → tiny job, few chances to fail → **skip the whole check pass** (no re-scan, no re-fix).
- **Otherwise** → re-run `scanGrid()` (reseeds `gridState[]` from the physical board, catching discs that didn't flip cleanly or were misclassified), then count mismatches vs the target. Re-run `displayBitmap()` **only if >5 cells are still wrong**; ≤5 are left as-is. The color sensor is ~99.5% accurate, so a full 666-cell scan misreads ~3 cells on average — ≤5 mismatches sit within that noise floor, so re-flipping them would more likely flip a *correct* disc than fix a real miss. Tolerating ≤5 doesn't lower display accuracy. One corrective pass at most.

**6. `flipDisc(x, y, catchByNextMove, inverted)`** — two-stage 180° rotation (+ optional second catch)

> **Servo-angle regime (changed 2026-08-09, merged from `FlipAllTest`).** `SERVO_US_RELEASE` went **1018 → 807** (≈46° → ≈25.5°) and `SERVO_US_ENGAGE` **1471 → 1317** (≈90° → 75°), and the release angle is now **lidar-standoff compensated per cell** instead of commanded raw. Those base angles are only valid **as a set** with the compensation (`RELEASE_EXTRA_REACH_MM`, `LIDAR_FIT_CMM`, `RELEASE_EDGE_ROW_TRIM_MM`) and with the target trims (`FLIP_TARGET_OFFSET_X`, `FLIP_NONINVERT_OFFSET_X`, `FLIP_SKEW_X_TOP` −1.5, `FLIP_CATCH_EXTRA_X`, the unload). Porting one half without the other leaves the flip wrong by tens of degrees. `SERVO_US_SCAN` was deliberately **pinned to the literal 889 µs** (~33.5°) rather than left derived from `SERVO_US_RELEASE` — the scan sweep is not part of the flip tuning and following RELEASE down would have raised the sweeping arm ~20°.

The absolute flip X is
`grid[y][x].x + FLIP_TARGET_OFFSET_X + flipSkewX(y) + (inverted ? FLIP_INVERT_OFFSET_X : FLIP_NONINVERT_OFFSET_X)`.
- `FLIP_TARGET_OFFSET_X` = **+2.0 mm** — global landing trim, every cell, +X = away from homing.
- `flipSkewX(y)` — linear column-lean correction, anchored at the **bottom** row: 0 at `y = GRID_H-1`, `FLIP_SKEW_X_TOP` (**−1.5 mm**, toward homing/−X) at `y = 0`. Net landing is +2.0 mm on the bottom row and +0.5 mm on the top row. Only the flip head shifts — scanning still targets cell centers.
- `FLIP_NONINVERT_OFFSET_X` = **−0.5 mm** on RTL rows only — a residual registration difference between the two sweep directions (the LTR/inverted direction is the confirmed-correct reference). Distinct from `FLIP_INVERT_OFFSET_X`, which compensates for hitting the opposite *face* of the squisk.

`inverted` is passed on **left-to-right sweep rows** (`ltr` in `displayBitmap`) and mirrors the whole X excursion: `dx = −FLIP_OFFSET_X`, so the arm clears the disc column toward −X and catches on the +X return. Two effects: the catch drives the squisk through its final 90° in the opposite rotational sense, so LTR rows **unwind the column-rod twist** the RTL rows wind in; and the return slide — the flip's last motion — now ends in the direction the sweep is already heading, so an LTR row no longer backtracks 16.8 mm before every next cell. **RTL rows keep the original flip** for exactly that reason: their −X return already points along the sweep. Coming at the disc from the other side puts the arm on the opposite face of the squisk, hence the **+11.0 mm** (`FLIP_INVERT_OFFSET_X`) shift of the flip target on inverted rows.

```
move gantry to disc (fx = cell X + 2.0 + skew + (inverted ? +11.0 : -0.5))
servo → ENGAGE (75°)   → settle 300ms  # rotates squisk 90°; NOT compensated
G91; X -unload; G90                    # arm unload, still at ENGAGE (1.5 mm, 4.0 mm on the off-board edge cell)
servo → REST (0°)      → settle 300ms
G91; X (dx + unload); G90              # clearing slide — lands at cell + dx regardless of the unload
servo → RELEASE (compensatedUs, ~793..921µs) → settle 100ms
G91; X (-dx ∓ FLIP_CATCH_EXTRA_X); G90 # catch stroke — sweeps 3.5 mm PAST the cell origin
#ifdef FLIP_SECOND_CATCH               # default OFF — commented-out //#define near FLIP_OFFSET_X
  servo → RELEASE2 (compensated)→ settle 100ms  # second catch: arm a further ~10° lower
  if !catchByNextMove:                  # only when the next move won't already do it
    G91; X +dx2; G90                    # explicit +X sweep (opposite the return)
    servo → REST (0°)    → settle 100ms
#else                                   # second catch disabled
  servo → REST (0°)      → settle 100ms # park; catchByNextMove ignored
#endif
```

**Arm unload.** At ENGAGE the arm is bearing against the squisk it just rotated, so before the servo lifts to REST the head backs off **opposite** the coming clearing slide — taking the contact force off the arm so it lifts away cleanly instead of dragging a loaded disc. The clearing slide then travels `dx + unload`, so net motion from the cell origin is unchanged. `FLIP_UNLOAD_X = 1.5 mm` normally; `FLIP_UNLOAD_X_EDGE = 4.0 mm` **only where the unload travels off the board**, which is direction-matched, not column-symmetric: `usesEdgeUnload(gx, inverted)` is `gx == GRID_W-1` on LTR/inverted rows (unload runs +X, off the right edge) and `gx == 0` on RTL rows (unload runs −X, off the left edge). Any other pairing would back 4 mm **into** the neighbouring populated column — do not "restore the symmetry". Set `FLIP_UNLOAD_X = 0.0f` to disable the feature entirely.

**Catch stroke.** The return travels `−dx` **plus `FLIP_CATCH_EXTRA_X` (3.5 mm) further in the same direction**, so the arm sweeps past the cell origin instead of stopping on it — that is what completes the second 90° consistently. Only the return is lengthened; the clearing slide stays at `FLIP_OFFSET_X`. Each cell re-establishes absolute X with `moveTo(fx)`, so the extra travel cannot accumulate.

**Lidar standoff compensation** (`LIDAR_COMP_MODE 1` — every cell). The arm is a lever that lies flat on the platform at `ARM_FLAT_DEG` (23°), so its perpendicular reach is `ARM_LEN_MM·sin(θ − FLAT)`; a cell whose standoff exceeds `LIDAR_REF_MM` (37.69) by *d* needs *d* more reach, giving `θ' = FLAT + asin(sin(θ − FLAT) + d/ARM_LEN_MM)`. `compensatedUs(baseUs, gx, gy)` applies that to `SERVO_US_RELEASE` per cell from the `LIDAR_FIT_CMM[18][37]` rod-bend table, and adds two terms **outside** the comp gate: `RELEASE_EXTRA_REACH_MM` (+2.0 mm global — shortened pusher pin, capped by rod compliance) and `RELEASE_EDGE_ROW_TRIM_MM` (−0.5 mm on the **top and bottom rows only**, whose rods sit closest to their mounts, are stiffest, and over-push at the interior's reach). Result over the board: **793..921 µs, mean 853**; nothing clamps at `SERVO_US_MIN`. `ARM_LEN_MM` is the arm's length — the pin trim is a linear reach demand and belongs in `RELEASE_EXTRA_REACH_MM`, not in the lever arm. `LIDAR_COMP_MODE 2` (masked paired trial) exists only in `FlipAllMaskedTest`; PARMain `#error`s on it.

`dx = ±FLIP_OFFSET_X (16.8 mm)` — **− on inverted (LTR) rows, + on RTL rows** — and every stroke (clearing, unload, catch-with-extra) is capped so it never commands past `X=0` or `−X_TRAVEL`; the unload is *skipped* rather than clamped if it would breach. Simulated over all 666 cells × both row directions × all strokes: worst-case margins **12.775 mm at `X=0`** (col 36, row 17, RTL clearing slide) and **19.700 mm at `−X_TRAVEL`** (col 0, row 0, LTR clearing slide), zero breaches — so the caps never bind today; they're the net for future offset/pitch/skew changes.

**The second catch is gated behind `#define FLIP_SECOND_CATCH`, OFF by default** (commented `//#define` near `FLIP_OFFSET_X` — uncomment to re-enable). With it OFF the arm parks at REST after the catch stroke and `catchByNextMove` is ignored. With it ON: the catch always sweeps **+dx by ≥16.8 mm**, opposite the return — which under the mirror means *against* the row's sweep direction on both row directions. So `catchByNextMove` can no longer be folded into the caller's next move and callers pass `false`; `flipDisc` always emits its own +dx stroke and re-parks at REST. (The parameter stays in the signature so re-enabling the second catch needs no signature change.) `SERVO_US_RELEASE2 = SERVO_US_RELEASE − SERVO_US_10_DEG` — still derived, so it followed RELEASE down to 704 µs base, and it is lidar-compensated at use.

Each cell logs one plog line while the clearing slide runs: `f x<gx> y<gy> L|R rel<us> unl<tenths mm> dx<tenths mm> miss<ackMisses>`.

**7. `onDisplayComplete()`** — `GET /complete.php?id=<N>` tells the server the image is confirmed displayed.

**8. Sleep 10 min** — release steppers (`$1=0` + tiny jog to trigger disable), then `delay(10 min)` before polling again. **Sync (`waitForIdle`) after the `$1=0` before the jog** — `$1=0` commits the settings block to the Mega's EEPROM, and grbl disables interrupts during the write, dropping the Serial1 RX bytes of anything pipelined behind it. Without the sync, the jog arrives garbled → `error:2` → the rest of the burst desyncs the `ok` accounting → 60 s `waitForIdle` watchdog → MCU reset. Same `waitForIdle`-after-`$1` guard is applied at boot and in `rehome()`.

---

## Color Classification

TCS3200 measures R/G/B/Clear light frequencies, read with **LED ambient-subtraction**: `readAmbientSubtracted()` averages `AMBIENT_FLASHES` (3) off/on flashes, each subtracting a 5-frame LEDs-off (ambient) read from a 5-frame LEDs-on (lit) read. This cancels room light so the value depends only on the disc + our own LEDs.

```
if clear <  SCAN_ON_CLEAR_MAX (6000)  →  cyan/on (1)
else                                  →  black/off (0)
```

Single threshold on the ambient-subtracted **clear** channel — the two faces separate by ~10× (on-cluster ~1.8k, off-cluster ~16k), so no model is needed (the ternary classifier was retired). **The sensor views the disc BACK**: a displayed-on (cyan-front) disc shows its black back → reads LOW clear; displayed-off shows its cyan back → reads HIGH. `classifyDisc` returns the FRONT/displayed color (`on = clear below threshold`), matching `gridState`'s convention. Ambient subtraction removed the old "blown regime" (bright ambient made every cell read the same → garbled draw), so the former `SCAN_C_CEILING` guard / re-init recovery is gone.

---

## GRBL Streaming

GRBL's RX buffer = 128 bytes. Code tracks how many bytes are in-flight:

- `sendGcode()` — blocks if buffer would overflow, then sends + tracks length
- `drainResponses()` — reads GRBL replies; each `ok` frees the bytes for that command
- `waitForMotion()` — sends `G4 P0` (dwell) + `waitForIdle()` so `ok` means motion *actually finished*, not just "parsed into planner"

---

## Error handling

| Situation | Response |
|---|---|
| GRBL `error:X` | Halt forever (`while(true)`) |
| WiFi/HTTP failure | Log, wait 10s, retry |
| Bitmap wrong length | Log, wait 10s, retry |
