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
poll server → got bitmap? → display it → verify it → mark complete → repeat
             no bitmap?  → wait 10s → repeat
```

### Step-by-step

**1. Poll** — `fetchNext()` opens a raw TLS socket to the server, `GET /next.php`

**2. Parse response**
- Status 200 + body = new bitmap (base64, 112 chars max)
- Body `"NONE"` or non-200 = queue empty, sleep 10s

**3. Decode** — base64 → 84 bytes = 666 bits (last 6 bits are unused padding), one bit per disc

**4. `displayBitmap()`** — iterate every cell; if `desired bit ≠ gridState`, call `flipDisc(x, y)`

**5. `flipDisc()`** — two-stage 180° rotation
```
move gantry to disc
servo → 80° (rotate)  → delay 500ms   # rotates squisk 90°
servo → 0° (rest)     → delay 500ms
G91; Y -dy; X +dx; Y +dy; G90         # arc around the disc body
servo → 25° (release) → delay 500ms   # arm now sits beside the half-rotated squisk
G91; X -dx; G90                       # slide back — the 25° arm pushes the squisk through the final 90°
servo → 0° (rest)     → delay 500ms
```
`dy = ±FLIP_OFFSET_Y (20 mm)` (sign inverts on the bottom row to stay inside the −Y soft limit); `dx = FLIP_OFFSET_X (16.8 mm)` capped so the arc never commands past `X=0`.

**6. Verify loop** (up to 10 passes)
- `verifyAndFix()`: scan cells with TCS3200, compare to desired bitmap. The scan iterates `x = 0..GRID_W-2` (rightmost column skipped); the existing `SCAN_OFFSET_X` is set up so the leftmost is likewise outside the sweep — verify covers the interior of the addressable grid only
- Any mismatch → re-flip that disc
- Loop until 0 mismatches or 10 passes hit

**7. `onDisplayComplete()`** — `GET /complete.php?id=<N>` tells the server the image is confirmed displayed

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
| Verify still wrong after 10 passes | Skip `complete.php`, leave entry pending |
