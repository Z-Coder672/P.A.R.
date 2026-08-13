# P.A.R.Main Flow

## Hardware in 5 seconds

| Thing | What it does |
|---|---|
| Arduino Nano ESP32 | Main brain — WiFi, HTTP, NTP clock, servo, color sensor |
| Arduino Mega + GRBL | Motion controller — moves the gantry via Serial1 |
| Servo (D9) | Flips individual discs (black ↔ blue) |
| TCS3200 (D4–D8) | Color sensor — reads which side of a disc is showing |
| VL53L4CD (A4/A5, I²C) | ToF ranger — per-cell standoff, daily 10:00 scan only |

---

## Boot (`setup`)

1. **Init grid coords** — 37×18 = 666 cells, each mapped to a physical (X, Y) in mm (negative coords because GRBL homes to full-negative). Starting X offset = 25 mm.
2. **Attach servo**, set to 0°
3. **Configure TCS3200** — S0/S1 = HIGH/LOW → 20% freq scaling (full speed is too fast to measure)
4. **Wait 2s** for GRBL to boot, flush any junk bytes
5. **Send G-code init** — `G21` (mm), `G90` (absolute), then `$H` to home the CNC
6. **Connect WiFi** → `timeBegin()` starts NTP → `timeWaitForSync(30 s)` (bounded — a dead network delays the schedule, it does not block the rig) → `cadenceLoadRecord()` reads the last lidar scan's date off flash → `lidarEnsure()` brings the VL53L4CD up so a wiring fault surfaces in the log at boot, not at 10:00 → ready to poll. The boot `scanGrid()` is commented out — the first color scan happens lazily in `loop()` (first job, `gridStateFresh` false).

Nothing is *scheduled* from `setup()`. The boot-time night sleep and the "we booted at 14:00 and still owe today's lidar scan" case are both handled by `cadenceGate()` at the top of `loop()`, so the whole schedule lives in one place.

---

## Main Loop (`loop`)

```
cadence gate → night (20:00–09:59)? → sleep until 10:00
             → scan owed but it's 18:00 or later? → sleep until 10:00, scan then
             → today's lidar scan not done? → run it (~50 min), persist to flash
poll server  → got bitmap? → re-scan board (skipped if last action was a clean scan) → display it → check pass → snapshot request → mark complete
                             → then: past 20:00? → sleep until 10:00 : 10 min linger
             → no bitmap?  → wait 10s → repeat
```

### Step-by-step

**0. `cadenceGate()`** — runs before any server ingestion. See **Daily Cadence** below. With no valid wall clock it returns immediately and the loop behaves exactly as it did before the cadence existed.

**1. Poll** — `fetchNext()` opens a raw TLS socket to the server, `POST /next.php` (must be POST — intermediates may silently retry GETs, and each retry would pop a queue item the board never sees)

**2. Parse response**
- Status 200 + body = new bitmap (base64, 112 chars max)
- Body `"NONE"` or non-200 = queue empty, sleep 10s

**3. Decode** — base64 → 84 bytes = 666 bits (last 6 bits are unused padding), one bit per disc

