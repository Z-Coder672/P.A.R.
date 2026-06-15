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
poll server → got bitmap? → re-scan board → display it → release sweep → mark complete → 10 min sleep → repeat
             no bitmap?  → wait 10s → repeat
```

### Step-by-step

**1. Poll** — `fetchNext()` opens a raw TLS socket to the server, `GET /next.php`

**2. Parse response**
- Status 200 + body = new bitmap (base64, 112 chars max)
- Body `"NONE"` or non-200 = queue empty, sleep 10s

**3. Decode** — base64 → 84 bytes = 666 bits (last 6 bits are unused padding), one bit per disc

**4. `scanGrid()`** — re-home and re-read every cell with the TCS3200. The 10-min idle disables steppers, so position can drift and discs may have been moved; this reseeds `gridState[]` so `displayBitmap` only flips cells that actually differ.

**5. `displayBitmap()`** — over the band of rows that contain at least one differing cell, flip cells where `desired bit ≠ gridState`. Rows with no differing cells are **skipped entirely** (no move to them — the next flipping row is still entered via `moveToYSafe`, so Y travel stays at an X soft-limit). Returns the number of cells flipped (= cells that were wrong vs the target), used to gate the check pass.

**5b. Check pass** — after the first `displayBitmap()` completes, conditionally re-scan + re-fix. Two short-circuits, both keyed on `CHECK_FIX_MAX_SKIP` (=5):
- **First draw flipped ≤5 cells** → tiny job, few chances to fail → **skip the whole check pass** (no re-scan, no re-fix).
- **Otherwise** → re-run `scanGrid()` (reseeds `gridState[]` from the physical board, catching discs that didn't flip cleanly or were misclassified), then count mismatches vs the target. Re-run `displayBitmap()` **only if >5 cells are still wrong**; ≤5 are left as-is. The color sensor is ~99.5% accurate, so a full 666-cell scan misreads ~3 cells on average — ≤5 mismatches sit within that noise floor, so re-flipping them would more likely flip a *correct* disc than fix a real miss. Tolerating ≤5 doesn't lower display accuracy. One corrective pass at most.

**6. `flipDisc(x, y, catchByNextMove)`** — two-stage 180° rotation (+ optional second catch)

The absolute flip X is `grid[y][x].x + flipSkewX(y)`: a linear left-lean correction for the board not being perfectly square to the head. `flipSkewX` is 0 at the bottom row and `FLIP_SKEW_X_TOP` (−2.0 mm, toward homing/−X) at the top row, interpolated by physical height. Only the flip head shifts — scanning still targets cell centers. The −dx return slide and the X=0 soft-limit cap both use the skewed X. Calibrate the magnitude with `FlipLeftColumnTest`.
```
move gantry to disc (skew-corrected X)
servo → ENGAGE (90°)   → settle 300ms  # rotates squisk 90°
servo → REST (0°)      → settle 300ms
G91; X +dx; G90                        # clear the disc column
servo → RELEASE (46°)  → settle 100ms
G91; X -dx; G90                        # return slide — the 46° arm pushes the squisk through the final 90°
#ifdef FLIP_SECOND_CATCH               # default OFF — commented-out //#define near FLIP_OFFSET_X
  servo → RELEASE2 (~36°)→ settle 100ms # second catch: arm a further ~10° lower
  if !catchByNextMove:                  # only when the next move won't already do it
    G91; X +dx2; G90                    # explicit +X sweep (opposite the return)
    servo → REST (0°)    → settle 100ms
#else                                   # second catch disabled
  servo → REST (0°)      → settle 100ms # park; catchByNextMove ignored
#endif
```
`dx = FLIP_OFFSET_X (16.8 mm)` capped so the slide never commands past `X=0`. **The second catch is gated behind `#define FLIP_SECOND_CATCH`, OFF by default** (commented `//#define` near `FLIP_OFFSET_X` — uncomment to re-enable). With it OFF the arm parks at REST after the return slide and `catchByNextMove` is ignored. With it ON: the catch always sweeps **+X by ≥16.8 mm** (opposite the −dx return) to push back any disc the first catch left over/under-rotated, and `catchByNextMove` lets the caller fold that sweep into a move it was already making — on an LTR row with another flip ahead, the next `flipDisc`'s opening move travels +X by ≥1 cell pitch (20.045 mm), so the arm is left down at RELEASE2 and that move *is* the catch (no extra G-code); on RTL rows, row ends, and the last flip of a row, `flipDisc` emits its own +X stroke and re-parks at REST. `SERVO_US_RELEASE2 = SERVO_US_RELEASE − SERVO_US_10_DEG` (offset, not a hardcoded µs).

**7. `releaseSweep()`** — serpentine top-to-bottom pass with servo parked at REST. Lets any half-rotated discs settle before the snapshot photo is taken. No sensing, no flipping.

**8. `onDisplayComplete()`** — `GET /complete.php?id=<N>` tells the server the image is confirmed displayed.

**9. Sleep 10 min** — release steppers (`$1=0` + tiny jog to trigger disable), then `delay(10 min)` before polling again. **Sync (`waitForIdle`) after the `$1=0` before the jog** — `$1=0` commits the settings block to the Mega's EEPROM, and grbl disables interrupts during the write, dropping the Serial1 RX bytes of anything pipelined behind it. Without the sync, the jog arrives garbled → `error:2` → the rest of the burst desyncs the `ok` accounting → 60 s `waitForIdle` watchdog → MCU reset. Same `waitForIdle`-after-`$1` guard is applied at boot and in `rehome()`.

---

## Color Classification

TCS3200 measures R/G/B/Clear light frequencies.

```
if B >= C × 28  →  black (0)
else            →  blue  (1)
```

B/C ratio is the single cleanest discriminator on this hardware. Sensor sits offset `(+4.5, -31.8)mm` from the flip head.

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
