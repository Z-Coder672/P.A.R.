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

**5. `displayBitmap()`** — iterate every cell; if `desired bit ≠ gridState`, call `flipDisc(x, y)`

**5b. Check pass** — after the first `displayBitmap()` completes, re-run `scanGrid()` (reseeds `gridState[]` from the physical board, catching any disc that didn't flip cleanly or was misclassified) then `displayBitmap()` again, which re-flips only the cells still differing from the target. One pass.

**6. `flipDisc(x, y, catchByNextMove)`** — two-stage 180° rotation (+ optional second catch)
```
move gantry to disc
servo → ENGAGE (90°)   → settle 300ms  # rotates squisk 90°
servo → REST (0°)      → settle 300ms
G91; X +dx; G90                        # clear the disc column
servo → RELEASE (38°)  → settle 100ms
G91; X -dx; G90                        # return slide — the 38° arm pushes the squisk through the final 90°
#ifdef FLIP_SECOND_CATCH               # default OFF — commented-out //#define near FLIP_OFFSET_X
  servo → RELEASE2 (~28°)→ settle 100ms # second catch: arm a further ~10° lower
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

**9. Sleep 10 min** — release steppers (`$1=0` + tiny jog to trigger disable), then `delay(10 min)` before polling again.

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
