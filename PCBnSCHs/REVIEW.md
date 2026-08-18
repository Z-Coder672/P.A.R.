# P.A.R. Shield — design review (Board3 / PCB3, 2026-08-16)

2-layer, 90.68 × 51.82 mm. Reviewed from the netlist, schematic SVG, per-layer renders,
Gerbers + drill files, and the BOM, cross-checked against the firmware and this project's
documented failure history.

Four independent review passes (power, signal levels, layout/EMI, BOM + failure-history
regression) plus reconciliation. Findings labelled **MEASURED** come from parsing Gerber
apertures / the netlist directly; **INFERRED** comes from renders or datasheets; anything
unresolved is listed in [Needs a bench measurement](#needs-a-bench-measurement).

---

## Verdict

The board is **close**, and several things in it are genuinely good — the ground pour is
excellent, the connector silkscreen is better than most hobby boards, the main MCU gets its
own 12 V→VIN rail (a real structural fix for the brownout history), and the Mega→ESP32
level-shift audit item that `PINOUT.md §2` has been carrying open is finally addressed.

But **it will not power up as designed**, and the servo supply path — the single most
expensive failure in this project's history — is worse on this board than on the harness it
replaces. Three netlist-level defects must be fixed before fab; one layout pass fixes most
of the rest.

---

## Blocking — the board cannot work as drawn

### B1. The switched rail is never enabled — MEASURED
`U4.3` (TPS22919 `ON`) ← net `5VSW` ← `U1.20` = **D11**. No sketch in the repo drives D11;
`PINOUT.md §2` lists D11–D13 as unused. The part has an internal **530 kΩ smart pull-down**,
so `ON` is held low and `5VSWED` stays **off** — and `5VSWED` powers the servo (J7), the
ServoNano (U2.27), the lidar (J6) and the colour sensor (J8). Everything downstream is dark.

**Fix (hardware, preferred): 100 kΩ from `U4.3` to 3V3.** Against the 530 kΩ pull-down that
gives 3.3 × 530/630 = **2.78 V**, far above the 1.0 V `ON` V_IH → rail on by default, and all
~22 existing sketches work unmodified while D11 remains an optional software kill.
**Pull it to 5 V and you put 5 V on D11 — don't.** Requires B4 (route 3V3).

Firmware alternative — `pinMode(D11, OUTPUT); digitalWrite(D11, HIGH);` first thing in
`setup()` — works but must be replicated in every sketch that uses the shield, and leaves a
window during reset/boot where the ESP32 tri-states D11 and the rail drops.

> **Why the firmware-only route is not enough.** The ESP32 tri-states its GPIOs on reset,
> brownout and boot, and this rig has *confirmed* brownout resets. Gating the servo rail on
> an MCU pin means a brownout now cuts servo power mid-flip: the arm goes limp wherever it
> is, `PARMain` reboots and runs `$H`, and homing drags the carriage with the arm possibly
> still at ENGAGE. That is precisely the sequence that snapped the flip arm, and precisely
> what `SERVO_ACK_MODE 2` was designed to prevent by never resetting on an unconfirmed arm
> position. **The servo should not be behind an MCU-gated switch at all** (see C1).

### B2. Both level-shift dividers are out of spec — MEASURED
R2/R3 (Mega TX1 → `U1.32` = D0) and R4/R5 (ServoNano D3 ack → `U1.29` = D2) are both
**2 k / 2 k**. ESP32-S3 V_IH = 0.75 × VDD = **2.475 V** (datasheet Table 5-4).

| Source condition | Divider out | Margin |
|---|---|---|
| Ideal 5.000 V | 2.500 V | +25 mV |
| Realistic V_OH ≈ 4.90 V | 2.450 V | **−25 mV — fails** |
| Mega on USB, rail 4.75 V | 2.375 V | **−100 mV — fails** |
| VDD +2 % → V_IH 2.525 V | 2.500 V | **−25 mV — fails** |

This works only when the source is exactly 5.00 V *and* VDD is exactly 3.30 V. Symptoms:
framing garbage on the 115200 GRBL link (surfacing as `error:N` retries → 60 s watchdog
`esp_restart()`), and missed acks → infinite retry stall.

**Fix: R3 and R5 → 3.3 kΩ.** Keeping 2 k on top gives 3.11 V; going to the documented
1.8 k/3.3 k gives 3.24 V. Either is fine and both stay under the 3.6 V abs max. Four
resistors, no layout change.

RC is a non-issue at both baud rates (τ ≈ 10 ns against an 8.68 µs bit at 115200) — **do not
attribute any GRBL link problem to divider bandwidth. It is entirely the DC level.**

### B3. The limit-switch half of J9 is electrically dead — MEASURED
`LIMSW-` is a **single-node net** (`J9.3` only — the only one on the board). `LIMSW+`
reaches `C1.2` and `J9.2` and nothing else. Neither touches an MCU pin, and C1 (1 nF) is
debouncing a net connected to nothing.

Note the main MCU has no limit-switch role in firmware — homing limits live on the
Mega/CNC Shield V3. So either route these to a free GPIO (D3, D12, D13) with a pull-up, or
delete the switch pins and C1 and mark them NC.

Related: **there is no `LED+` net anywhere on the board.** Only `LED-` (Q1 drain) exists, so
the LED bank's anode feed and any current limit stay off-board and undocumented. A tidy fix
for both: repurpose J9 as the LED connector — `J9.2` → LED+, `J9.1` → LED−, drop `J9.3`/C1.

### B4. 3V3 is not routed anywhere — MEASURED
Net `$1N5429` = `J2.2` ↔ `U1.3` only: the 3.3 V rail exists on the socket and goes nowhere.
It is the enabler for B1's `ON` pull-up, the I²C pull-ups (C5), and moving the lidar off 5 V
(C6). **Routing 3V3 as a board net is the single highest-leverage change available.**

---

## Critical — electrical

### C1. The servo droop budget is spent on the board, before any wire — MEASURED
`PINOUT.md §7` sets the target at **≤ 0.2–0.3 V total drop at stall, measured at the servo
connector**. Measured path from J1 to the servo and back:

| Segment | R | Drop @ 1.5 A |
|---|---|---|
| J1.3 → U4.1 (`+5V` in, 80 mil) | 14.1 mΩ | 21 mV |
| **U4 TPS22919 R_on** (88 mΩ typ / ~120 mΩ hot) | 88–120 mΩ | **132–180 mV** |
| **U4.6 → J7.1 (`5VSWED`): 82.5 mm of 17-mil** | **93.6 mΩ** | **140 mV** |
| GND plane J7.2 → J1.2 (FD solve) | 2.3 mΩ | 3.5 mV |
| **Board total** | **198–230 mΩ** | **300–345 mV** |

The rail travels **82.5 mm** for a ~35 mm straight-line distance — west, down the left edge,
32 mm east along the bottom, then north-east to J7. Thermally the 17-mil is fine (ΔT ≈ 14 °C
at 1.5 A); **resistively it is the whole problem.** Every millivolt the harness rework bought
is handed straight back.

**Fix, in order of value:**
1. **Take the servo off the load switch.** Feed `J7.1` from `+5V` directly. Removes
   88–120 mΩ and the 1.5 A ceiling at a stroke, and fixes B1's brownout interaction.
2. Move U4 and C2 beside J7; route VOUT→J7 as a short 80-mil trace or a top-layer pour —
   30 mm at 80 mil is **7 mΩ**, a 13× improvement.
3. At minimum widen `5VSWED` to ≥ 60 mil (→ 26.5 mΩ) and pour it on the empty top layer.

### C2. The switched rail is a daisy chain with the servo *last* — MEASURED
Chain order from U4: ServoNano 5 V (`J5.12`, 36.5 mΩ) → lidar (`J6.1`, 44.5 mΩ) → TCS3200
(`J8.6`, 90.4 mΩ) → **servo (`J7.1`, 93.6 mΩ)**. Because the largest load is at the far end,
the full 1.5 A flows through every sensor's supply trace. Node sag at stall, relative to
U4 VOUT: ServoNano **−55 mV**, lidar −67 mV, TCS3200 **−136 mV**.

So a flip stall pulls down the supply of the microcontroller generating the servo pulse, and
the colour sensor's supply, simultaneously and by construction. Your brownout note
prescribes *"separate the logic supply from the servo/stepper supply with a common ground"* —
this does the opposite.

**Fix:** star the rail from a single node, highest current first; better, give the servo its
own feed from J1 and leave only sensors behind U4.

### C3. Zero decoupling on the entire board — MEASURED
The complete capacitor census is **C2 (1500 µF, on the *input* side of the switch, 27.7 mm
away) and C1 (1 nF, on the dead LIMSW+ net)**. There is not one 100 nF anywhere, and
**`5VSWED` — feeding servo, ServoNano, lidar and colour sensor — has no capacitor at all.**
TI's datasheet *requires* a local input cap on U4; none exists.

**Fix:** 10 µF X7R + 100 nF within 2 mm of `U4.1`; bulk + 10 µF + 100 nF **at J7**; 100 nF at
J6 and J8; 100 nF at each module's 5 V/GND pins.

⚠ **Inrush ceiling if you keep the switch:** the TPS22919's slew is fixed at 3.2 mV/µs, so
turn-on inrush = C_out × 3.2 mA/µF. 220 µF → 0.70 A; **470 µF → 1.50 A, at the device limit.**
Cap C_out at ~220 µF behind U4. If the servo moves to unswitched `+5V` (C1), this constraint
disappears and you can fit 470–1000 µF at J7.

### C4. R5 destroys the ack line's fail-loud property — MEASURED topology
With R5 (2 kΩ) permanently soldered from the ack node to GND, a broken ack wire, an unplugged
ServoNano, or an **unpowered** ServoNano leaves D2 held at

> 3.3 × 2k/(2k + 45k) = **0.14 V** — a hard LOW, far below V_IL = 0.825 V

The ack is **active-LOW**, so a dead ack source reads as **permanently acknowledged**.
`servoAckSeen()` returns true immediately and `SERVO_ACK_MODE 2`'s retry-until-confirmed
enforcement — the thing credited with preventing the arm break — passes vacuously.

**This is a topology problem, not a ratio problem.** The proven 1.8 k/3.3 k gives 0.23 V and
fails identically. No resistor value fixes it while the bottom leg is populated.

**Fix — cheapest is firmware, today:** `servoAckWaitIdle()` already detects this (the line
never returns HIGH, so every command carries an extra ~100 ms). Promote that timeout from a
silent diagnostic to a **hard fault**. Three lines, no hardware change.
Proper respin: open-drain + pull-up, or a buffer whose absence reads HIGH.

> **`PINOUT.md §3b` is wrong and should be corrected.** It claims *"an unfitted or broken wire
> reads HIGH = 'no ack' = fails loud."* That holds only when the connection to D2 itself is
> absent. A break **upstream** of the divider reads LOW and fails **silent**. The doc is also
> internally inconsistent — its own validation section describes the stuck-low vacuous pass
> and its +100 ms signature. On the breadboard the divider lived on the removable harness, so
> "unfitted" was the likely failure; soldering it down makes the silent case the likely one.

### C5. No I²C pull-ups — MEASURED
`SDA` = {`J2.8`, `J6.3`, `U1.9`} and `SCL` = {`J2.9`, `J6.4`, `U1.10`} — three pins each, no
resistor, no footprint. Firmware runs `Wire.setClock(400000)`, which needs t_r ≤ 300 ns:

| Pull-up | Max bus C at 400 kHz |
|---|---|
| 45 kΩ (ESP32 internal) | **7.9 pF — impossible** |
| 10 kΩ (typical breakout) | 35 pF — marginal |
| 2.2 kΩ | 161 pF ✓ |

`PINOUT.md §5` records the lidar's boot init failing *"roughly half of observed power-ons"*,
then running a 71-minute pass flawlessly once up — a signature that fits init-sequence timing
sensitivity. **Slow rise time is a credible root cause.**

**Fix:** 2.2 kΩ (or 4.7 kΩ) from SDA and SCL to **3V3**. Free diagnostic before any respin:
drop `Wire.setClock()` to 100000 and see whether the boot-failure rate changes.

### C6. 5 V into non-5 V-tolerant pins, in two places
- **TCS3200 `OUT` → D8, bare copper.** `J8.6` = `5VSWED` (5 V), so the sensor runs at 5 V and
  its push-pull `OUT` (V_OH ≈ 4.6–5.0 V) goes straight to `U1.23` = D8. ESP32-S3 abs max is
  VDD+0.3 = **3.6 V**. This is the line `pulseIn` hammers for ~70 min per full-board scan.
  *Caveat: `PINOUT.md` never recorded the sensor's supply voltage on the existing rig, so this
  may be inherited rather than introduced — check with a meter before judging it a regression.*
  **Fix:** move `J8.6` to 3V3 (TCS3200 runs 2.7–5.5 V; its V_IH is 2.0 V so 3.3 V drive on
  S0–S3 is legal) — but note output frequency scales with VDD, so `SCAN_ON_BLUE_MAX = 3535`
  must be re-derived. Calibration-preserving alternative: 1 k series + 2 k to GND on `OUT`.
- **Lidar I²C.** `J6.1` feeds the VL53L4CD breakout from 5 V while SDA/SCL run straight to
  A4/A5. Safe *only* if that specific carrier level-shifts and doesn't reference its pull-ups
  to VIN. **Measure SDA idle voltage at `J6.3` before powering the board.** Feeding `J6.1`
  from 3V3 removes the question entirely.

### C7. Q1 (2N7002) — under-driven, undersized, no gate pull-down — MEASURED + datasheet
`GATE` = {`J3.13`, `Q1.1`, `U1.21`} — no pull-down, no series resistor. The 2N7002's
R_DS(on) is characterised at V_GS = 10 V and 4.5 V, **never at the 3.3 V the ESP32 drives**,
and V_GS(th) runs to 2.5 V (3.0 V on some vendors' parts) — so a worst-case device has under
1 V of overdrive. Continuous I_D is only ~115 mA (Vishay) / 300 mA (Diodes).

Two consequences worth taking seriously:
- **No gate pull-down** ⇒ the LED bank's state is undefined through reset, boot and brownout.
  `readAmbientSubtracted()` is only valid if the bank is definitively **off** during the OFF
  flash — this is a correctness risk, not cosmetic.
- Operating near threshold, channel resistance drifts with die temperature, so **LED
  brightness drifts over a long scan**. That is a *candidate* mechanism for the unresolved
  "12 false positives on a 70-min full-board pass, 0/888 on the 13-min bottom-rows test"
  asymmetry — both predict long-run-only failure. **Hypothesis, not a diagnosis.**

**Fix:** 100 kΩ gate→GND (mandatory, cheap), ~100 Ω gate series, and swap to a logic-level
part specified at V_GS = 2.5 V — **AO3400A** or DMG3414U.

### C8. R1 (2 kΩ) on the servo command line raises noise gain ~50×
DC-harmless (AVR input leakage ≤ 1 µA → 2 mV drop), but it lifts the ServoNano RX node from
~40 Ω to 2 kΩ — on the exact net `CLAUDE.md` nominates as the untested stepper-EMI coupling
path, and the one the bench rig (steppers unpowered, 91 kB, zero dropouts) could not
reproduce. **Fix: 100–330 Ω.**

Optional, if servo-link reliability stays open: R1 = 1 kΩ plus a 10 kΩ pull-up from the
ServoNano-side node to 5VSWED gives V_high = 3.455 V, a solid +455 mV over the AVR's 3.00 V
V_IH. Only adopt once rail sequencing (B1) is deliberate, or 0.45 mA flows into the ESP32
clamp when 5VSWED is live and the ESP32 isn't.

### C9. J12 has no ground pin — MEASURED
`J12` uses only pins 3 (`ESP32TX`) and 4 (`ESP32RX`). The only GND pins on any header are
`J11.35/36`. A 115200 UART with its return several inches away through a different connector
is exactly how you get the coupling the EMI hypothesis describes. **Fix: assign J12.1 and/or
J12.2 to GND** (both free) and route them flanking TX/RX.

### C10. No decoupling on the ESP32 VIN node — MEASURED
`+12V` → 15-mil trace → D1 → `U1.16`. **No capacitor on either the +12 V net or the D1
cathode.** If 12 V is shared with the stepper supply, every commutation transient lands on
the Nano's regulator input unbuffered — on a board with a confirmed brownout history.
**Fix:** 100 µF/25 V + 100 nF at the D1 cathode, 100 nF at the anode. Widen +12 V to 25 mil.

### C11. Two 5 V sources shorted together — MEASURED
`+5V` is driven from **both** `J1.3` (screw terminal) and `J11.1`/`J11.2`, with no ORing, no
fuse, and no reverse protection (D1 guards only +12 V). If J11 mates to the Mega's 5 V, the
servo's 1.5 A stall shares the Mega's 5 V node — the coupling `PINOUT.md §7` already flags.
**Fix:** pick one source; mark the other NC.

### C12. ServoNano USB back-feeds the switched rail — INFERRED
`U2.27` (ServoNano 5 V) sits on `5VSWED`, and ServoNano's USB is in active use as a
signal-integrity probe. On CH340 Nano clones USB V_BUS reaches the 5 V rail through a
diode/fuse, so with USB plugged the rail is powered even when U4 is off — silently powering
the servo, lidar and sensor from the Mac's USB port, and putting a 1.5 A stall load on a
500 mA budget. The TPS22919's QOD FET would also be trying to sink that rail through 24 Ω.
**Measure `5VSWED` with U4 off and ServoNano USB attached before leaving the board powered.**

---

## Layout

**The ground is genuinely good — every problem is on the high side.** MEASURED: the bottom
pour is a single connected region of 3426 mm² containing every GND pad; it survives 0.6 mm/side
erosion without splitting. No plane split, no starved region, no forced detour.
J7.2 → J1.2 = **2.34 mΩ** (3.5 mV at 1.5 A). Useful diagnostically: if you measure droop at
the servo, it is almost entirely on the +5 V wire, not the return.

**The top layer has no copper pour at all** (zero regions in the GTL) and only 1.12 m of
trace — so there is abundant free area for every widening/pour fix above.

| # | Finding | Detail |
|---|---|---|
| L1 | **11 cm² high-current loop** | J1 → C2 → U4 → 82.5 mm → J7, closed by the plane return, encloses ≈1115 mm². A 1.5 A pulsed loop that size is both radiator and receiver. The C3 fix (bulk + ceramic *at J7*) collapses it to a few mm². |
| L2 | **`LED−` ‖ `SCL`: 150 µm gap, 27 mm** | The one coupling to fix. LED− is Q1's switched drain running beside the I²C clock. Guard trace stitched every ~5 mm, or move LED− to the empty top layer. |
| L3 | `SIG` ‖ servo-command (`$1N827`): **152 µm, 47.8 mm** | Both slow (50 Hz PWM, 9600 baud) so consequence is low — but these are the two most failure-prone nets in the system at minimum pitch with no guard. A guard is free here. |
| L4 | U4 has no heatsinking copper | 0.198 W in SC70-6, θ_JA ≈ 200–250 °C/W → 40–50 °C rise. Only 2 stitching vias. Pour top-layer 5VSWED and GND regions around it with 4+ vias. |
| L5 | 108 mm of bottom trace slots the plane | SDA/SCL/LED− cross slots 17/14/17×. Second-order, but it clusters under the I²C pair. Move those short hops to the empty top layer. |
| L6 | `+5V` to J11 is 15-mil for 30 mm (45.4 mΩ) | Fine as a low-current tap; widen to 40 mil if J11 feeds a real load. |

### DFM
- **m1.** Four corner NPTH holes sit **0.31 mm** from the routed edge (1.153 mm hole, 0.89 mm
  to edge). Most fabs want ≥0.4–0.5 mm or the tab breaks out. Move 0.3 mm inboard or shrink.
- **m2.** Silkscreen printed over exposed pads — 20 instances, significantly on **`J7_1`**
  (the servo +5 V terminal) and R6/R4/C2/R2/R1. Most fabs auto-clip, but clip it yourself
  with ≥0.15 mm clearance before ordering.
- **m3.** `J7_2` (servo GND return) has only **2** thermal spokes where every other GND pad
  has 4. Not resistively significant (~0.34 mΩ) but a needless constriction on the highest-
  current return. Use 4 spokes or direct-connect.
- **m4.** No fuse, no reverse protection and no TVS on either J1 input. Terminal ordering is
  good (+5V, GND, +12V — GND between), but a mis-wire puts 12 V on the 5 V rail and destroys
  U4 (5.5 V abs max), both Nanos and every sensor. ~$0.30: 2 A polyfuse + SMBJ5.0A.
- **m5.** No board name, revision or date on the silk. With divider values changing between
  revisions, an unmarked board is a real hazard.
- **m6.** J6/J7/J8 are silked `+5V` but the net is `5VSWED` (switched, currently always off).
  Someone probing for 5 V will read 0 V and suspect the supply. Label `+5VSW`.
- **m7.** No test points anywhere — not on `+5V`, `5VSWED`, GND, the servo feed, or either
  divider node. `PINOUT.md §9` is built around exactly these measurements.
- **m8.** C2 (10 mm can) to J1 clearance is ~1.5 mm nominal — clear but tight for a ±0.5 mm
  can tolerance and a hand-placed terminal block. Verify before fab. C2 does not foul either
  Nano module or D1. Both USB connectors face the board edge unobstructed.
- **m9.** No header for the **witness servo**, which `PINOUT.md §3a` documents as teed on the
  same signal and credits with localising the moving-carriage intermittent open. Adding a
  second 3-pin header in parallel with J7 is free and preserves the rig's best diagnostic.

---

## Corrected claims

Two review passes disagreed; these are the resolutions.

- **R6 (0 Ω, 2512) is CORRECT — not a defect.** One pass inferred `U4.5` = CT and called R6 a
  dead-end stub defeating soft-start. The BOM confirms **TPS22919DCKR**, whose pin 5 is
  **QOD** (quick output discharge); tying QOD to VOUT is one of three configurations TI
  explicitly sanctions, selecting the internal 24 Ω discharge path. R6 carries no load current
  and adds **zero** impedance to the servo feed. *The only nit: a 2512 is several sizes larger
  than the function needs and misleadingly implies a power path — use 0603.*
- **U4's `ON` pin does not float.** One pass claimed the rail comes up indeterminate. The
  TPS22919 has an internal 530 kΩ smart pull-down, so it comes up deterministically **off**.
  The fix is a pull-**up** (B1), not a pull-down. *(One residual unknown: if GPIO38's reset
  configuration enables a weak internal pull-up, 3.3 V through ~45 kΩ against 530 kΩ would
  give 3.04 V and the rail would come up on. Unverified — another reason to fit the 100 kΩ.)*
- **A 100 kΩ `ON` pull-up is sufficient.** One pass claimed ≤50 kΩ was needed to overcome the
  smart pull-down. 100 kΩ gives 2.78 V against a 1.0 V threshold — ample.
- **The silkscreen is correct.** It reads "Arduino Nano ESP32" with ESP32-specific labels
  (VBUS, B0, B1), connector functions are labelled, and J1 has polarity arrows. Only the
  **symbol / footprint / netlist / BOM** say "Arduino Nano RP2040" — a procurement and
  future-edit hazard (the RP2040 was retired Aug 2026), not an assembly one. Rename to
  "Arduino Nano ESP32 (ABX00083)" before the file propagates.

---

## Regression scorecard vs. documented history

| Documented failure | Verdict |
|---|---|
| Servo feed resistance / droop | **Reintroduced, worse.** 88–120 mΩ switch + 93.6 mΩ trace = the whole budget (C1), servo last on a daisy chain (C2), no output cap (C3). |
| ESP32-S3 not 5 V tolerant (Mega TX1 → D0) | **Addressed — good catch — but wrong ratio** (B2). Closes the `PINOUT.md §2` audit item. |
| Servo ack divider | **Regressed** from proven 1.8k/3.3k (B2), and R5 breaks fail-loud entirely (C4). |
| Brownout resets (`HAD_POR`) | **Partly fixed** — MCU gets its own 12 V→VIN rail. **Partly reintroduced** — ServoNano on the servo rail (C2), no decoupling (C3), no VIN bulk (C10). |
| RUN-pin glitch (`HAD_RUN`) | **Ignored.** The memory note prescribes ~100 nF RUN→GND; `U1` RESET goes to the socket pin and nothing else. Free fix, not taken. |
| Stepper EMI on servo command line | **Worsened.** 2 kΩ series raising receiver impedance (C8), no J12 ground (C9), 48 mm parallel run (L3), 11 cm² loop (L1). |
| One-way link dropped command | **Not addressed**, and its only safety net is compromised (C4). |
| Y-limit glitch / ground bounce | **Referenced, not fixed.** The known-too-small 1 nF is reproduced verbatim, no series R, signal routed nowhere (B3). |
| Blown-sensor "added light" regime | **Risk added.** No gate pull-down and an unspecified V_GS region put the LED bank's off-state and repeatability in question (C7). |

---

## Needs a bench measurement

1. **SDA idle voltage at `J6.3`** with the lidar attached — is the breakout putting 5 V on A4/A5? (C6)
2. **TCS3200 supply voltage on the *existing* rig** — is `OUT` at 5 V today, i.e. is C6 inherited or introduced?
3. **`5VSWED` with U4 off and ServoNano USB plugged** — does USB back-feed the rail? (C12)
4. **Copper weight ordered.** All resistances assume 1 oz. At 2 oz they halve and C1's total falls to ~0.20 V — still at the ceiling, so C1 stands either way.
5. **What J11's 32 unrouted pins and J10's 8 pins are for.** J10 has zero nets. If J10 lands on a Uno-style block, it may collide with the CNC Shield V3, which is not stackable.
6. **Whether +12 V shares a supply with the steppers** — sets C10's severity.
7. **Actual LED bank current** — decides whether C7's 2N7002 is merely under-driven or outright undersized.
8. After any fix: **volts at J7.1 under a real stall**, using `ServoLoadTest/`, before touching any firmware constant. That is the documented procedure and it is the acceptance test for C1/C2.

---

## Suggested order of work

1. **Netlist-level, must precede fab:** B1 (`ON` pull-up + route 3V3), B2 (R3/R5 → 3.3 k),
   B3 (J9 — route or delete, and add LED+), B4 (route 3V3), C9 (J12 ground).
2. **One layout pass fixes four things at once:** re-place U4 and C2 beside J7, star the
   switched rail, widen `5VSWED` to ≥60 mil or pour it, and add bulk + ceramics at J7 — this
   resolves C1, C2, C3 and L1 together. Strongly consider taking the servo off the switch
   entirely, which also defuses B1's brownout interaction.
3. **Cheap component changes:** C7 (gate pull-down + AO3400A), C8 (R1 → 100–330 Ω),
   C10 (VIN bulk), C5 (I²C pull-ups to 3V3), m4 (polyfuse + TVS).
4. **DFM before ordering:** m1 (corner holes), m2 (clip silk off pads), m3 (J7.2 spokes),
   m5/m6 (board rev, `+5VSW` labels), m7 (test points), m9 (witness-servo header).
5. **Firmware, independent of the respin and worth doing now:** promote
   `servoAckWaitIdle()`'s stuck-low timeout to a hard fault (C4).
6. **Docs:** add D11 to `PINOUT.md §2` and close the level-shifting audit item; correct the
   "fails loud" claim in `PINOUT.md §3b` and `CLAUDE.md` (C4).

---

## Revision status — V3 gerbers + V2 netlist (2026-08-17)

Measured from `Gerber_PCB3_Shield_2026-08-17_V3.zip` and
`Netlist_Schematic3_Shield_2026-08-17_V2.tel`. Path resistances are Dijkstra over a
two-layer segment graph stitched at the vias, with true arc lengths from the I/J offsets
(V3's top layer has 5 arcs contributing +1.73 mm over their chords — no longer negligible).

### Fixed

| Was | Now |
|---|---|
| **C1** servo droop: 198–230 mΩ, 300–345 mV @1.5 A | **~46.7 mΩ, ~70 mV** — J1.3 → J7.1 is 68.0 mm entirely at 30 mil = 44.4 mΩ, plus ~2.3 mΩ of plane return. **Inside the ≤0.2–0.3 V budget with 130–230 mV of headroom for the harness.** |
| **C2** daisy chain, servo last | Resolved. `J7.1` moved from `5VSWED` to **`+5V`**, so the servo no longer passes through U4 or through the sensors' supply traces. `5VSWED` now carries only ServoNano logic + lidar + colour sensor (tens of mA), so the chain ordering stops mattering. |
| **C11** two 5 V sources shorted | Resolved. `+5V` = {`C2.1`, `J1.3`, `J7.1`, `U4.1`} — `J1.3` is now the sole source. |
| U4 → J8.6 sensor rail | 62.6 → 53.8 mΩ (73.4 mm now at 30 mil). Irrelevant at sensor currents, but free. |

Taking the servo off the load switch was the decisive change — it removed the 88–120 mΩ
R_on term that widening alone could never touch, and it also defuses **B1**'s worst
consequence (a brownout can no longer cut servo power mid-flip, because the servo is no
longer behind an MCU-gated switch).

### ⚠ New, and now blocking: the dividers are still 2 kΩ/2 kΩ

`$PACKAGES` is **byte-identical** between netlist V1 and V2 — no component value changed and
no part was added. R1–R5 are all still 2 kΩ.

This was "out of spec" before. **With the active-HIGH ack firmware (committed 2026-08-17) it
is now a hard failure**, because `INPUT_PULLDOWN` (~45 kΩ) sits in parallel with the bottom leg:

| Divider | bottom ‖ 45 kΩ | asserted level | vs V_IH 2.475 V |
|---|---|---|---|
| 1.8 k / 3.3 k (breadboard rig) | 3.075 kΩ | **3.154 V** | OK |
| 2 k / 3.3 k | 3.075 kΩ | **3.029 V** | OK |
| **2 k / 2 k (this PCB)** | 1.915 kΩ | **2.446 V** | **FAILS** |

On this board the ack would never assert, `SERVO_ACK_MODE 2` would retry forever, and
**PARMain would block on the first servo command of the first job.** The current hand-wired
rig is 1.8 k/3.3 k and is unaffected — this bites only if the PCB is fabbed as drawn.

**R3 and R5 must go to 3.3 kΩ before fab.** Same for R2/R4's partner legs on the GRBL link.

### Also new: J11 moved onto the switched rail

`J11.1`/`J11.2` moved from `+5V` to `5VSWED`. If J11 is an **output** (the shield powering
something on the Mega header) that is fine. If it is an **input** — 5 V arriving from the
Mega — it now back-feeds U4's VOUT, defeating the load switch and holding `5VSWED` live
whenever the Mega is powered, regardless of D11. **Confirm the direction before fab.**

### Still outstanding — unchanged in V2/V3

Netlist connectivity is otherwise identical, and no components were added, so every finding
below still stands exactly as written above:

- **B1** `5VSW`/D11 — no `ON` pull-up, no firmware drives D11. The rail is still off, so the
  sensors, lidar and ServoNano still get no power. (The servo now does, via `+5V`.)
- **B3** `LIMSW-` is still a single-pin net; `LIMSW+` still reaches only C1. Still no `LED+`.
- **B4** 3V3 still unrouted.
- **C3** still zero decoupling; `5VSWED` still has no capacitor. Note the inrush ceiling no
  longer applies to the servo, so bulk at J7 can now be sized freely.
- **C5** no I²C pull-ups. **C7** no gate pull-down on Q1, still 2N7002. **C8** R1 still 2 kΩ.
- **C9** J12 still has no ground pin. **C6** sensor rail still 5 V into D8.
- All DFM items (m1–m9) unchanged.

---

## Revision status — netlist V3 + BOM V2 (2026-08-17)

### Fixed
- **B2 dividers** — `R3`, `R5` → **3.3 kΩ** (R1/R2/R4 stay 2 kΩ). Asserted level 3.03–3.11 V
  against V_IH 2.475 V. Connectivity is otherwise byte-identical to netlist V2.

### J11 confirmed — and it makes B1 worse
`J11.1`/`J11.2` on `5VSWED` are an **output**: the shield powers the Mega. That is
intentional and correct as a topology, and it resolves the dual-source question.

But it means **the Mega — and therefore GRBL — is now downstream of the load switch**, which
nothing turns on (B1: `ON` ← D11, undriven, internal 530 kΩ pull-down). As drawn, powering
the board leaves the motion controller dead as well as the sensors. This raises B1 from
"peripherals don't come up" to "nothing except the ESP32 comes up", and makes the
**100 kΩ `ON` pull-up to 3V3 the single most important remaining change**.

Two follow-ons worth checking:
- Load on the switch is now Mega + CNC-shield logic (~300 mA est.) plus ServoNano and
  sensors. Comfortably inside 1.5 A, ~27 mV across R_on — fine.
- Feeding the Mega's **5 V pin** bypasses its USB/barrel power-select. If the Mega's USB is
  also connected (for flashing GRBL), that is two sources on one rail. Normal practice is to
  power the Mega *or* plug its USB, not both — worth a silkscreen note near J11.

### The hand-wired rig is 2 kΩ/2 kΩ — not 1.8 kΩ/3.3 kΩ
Physically confirmed 2026-08-17. `PINOUT.md` and `CLAUDE.md` both claimed 1.8 k/3.3 k; both
have been corrected. This matters more than a documentation slip:

| Divider | Asserted level | vs V_IH 2.475 V |
|---|---|---|
| 2 k/2 k, ideal 5.00 V | 2.500 V | +25 mV |
| 2 k/2 k, real V_OH 4.90 V | 2.450 V | **−25 mV** |
| 2 k/2 k **+ INPUT_PULLDOWN** | **2.446 V** | **−29 mV — hard fail** |

The old active-LOW protocol survived on this ratio because the *idle* level with
`INPUT_PULLUP` was 2.517 V — 42 mV of margin, which is why the 2026-07-27 validation run
(750 commands, 0 misses) passed. The active-HIGH change plus `INPUT_PULLDOWN` would have
inverted that into a permanent fail: the ack never asserts, `SERVO_ACK_MODE 2` retries
forever, and PARMain blocks on the first servo command of the first job.

**Fix applied in firmware, not hardware** (no 3.3 kΩ/1.8 kΩ parts on hand, and the rig and
the PCB must run one build): the ack is now read as an **analog level**, `servoAckHigh()`,
threshold **1.20 V**. Idle is 0 V (the divider's bottom leg is the pulldown), asserted is
≥2.45 V, so the decision sits >1.2 V from either state and no longer depends on V_IH or on
the divider ratio. Works unchanged on 2 k/2 k, 2 k/3.3 k and 1.8 k/3.3 k. `SERVO_ACK_PIN` =
D2 = GPIO5 = **ADC1**_CH4 — ADC1 is mandatory, ADC2 is unusable while WiFi is up.

Do not revert to `digitalRead`, and do not fit `INPUT_PULLDOWN`, while the rig is 2 k/2 k.

---

## B1 resolved by design decision, not by a pull-up (2026-08-17)

The earlier recommendation — a 100 kΩ pull-up on `ON` so the rail is on by default — is
**withdrawn**. It was driven by the brownout-cuts-servo-power argument, and V3 made that
obsolete by moving `J7.1` to unswitched `+5V`. With the servo off the switched rail, a
pull-up would only have inverted the default while giving up the one thing the switch is
good for.

**Adopted instead: `PARMain` drives D11 HIGH as the first statement in `setup()`.** The rail
stays fail-off, and the board gets a deterministic power-on order — ESP32 first, then Mega,
ServoNano, lidar and colour sensor. No component change.

Implementation notes that are easy to get wrong:
- D11 must go HIGH **before** the boot servo-park, which is a UART write to a board on the
  switched rail. `SHIELD_PWR_SETTLE_MS` = 2500 ms covers the ServoNano's CH340 bootloader.
- Every sketch used on the shield needs the same two lines; one that lacks them looks like a
  dead board rather than a firmware omission.
- The rail is all-or-nothing — it cannot power-cycle the lidar (REVIEW.md C5's flaky init)
  without also resetting GRBL and the ServoNano, because J11 put the Mega on it.
- On `esp_restart()` the pin tri-states, so the stall watchdog now power-cycles GRBL too.
  That is a stronger recovery than the soft reset it replaces.

---

# Blind re-audit (2026-08-17) — two independent passes, adjudicated

Two auditors reviewed netlist V3 + BOM V2 + V3 Gerbers **with no access to this document,
to any earlier revision, or to the firmware**, to test whether the findings above were
anchored. Both worked only from the design files plus a neutral description of the machine.

**Independently reproduced by both:** TCS3200 `OUT` into D8, the dead `LIMSW` nets,
unbuffered 3.3 V→5 V in both directions, the 2N7002, no gate pull-down, no I²C pull-ups,
no decoupling, no 5 V protection, `LED-`‖`SCL` coupling, J12 without a ground pin, the
0.312 mm hole-to-edge, silk on pads, J10 unconnected, 3V3 unrouted. Both also independently
cleared R6 as a sanctioned QOD configuration and rated the ground pour excellent.

**Measurement cross-check.** Three methods — a 1.4 M-node sparse resistive solve, a
finite-element mesh, and Dijkstra over the vector segments — on `J1.3 → J7.1`:
**43.86 / 44.3 / 44.4 mΩ**. On the `LED-` return: 137.73 / 136.7 mΩ. Both auditors also
flood-filled copper against the netlist: **zero opens, zero shorts**.

## Adjudicated disagreements

Each resolved against the primary source, not by preferring an auditor.

### 1. TCS3200 V_IH — auditor A correct, B wrong
B could not extract the datasheet and speculated V_IH might be 0.7 × VDD = 3.5 V, which
would put 3.3 V drive on S0–S3 out of spec. A found the real figure.

**ams TCS3200 DS000107 v1-01, p.6, Recommended Operating Conditions:**
`VIH High-level input voltage, VDD = 2.7 V to 5.5 V: min 2, max VDD` — an **absolute 2.0 V**,
not a fraction of VDD. `VIL: 0 to 0.8 V`.

**3.3 V drive on S0–S3 is fully in spec even at VDD = 5 V.** B's HIGH-6 is withdrawn.
(Corroborated independently by the rig having run this way for months.)

### 2. TCS3200 V_OH — auditor A correct, B overstated
B assumed a rail-to-rail ~4.9 V output. **Datasheet p.6: `VOH, IOH = -2 mA: min 4, typ 4.5 V`.**

The overstress on D8 is therefore **0.4–0.9 V above the 3.6 V absolute maximum**, not 1.3 V.
Still a clear absolute-maximum violation and still critical — but quote the right number.

### 3. TPS22919 ON pin floating — auditor B correct, A wrong
A argued the internal Smart Pull-Down holds the rail off during an ESP32 reset and called
this good design. B said the pull-down disconnects once ON has been driven high.

**TI TPS22919 SLVSEN5B §8.3:** *"When power is first applied to VIN, a Smart Pull Down is
used to keep the ON pin from floating until the system sequencing is complete. **Once the ON
pin is deliberately driven high (≥VIH), the Smart Pull Down is disconnected** to prevent
unnecessary power loss."* Pin table, pin 3: *"Active high switch control input. **Do not
leave floating.**"*

So A is right about first power-on and wrong about everything after it. Since `PARMain` now
drives D11 high on every boot, the 530 kΩ is disconnected from that moment on, and **every
subsequent ESP32 reset leaves ON genuinely floating.** Fit the **100 kΩ pull-down on `5VSW`**.
Also confirmed from the same table: `VIH ON Pin High Voltage Range 1 – 5.5 V`, so 3.3 V drive
is fine.

### 4. R6 / QOD + USB back-feed — auditor A correct, and confirmed
Everyone (including this document) cleared R6 as datasheet-sanctioned and stopped there. A
accepted that and asked what it *does*. Confirmed from **SLVSEN5B Table 2**: with ON low and
QOD tied to VOUT, VOUT is pulled to GND through `RPD,QOD` = **24 Ω** (p.5).

`5VSWED` also reaches the Mega's 5 V pins (J11.1/2) and the ServoNano's 5 V pin (J5.12), both
of which back-feed their USB 5 V onto that rail. With the switch off and either USB plugged:

> 5 V / 24 Ω = **208 mA continuous, 1.04 W in an SC-70-6**.
> RθJA = **210.7 °C/W** (p.4) → a **219 °C rise**, far past the 180 °C thermal shutdown.

This is live in an ordinary scenario — flashing GRBL over the Mega's USB with the shield
unpowered. **Fix: R6 → ~1 kΩ** (caps the sink at ~5 mA; the datasheet's own worked example
uses R_QOD = 1 kΩ), or depopulate it — leaving QOD floating is the documented third option.

A correct configuration made destructive by an interaction with a second finding. Neither the
four-pass review nor auditor B caught it.

### 5. J1 silkscreen glyphs — auditor A correct, B wrong (and this document was wrong)
B reported "two custom silk arrows... each pointing into a gap" and rated it a mis-wire
hazard. Extracted from `Gerber_TopSilkscreenLayer.GTO`, both glyphs are **closed rectangles**:
four 0.254 mm strokes bounding 27.23–28.23 × −1.20…−2.70 mm and 32.23–33.23 × −1.20…−2.70 mm.
They are **1.0 × 1.5 mm boxes, not arrows**, and almost certainly J1's body outline (the
KF301 has divider ribs between poles). The three text labels are correctly aligned over their
pads (25.23 / 30.23 / 35.23), which both auditors independently verified.

**No mis-wire hazard from the legend.** The real J1 risk is the absence of reverse-polarity
and overvoltage protection on `+5V`, which both auditors flagged and which stands.

### 6. Drill and via counts — auditor B correct, A double-counted
Measured: `Drill_PTH_Through.DRL` holds **158** holes (24 of them Ø0.305 mm vias);
`Drill_PTH_Through_Via.DRL` holds **24**, and **all 24 coordinates are identical** to ones
already in the PTH file. `Drill_NPTH_Through.DRL` holds 8.

**166 unique holes: 158 plated (24 vias) + 8 non-plated.** A's "182 PTH / 48 vias" summed the
two files without deduplicating; B spotted the standard EasyEDA duplication.

### 7. Minimum clearance — B's figure preferred
A measured 0.141 mm, B 0.152 mm. A stated its own raster quantisation as ±0.025 mm, and
0.152 mm (6 mil) is the EasyEDA default rule. Take **0.152 mm**; either way it passes.

### 8. Ground return resistance — unresolved, immaterial
A's sparse solve gives J7.2→J1.2 = 3.375 mΩ; B's mesh gives 2.47 mΩ; the earlier FD solve
gave 2.34 mΩ. The spread is meshing/boundary-condition choice. All three agree the pour is
excellent and contributes ≤5 mV at 1.5 A. No action.

### 9. Board dimensions — not a real conflict
Outline measured from `Gerber_BoardOutlineLayer.GKO`: **90.68 × 51.82 mm** overall. A quoted
the total; B quoted the 90.68 × 46.87 mm main body plus the 3.30 × 5.08 mm J12 tab separately.

## New findings from the blind pass, not in the review above

| # | Finding | Severity |
|---|---|---|
| N1 | **R6/QOD + USB back-feed burns ~1 W in U4** (adjudication 4) | **Critical** |
| N2 | **Split rails have no supervisor.** The ESP32 runs from 12 V behind its own buck, everything else from 5 V. A 5 V brownout — a servo stall — resets the ServoNano and the Mega **while the ESP32 keeps running**, streaming G-code and servo commands into peripherals that are mid-reset. No reset distribution, no brownout signal between domains. | Major |
| N3 | **Servo is energised before its controller.** J7.1 is on unswitched `+5V`; the ServoNano generating `SIG` is on switched `5VSWED`. Between power-on and D11 asserting, the servo has power and an 82 mm floating signal line. Fix: 10 kΩ pull-down on `SIG`. | Minor |
| N4 | **J8 does not bring out the TCS3200's `/OE`.** If the breakout does not tie it low, `OUT` is high-Z and the sensor reads nothing. | Medium |
| N5 | **Near-zero solder-mask expansion** on the 15-pin header pads: copper Ø1.7 mm, mask Ø1.7016 mm — 0.8 µm/side. With ±0.05 mm registration the mask encroaches. Set 0.05 mm expansion or confirm the fab overrides it. | Minor |
| N6 | **`LED-` return is 137.7 mΩ** — Q1 sits 34.9 mm from J9 on 10-mil trace. At 500 mA that is 69 mV in series with the FET's ill-defined R_DS(on), directly modulating LED brightness. Widen and move Q1 to J9. | Minor |
| N7 | **`LED-`‖`SCL` is 53 mm, not 27 mm**, at a 6 mil gap — with an estimated ~0.92 V injected glitch against an I²C V_IL max of 0.99 V. **Second-order and important: today's weakly-driven 2N7002 has slow edges, which is the only thing keeping this benign. Upgrading Q1 without also rerouting `LED-` trades an LED-repeatability problem for I²C corruption.** Add a 100–470 Ω gate resistor to keep the edge deliberately slow. | Major |
| N8 | **J12's mapping to a Mega hardware UART is unproven** from the files. If pins 3/4 do not land on a Mega serial port, the ESP32↔GRBL link does not exist. Highest-priority bench check. | Verify |
| N9 | `J2.12` on a Nano ESP32 is **VBUS**, not 5 V — an output live only under USB power. It is unconnected here, which is correct; never feed 5 V into it. | Nit |

## Consolidated must-fix before fabrication

Confirmed by two or three independent passes:

1. **TCS3200 → 3.3 V** (`J8.6` to the unused `J2.2`), or divide `OUT` 2 k/3.3 k. Re-derive
   `SCAN_ON_BLUE_MAX`, since output frequency scales with VDD.
2. **`R6` → 1 kΩ** — prevents 1 W in an SC-70 whenever a debug USB is plugged in.
3. **100 kΩ pull-down on `5VSW`** — the Smart Pull-Down is gone after the first assertion.
4. **Route or delete the limit-switch nets**; add `LED+`.
5. **100 kΩ gate pull-down + 100–470 Ω gate resistor on Q1**, and swap to AO3400A — but
   reroute `LED-` away from `SCL` in the same spin (N7).
6. **I²C pull-ups, 2.2 kΩ to 3V3** — requires routing 3V3.
7. **Decoupling**: 100 nF at U4 in/out, 10 µF on `5VSWED`, 100 nF at J6/J8, and bulk at J7.
8. **Protection on `+5V`**: polyfuse + P-FET or TVS.

Deferred by decision, not oversight: the 3.3 V→5 V links (M2) remain unbuffered. They are
inherited from the working hand-wired rig, not introduced by this board — but both auditors
rate them higher than this document originally did, and they sit on the servo-command path
with a documented dropped-command failure history. A 74LVC2T45 or 74AHCT1G125 is the fix if
that failure recurs.