**4. `scanGrid()` (conditional)** — re-home and re-read every cell with the TCS3200. **Skipped when `gridStateFresh` is set**, i.e. when `gridState[]` is already a *measured* picture of the board: on the first job after boot (reusing `setup()`'s scan), and after any job whose last board-touching action was a scan with no fixing after it — the check pass re-scanned and found ≤`CHECK_FIX_MAX_SKIP` wrong, or the draw flipped nothing at all (`gridStateFromScan`, carried across the idle at the end of the job). That scan already describes the final board, so repeating it costs ~70 min to learn what was just measured. Any flip after a scan makes the state *inferred* rather than measured (`displayBitmap` clears `gridStateFromScan` before its first flip), and the next job re-scans. When the scan is skipped, `rehome()` still runs if `needsRehome` is set — the end-of-job `$1=0` releases the steppers, so the gantry may have been nudged.

Otherwise the full sweep runs. It reseeds `gridState[]` so `displayBitmap` only flips cells that actually differ. The reading sweep runs with the **flip arm parked at REST** (`SERVO_US_SCAN` = `SERVO_US_REST` = 544 µs, changed 2026-08-10). It used to be dropped to ~33.5° so that — since the sensor trails the flip head by `SCAN_OFFSET_X` (−24.005 mm ≈ one cell pitch) — the lowered arm brushed the board and pushed through any squisk left at stage 1 (90°, half-rotated). That secondary settling job is abandoned: the arm now stays up for the whole scan, so the "carriage moves only with the servo at REST" invariant holds trivially (a dropped arm dragged across a populated board is what snapped the flip arm before), and half-rotated discs are no longer nudged during scanning — one left mid-rotation stays mid-rotation until the check pass re-flips it. Each cell is read with **LED ambient-subtraction** and classified by a simple clear-channel threshold (see Color Classification below); ambient subtraction removed the old room-light "blown regime", so the former `SCAN_C_CEILING` guard / sensor re-init recovery is gone.

**5. `displayBitmap()`** — over the band of rows that contain at least one differing cell, flip cells where `desired bit ≠ gridState`. Rows with no differing cells are **skipped entirely** (no move to them — the next flipping row is still entered via `moveToYSafe`, so Y travel stays at an X soft-limit). Returns the number of cells flipped (= cells that were wrong vs the target), used to gate the check pass.

**5b. Check pass** — after the first `displayBitmap()` completes, conditionally re-scan + re-fix. Two short-circuits, both keyed on `CHECK_FIX_MAX_SKIP` (=5):
- **First draw flipped ≤5 cells** → tiny job, few chances to fail → **skip the whole check pass** (no re-scan, no re-fix).
- **Otherwise** → re-run `scanGrid()` (reseeds `gridState[]` from the physical board, catching discs that didn't flip cleanly or were misclassified), then count mismatches vs the target. Re-run `displayBitmap()` **only if >5 cells are still wrong**; ≤5 are left as-is. The color sensor is ~99.5% accurate, so a full 666-cell scan misreads ~3 cells on average — ≤5 mismatches sit within that noise floor, so re-flipping them would more likely flip a *correct* disc than fix a real miss. Tolerating ≤5 doesn't lower display accuracy. One corrective pass at most.

When the check pass ends on that skip-the-fix branch, its scan is the last thing that touched the board — so it is carried over as the *next* job's scan (see step 4) instead of being thrown away.

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

**8. Release steppers, then linger *or* sleep** — `releaseSteppers()` sends `$1=0` + a tiny jog (`G0 X-0.1`, away from the `X=0` soft limit) to trigger the disable, and sets `needsRehome`. **Sync (`waitForIdle`) after the `$1=0` before the jog** — `$1=0` commits the settings block to the Mega's EEPROM, and grbl disables interrupts during the write, dropping the Serial1 RX bytes of anything pipelined behind it. Without the sync, the jog arrives garbled → `error:2` → the rest of the burst desyncs the `ok` accounting → 60 s `waitForIdle` watchdog → MCU reset. Same `waitForIdle`-after-`$1` guard is applied at boot and in `rehome()`.

Then the cadence decides how long to wait: local hour ≥ 20 → `sleepUntilMorning()`, otherwise the usual `delay(10 min)`. The check is here — not only at the top of `loop()` — so the rig never starts an hour-long job that would finish deep into the evening and then immediately start another.

---

## Daily Cadence

Three coupled pieces: a real wall clock, a daily lidar standoff scan, and a night sleep. The board is powered 24/7, so this is a **schedule**, not power management.

**Wall clock.** NTP over the WiFi link the poller already keeps up: `configTzTime(PAR_TZ, "pool.ntp.org", "time.nist.gov")`, timezone `MST7MDT,M3.2.0,M11.1.0` (US Mountain with DST) as a named constant. Started once WiFi is up and re-asserted (idempotently) at the top of every loop; SNTP re-polls on its own and survives a WiFi bounce. **Every** cadence decision goes through `localNow(struct tm&)`, which returns false unless `time()` exceeds `TIME_VALID_FLOOR` (2025-01-01 UTC) — the ESP32 RTC starts at the epoch and SNTP fills it in *asynchronously*, so the clock is readable long before it is true. With no valid clock the rig does not scan, does not sleep, and does not mark a day done; it just runs uncadenced and logs that once every 5 min.

**Daily lidar scan (10:00 local).** `runDailyLidarScan()` sweeps the VL53L4CD over all 666 cells and writes the result to flash. It runs when the clock says the hour is ≥ 10 **and** the flash record's date isn't today — which covers both "it just turned 10:00" and "we rebooted at 14:00 and owe today's scan". An owed scan is only **started before 18:00** (`LIDAR_SCAN_CUTOFF_HOUR`): at/after the cutoff the day is written off — the rig sleeps to 10:00 and runs the scan on wake — rather than sweeping ~50 min into the evening and then printing into the night. Sensor mechanics are ported from `ScanColorLidarTest`: free-running at a 50 ms timing budget, one fresh 4 s window per cell (~80 samples), reduced to a **20 %-trimmed mean in tenths of a mm**; head offsets `LIDAR_OFFSET_X = +31.075` (calibrated so col 36 targets exactly the `X=0` machine limit) and `LIDAR_OFFSET_Y = SCAN_OFFSET_Y + 6.0 = +14.0` (the top row's target is −14.2 + 14 = −0.2, inside the envelope — this is what the `$131` 406→412 bump bought). Targets are clamped on **both** axes (`clampScanY`, and `clampScanX` as the mirror safety net).

Readings are stored **RAW**, matching that sketch's `LIDAR_APPLY_CALIBRATION 0` run: the back-colour distance offsets are only meaningful against a same-pass colour classification, and this sweep does not run the colour sensor. A half-applied correction would be worse than none — the correction belongs in the offline fit that produces `LIDAR_FIT_CMM`.

Motion obeys the same conventions `scanGrid()` does, for the same reasons: **flip arm parked at `SERVO_US_REST` for the whole pass** (a dropped arm dragged across a populated board is what snapped it before), serpentine order via `cellAt()` so end-of-row X equals start-of-next-row X, every cross-row leg through `moveToYSafe` (pure-Y travel only at an X soft limit), intra-row moves are pure X, nothing sensed until `waitForMotion()` confirms the carriage stopped, and a mid-scan `rehome()` after row 8 so step drift can't skew the second half. Per-cell flash writes are pipelined behind the travel to the next cell, exactly as in `scanGrid`. `gridState[]` is **not** touched — no disc is flipped, so `gridStateFresh`/`gridStateFromScan` carry through unchanged; `rehome()` clears `needsRehome`. Cost is ~4.3 s/cell ≈ **50 minutes**, which is why it is scheduled once a day and not paid by the print loop.

If the ranger never initialises, `lidarEnsure()` gives up after 10 attempts (re-initing I²C between them) and — **unlike `ScanColorLidarTest`, which halts** — the rig carries on printing. The day is recorded with `sensorOk = 0` and no cells, so a dead ranger costs one day of data instead of putting the rig in a loop that re-attempts a 50-minute sweep all day.

**Flash record.** One fixed-layout `LidarScanRecord` at `/cadence.bin` on the **same LittleFS mount plog uses** — the factory `ffat` partition (`LittleFS.begin(true, "/littlefs", 10, "ffat")`; see `persistent_log.cpp` for why it is not `spiffs`). plog mounts it at boot; `cadenceFsReady()` re-calls `begin()` only as a guard for the case where that mount *failed*, which returns true immediately when the label is already mounted, so plog's files are never touched or reformatted. The struct holds magic / version / grid geometry / `sensorOk` / local `(tm_year, tm_yday)` / unix epoch / OK-cell count / `uint16_t dist10[666]` / an FNV-1a checksum over everything preceding it. Written temp-then-rename, same crash policy as plog's rotation. Anything unexpected on load — missing, wrong size, wrong magic/version/geometry, bad checksum — is treated as "no scan on record", so the failure direction is *re-scan*, never *skip*. **The stored `(year, yday)` is the sole authority for "has today's scan run"** — not a RAM flag — so a reboot at any hour resumes the schedule correctly.

`LidarScanRecord` is declared near the **top** of the .ino, next to `GRID_W`/`GRID_H` and far from the code that uses it, for the same reason `TcsFilter` is: the Arduino IDE injects its auto-generated forward declarations immediately above the first function in the file, so any type named in a function signature must be complete by that point.

**Night sleep (20:00 → 10:00).** `isNightHour(h)` is `h >= 20 || h < 10`, written once so the post-print check and the boot-time check can never disagree about the boundary. `sleepUntilMorning()` parks the servo at REST, releases the steppers via `releaseSteppers()` if they're still energised, then idles: `delay(30 s)` ticks until an epoch wake target — the next 10:00 local, computed with `mktime` (`tm_isdst = -1`, so a sleep spanning a DST change still ends at 10:00 wall-clock) — with an hourly heartbeat line and a **17 h backstop** (longer than the longest legitimate sleep, 18:00 → 10:00 = 16 h in the past-cutoff case; short enough that a stuck clock costs one day rather than forever). The wake condition is an epoch compare, not "hour left the night window", because the past-cutoff owed-scan case enters sleep as early as 18:00 where an hour-window test would bounce straight out. On wake it re-establishes WiFi and re-asserts NTP. Nothing is polled and nothing is ingested during the sleep.

It is a plain `delay()` loop, **not** light or deep sleep, on purpose: `delay()` yields to FreeRTOS so the idle task still feeds the watchdogs, the `millis()`-based GRBL/WiFi stall watchdogs stay coherent, and no reset-cause breadcrumb is fabricated — deep sleep would reboot the SoC, log `ESP_RST_DEEPSLEEP`, and re-run the whole homing sequence, defeating the point of that breadcrumb. The RTC free-runs without the network, so the wake decision never depends on NTP staying reachable.

Ordering in `cadenceGate()` is sleep **first**, then scan, so the morning wake always lands on the scan check and the day's first print draws against fresh standoff data.

Log lines: `ntp start`/`ntp synced`/`ntp NOT synced`, `cadence: record …`, `cadence: lidar scan begin|end`, per-cell `ld y<y>c<x> <mm.t> n<samples> mn<min> mx<max>`, `cadence: night sleep at HH:MM until 10:00`, `cadence: sleeping HH:MM`, `cadence: wake`, `cadence: no valid clock - running uncadenced`.

---

## Color Classification

TCS3200 measures R/G/B/Clear light frequencies, read with **LED ambient-subtraction**: `readAmbientSubtracted()` averages `AMBIENT_FLASHES` (3) off/on flashes, each subtracting a 5-frame LEDs-off (ambient) read from a 5-frame LEDs-on (lit) read. This cancels room light so the value depends only on the disc + our own LEDs.

```
if channel <  SCAN_ON_BLUE_MAX (3535)  →  cyan/on (1)
else                                   →  black/off (0)
```

Single threshold on the ambient-subtracted channel that `tcsReadRGBC` returns as `c` — the two faces separate by ~10×, so no model is needed (the ternary classifier was retired). **That channel is physically BLUE, not clear:** the TCS3200 S2/S3 select lines are crossed on this rig, so the `TcsFilter` enum labels are wrong. Do **not** "fix" the wiring — blue discriminates the two disc faces 10.21× where true clear manages only 2.22×. 3535 is the geometric mean of the measured populations (black-back ceiling 1628, cyan-back floor 7677), replacing a historical 6000 that was lopsided toward the failing side. Full measurements are in `PARMain.ino` at `SCAN_ON_BLUE_MAX`. **The sensor views the disc BACK**: a displayed-on (cyan-front) disc shows its black back → reads LOW clear; displayed-off shows its cyan back → reads HIGH. `classifyDisc` returns the FRONT/displayed color (`on = clear below threshold`), matching `gridState`'s convention. Ambient subtraction removed the old "blown regime" (bright ambient made every cell read the same → garbled draw), so the former `SCAN_C_CEILING` guard / re-init recovery is gone.

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
| GRBL `error:N` | Re-send the same command, up to `MAX_ERROR_RETRIES` (10) at 3 s spacing per same-command run; a clean `ok` resets the counter. Exhausted → park servo → `esp_restart()` |
| GRBL `ALARM` | `grblAlarmRecover()` — Ctrl-X soft reset → `$H` → reassert modals → clear the queue → force a re-scan next job. Any sub-step failing → `esp_restart()` |
| GRBL comms wedged (60 s no progress) | `grblStallReset()` — park servo → `esp_restart()` |
| Servo command unacked (`SERVO_ACK_MODE 2`) | Re-send and **retry forever**. Blocking is the safe failure: every call site is reached with GRBL idle. Deliberately does *not* reset — a reset re-homes, and homing with the arm possibly at ENGAGE is how the arm broke |
| WiFi down > 60 s | Park servo → `esp_restart()` |
| WiFi/HTTP failure | Log, wait 10s, retry |
| Bitmap wrong length | Log, wait 10s, retry |
| VL53L4CD init fails ×10 | Log, record the day with `sensorOk = 0`, **keep printing** |
| NTP never syncs | Log every 5 min, run uncadenced (no scan, no sleep) |
