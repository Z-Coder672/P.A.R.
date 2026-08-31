#include <WiFi.h>
#include <WiFiClientSecure.h>  // ESP32: TLS client is a separate lib, not part of WiFi.h
#include <ArduinoHttpClient.h>
#include <base64.hpp>
#include <esp_system.h>      // esp_restart()
#include <time.h>            // NTP wall clock for the daily cadence
#include "esp_sntp.h"        // sync-notification callback — clockValid() anchor
#include <Wire.h>            // I2C for the VL53L4CD (A4/A5)
#include <VL53L4CD.h>        // Pololu lib — daily lidar standoff scan
#include <LittleFS.h>        // cadence record; shares plog's "ffat" mount
#include "env.h"
#include "persistent_log.h"  // flash-backed log for WiFi/HTTP/poll events only
#include "par_root_ca.h"     // CA bundle for the TLS calls to SERVER

const char* SSID = "ab guest";
const char* PASSWORD = WIFI_PASSWORD;
const char* SERVER = "par.zimmzimm.com";
const int PORT = 443;

WiFiClientSecure wifi;
HttpClient client(wifi, SERVER, PORT);

// The site sits behind Cloudflare, which rotates Universal SSL between Google
// Trust Services, Let's Encrypt and SSL.com WITHOUT NOTICE, so every TLS client
// in this sketch is pinned to the full 6-root bundle in par_root_ca.h rather
// than a single issuer. Do NOT trim it down to whichever root is live today —
// the next rotation would take the rig offline. (On WiFiNINA the NINA firmware
// carried its own root store and this was implicit; ESP32 mbedTLS has none, so
// the roots must be supplied per client or the handshake fails.)
//
// This is a MACRO, not an inline function, on purpose: the Arduino IDE injects
// its auto-generated forward declarations immediately above the FIRST function
// in the file, and a helper defined up here would push that insertion point
// above the TcsFilter enum below — breaking tcsSelect(TcsFilter)'s prototype.
#define parSecure(c) ((c).setCACert(PAR_ROOT_CA))

const int GRID_W = 37;
const int GRID_H = 18;

// Daily lidar standoff scan, as persisted to flash. Defined UP HERE, far from
// the cadence code that uses it, for the same reason TcsFilter is: the Arduino
// IDE injects its auto-generated forward declarations immediately above the
// FIRST function in the file, so any type named in a function signature must
// already be complete by that point. Declared next to the cadence helpers it
// would only be visible ~1500 lines too late, and cadenceChecksum(const
// LidarScanRecord&) would fail to compile in the generated prototype block.
//
// Fixed layout, written and read as raw bytes by the same firmware — there is
// no cross-compiler portability requirement, only self-consistency, which the
// magic/version/geometry/checksum fields enforce. The stored (year, yday) is
// the authority for "has today's scan already run".
// Declared up here for the same reason as LidarScanRecord below: the Arduino
// preprocessor hoists auto-generated prototypes to the top of the file, so any
// type used in a function signature must already be visible.
struct ClearCutRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t pad;
  int32_t  cut;
  uint32_t checksum;
};

struct LidarScanRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t gridW;
  uint16_t gridH;
  uint16_t sensorOk;                    // 0 = ranger never came up; cells are 0
  int32_t  year;                        // tm_year (years since 1900), local
  int32_t  yday;                        // tm_yday (0..365), local
  uint32_t epoch;                       // unix seconds at completion
  uint32_t cellsOk;                     // cells that returned >=1 sample
  // Mid-sweep checkpoint, written at every row boundary. DISTINCT from
  // (year, yday) above, which still mean "a full sweep finished" and are still
  // stamped only at the end -- so a partial record can never read as done.
  uint16_t rowsDone;                    // visit-rows completed, 0..GRID_H
  uint16_t pad2;
  int32_t  progYear;                    // day the partial sweep belongs to
  int32_t  progYday;
  uint16_t dist10[GRID_H * GRID_W];     // trimmed mean, tenths of a mm; 0 = none
  uint32_t checksum;                    // over every byte before this field
};

// CNC homes to full negatives, so the work area lives in negative coordinates.
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131, so this anchors the grid

// TCS3200 color sensor: S0-S3 + OUT on D4..D8, plus an LED illumination bank on
// D10 (via NPN, HIGH = LEDs on) for ambient-subtracted reads.
// Defined before any function so the Arduino IDE's auto-prototype generator
// (which injects forward declarations just above the first function) sees
// TcsFilter as a valid type when it forward-declares tcsSelect(TcsFilter).
// Nano ESP32 header-pin macros (D4=GPIO7, D5=8, D6=9, D7=10, D8=17, D10=21).
// The physical wiring is UNCHANGED from the RP2040 build — the ESP32-S3 GPIO
// matrix just maps different silicon pins to the same header positions, so use
// the Dn names rather than raw numbers.
const int TCS_S0 = D4;
const int TCS_S1 = D5;
const int TCS_S2 = D6;
const int TCS_S3 = D7;
const int TCS_OUT = D8;
const int TCS_LED = D10;  // illumination bank (NPN base); HIGH = on

// S2/S3 select the photodiode filter bank.
//
// !! The names below are the plain datasheet table. Whether they are also
// !! PHYSICALLY right on this rig is UNRESOLVED and does not matter: what the
// !! code depends on is which commanded (S2,S3) pair discriminates the disc
// !! faces, and that has been measured directly. It is (S2=H, S3=L) -- value 2,
// !! named TCS_CLEAR here, landing in tcsReadRGBC's `c` output slot. That is
// !! the slot classifyDisc thresholds. See the comment on classifyDisc for the
// !! ground-truth measurements.
// !!
// !! Do NOT re-derive this mapping from a sum test (an unfiltered channel must
// !! be ~ the sum of the filtered ones). That identity does not hold on this
// !! sensor at either supply voltage -- at 3V3 the `g` slot reads ~5x the `c`
// !! slot, which is physically impossible for a filtered vs unfiltered pair --
// !! so the test proves nothing in either direction. It is what produced the
// !! "S2/S3 are crossed" theory, and then the 2026-08-20 reversal of it, and
// !! both of those moved which channel was thresholded while claiming not to.
// !! Measure against a known bitmap instead; the check pass gives you one free.
enum TcsFilter {
  TCS_RED = 0,    // S2=L,S3=L
  TCS_BLUE = 1,   // S2=L,S3=H -- populations OVERLAP, unusable for classifying
  TCS_CLEAR = 2,  // S2=H,S3=L <- the channel classifyDisc reads (slot `c`)
  TCS_GREEN = 3   // S2=H,S3=H -- separates as well as value 2 (~9x), unused
};

// ---- Shield peripheral power (D11 -> TPS22919 ON) --------------------------
// The shield PCB gates a switched 5V rail (5VSWED) behind a load switch whose
// ON pin is this GPIO. That rail feeds the Mega/GRBL, the ServoNano, the lidar
// and the TCS3200 -- everything except this MCU and the servo motor itself
// (which sits on unswitched +5V, deliberately: see PCBnSCHs/REVIEW.md C1).
//
// The part has an internal 530k pull-down, so the rail is OFF until this pin is
// driven HIGH. That is the intended behaviour, not a defect: it gives a
// deterministic power-on order -- this MCU boots first, then brings everything
// else up -- rather than every board racing at power-on.
//
// !! CONSEQUENCES, all of which are load-bearing:
// !! 1. This must be driven HIGH before ANY peripheral is addressed. The boot
// !!    servo-park below is sent over a UART to a board that is unpowered until
// !!    this happens, so the park would be silently lost.
// !! 2. SHIELD_PWR_SETTLE_MS must cover the ServoNano's bootloader (~2 s on a
// !!    CH340 Nano) before the park command is sent, not just the rail's rise.
// !! 3. Every sketch run ON THE SHIELD needs these two lines. A sketch without
// !!    them presents as a completely dead board -- no GRBL, no sensors, no
// !!    servo response -- which looks like a wiring fault. See PINOUT.md S10.
// !! 4. The rail is all-or-nothing. It cannot power-cycle the lidar for
// !!    recovery without also resetting GRBL and the ServoNano.
// !! 5. On esp_restart() (the 60 s stall watchdog, grblAlarmRecover's fallback)
// !!    this pin tri-states and the whole rail drops -- so a watchdog reset now
// !!    power-cycles GRBL too. That is a STRONGER recovery than the soft reset
// !!    it replaces, and PARMain re-homes on boot anyway.
// !! On a hand-wired rig with no shield, D11 is unconnected and this is a no-op.
const int SHIELD_PWR_PIN = D11;
const unsigned long SHIELD_PWR_SETTLE_MS = 2500;

const int SERVO_PIN = D9;
// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°): REST≈2° (parked),
// RELEASE≈25.5° (pushes the half-rotated squisk during the X slide-back),
// ENGAGE=75° (initial 90° squisk rotation).
//
// !! REGIME CHANGE (ported from FlipAllTest, 2026-08-09). RELEASE/ENGAGE used to
// !! be 1018 (~46°) / 1471 (~90°) here, with no lidar compensation and no reach
// !! trim. The pair below is the FlipAllTest regime and is only valid AS A SET
// !! with compensatedUs() applied on top (RELEASE_EXTRA_REACH_MM, the per-cell
// !! LIDAR_FIT_CMM correction, the edge-row trim) and with the flip-target trims
// !! (FLIP_TARGET_OFFSET_X, FLIP_NONINVERT_OFFSET_X, FLIP_SKEW_X_TOP -1.5,
// !! FLIP_CATCH_EXTRA_X, the unload). Porting the base angles without the trims,
// !! or the trims without the base angles, leaves the flip wrong by tens of
// !! degrees — which is exactly how the arm gets driven into the board.
const int SERVO_US_REST = 565;
// RELEASE is a BASE angle that lidar compensation then shifts per cell; it is
// never commanded raw. Uncompensated it is 25.51°. Through compensatedUs() the
// board spans 793..921µs (24.15°..36.56°), mean 853µs — so nothing clamps at
// SERVO_US_MIN and the reference cell (0,17) lands at 795µs (802µs before
// RELEASE_EDGE_ROW_TRIM_MM was added; FlipAllTest's comment still cites 802).
const int SERVO_US_RELEASE = 807;
// ENGAGE is deliberately NOT lidar-compensated — the 90° stage-1 rotation only
// has to clear the disc, so it is a fixed 75° (1317µs = 74.97°) everywhere.
const int SERVO_US_ENGAGE = 1317;  // 75°, uncompensated
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
// Lowered arm angles below RELEASE. RELEASE2 (~10° below) is the second-catch
// error-reduction pass, and stays DERIVED from RELEASE because it is part of the
// flip tuning — it must track the release regime. Pulse mapping is 544–2400µs
// over 0–180° (~10.3µs/°), so 10° ≈ 103µs and 12.5° ≈ 129µs.
const int SERVO_US_10_DEG = 103;
const int SERVO_US_12_5_DEG = 129;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
// SCAN is the scan-sweep arm angle, where the dropped arm brushes the board to
// push half-rotated stage-1 squisks through. It is NOT part of the flip tuning.
//
// It used to be written `SERVO_US_RELEASE - SERVO_US_12_5_DEG`, which at the old
// RELEASE=1018 evaluated to 889µs (~33.5°) — the angle the scan sweep was
// actually tuned at. Keeping that expression through the RELEASE 1018→807
// regime change would have silently dropped it to 678µs (~13°), raising the arm
// ~20° and changing scanning behaviour as a side effect of a FLIP change. So it
// is PINNED to its old absolute value. If the sweep ever needs retuning, retune
// this literal directly; do not re-derive it from RELEASE.
// CHANGED 2026-08-10: the scan now runs with the arm PARKED AT REST.
// It was 889µs (~33.5°), a deliberately dropped arm that brushed the board so
// half-rotated stage-1 squisks got nudged through as scanGrid swept past. That
// secondary settling job is now abandoned: the arm stays up for the whole scan.
// Consequences, both intended:
//   * the "carriage moves only with the servo at REST" invariant now holds
//     trivially for the entire scan, not just its entry and the rehome — the
//     dropped arm dragged across a populated board is what snapped the flip arm.
//   * half-rotated discs are NO LONGER nudged during scanning, so a disc left
//     mid-rotation stays mid-rotation until the check pass re-flips it. If
//     ambiguous reads start showing up on cells that were mid-rotation, this is
//     why — restore a value above REST rather than re-deriving from RELEASE.
// Kept as its own constant (rather than replacing the call sites) so the BOOT
// line still reports the scan angle a run actually used.
const int SERVO_US_SCAN = SERVO_US_REST;   // 565µs, arm parked
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;
// Extra travel on the CATCH stroke — the one that runs with the arm down at
// RELEASE, sweeping back across the cell to drive the half-rotated squisk
// through its final 90 deg. The clearing slide (which runs with the arm at REST)
// is UNCHANGED at FLIP_OFFSET_X; only the return is lengthened, so the arm
// sweeps this far PAST the cell origin instead of stopping on it.
//
// Set on the rig to complete the rotation more consistently. Each cell
// re-establishes absolute X with moveTo(fx) at the top of flipDisc, so the extra
// travel cannot accumulate across cells. It IS clamped against both soft limits
// in flipDisc, same as dx.
const float FLIP_CATCH_EXTRA_X = 3.5f;

// Inverted flip — applied on LEFT-TO-RIGHT sweep rows. The clearing slide runs
// −X and the catch/return slide runs +X, the opposite of the original flip.
// Two reasons:
//   * The catch pushes the squisk through its final 90° in the opposite
//     rotational sense, so LTR rows unwind the twist the RTL rows wind into the
//     column rods instead of both directions adding to it.
//   * The return slide — the last motion of the flip — now ends in the
//     direction the sweep is already heading, so an LTR row no longer pays
//     FLIP_OFFSET_X of dead backtrack before every next cell. RTL rows keep the
//     original flip for exactly the same reason: their −X return already points
//     along the sweep.
// Approaching the disc from the other side puts the arm on the opposite face
// of the squisk, so the flip X target shifts right by this much to keep the
// contact geometry identical. Applies only to inverted (LTR) rows.
const float FLIP_INVERT_OFFSET_X = 11.0f;

// Direction-dependent X trim for the NON-inverted (RTL) rows. The bottom row is
// swept with ltr=true, i.e. INVERTED, and its landing positions were confirmed
// correct on the rig — so the inverted direction is the reference and this shifts
// only the other one. Negative = left / toward -X / toward the homing corner.
// Set -0.5 mm from a full-board run: RTL rows landed slightly right of the LTR
// rows' contact point. This is separate from FLIP_INVERT_OFFSET_X, which
// compensates for approaching the squisk from the opposite face; this one is a
// residual registration difference between the two sweep directions.
const float FLIP_NONINVERT_OFFSET_X = -0.5f;

// Global flip-target trim. Shifts the absolute X the flip head drives to for
// EVERY cell, +X = right = away from the homing corner. Independent of
// FLIP_OFFSET_X (the relative clear/catch stroke) and of FLIP_INVERT_OFFSET_X
// (which applies to inverted rows only) — this one moves the landing position
// itself. Kept in step with FLIP_SKEW_X_TOP below: the pair was tuned together.
const float FLIP_TARGET_OFFSET_X = 2.0f;

// Column-lean skew. The disc columns are not perfectly square to the flip head:
// the higher up the board a cell sits, the further LEFT (-X, toward homing) its
// true flip position is. Modelled as a linear X shift in physical height,
// ANCHORED AT THE BOTTOM ROW: 0 at gy = GRID_H-1, FLIP_SKEW_X_TOP at gy = 0.
// (bitmap y=0 is the TOP row, and physical Y increases upward.) Applied only to
// the flip head — scanning still targets cell centers.
//
// Set from a column-1 run: with FLIP_TARGET_OFFSET_X alone the top landed
// correctly and the bottom needed 1.5 mm more to the right. That is expressed
// here as "+2.0 mm everywhere, then 1.5 mm back off at the top" — hence
// FLIP_TARGET_OFFSET_X 2.0 and this term at -1.5. Net: the top row sits at
// +0.5 mm, the bottom row at +2.0 mm. (Was -2.0 here before the FlipAllTest
// merge, in the old 1018µs release regime with no target offset at all.)
const float FLIP_SKEW_X_TOP = -1.5f;  // X shift (mm) at the top row; −X = toward homing

// Linear skew for grid row gy (bitmap y; 0 = top = highest physical Y). Returns
// the X offset to add to the flip head's target. Bottom row → 0, top row →
// FLIP_SKEW_X_TOP, interpolated by physical height.
static inline float flipSkewX(int gy) {
  float heightFrac = (float)((GRID_H - 1) - gy) / (float)(GRID_H - 1);
  return FLIP_SKEW_X_TOP * heightFrac;
}

// Arm UNLOAD. While the arm is still at ENGAGE it is bearing against the squisk
// it just rotated, so the head backs off this much in the direction OPPOSITE the
// upcoming release stroke BEFORE the servo returns to REST. That takes the
// contact force off the arm so it lifts away cleanly instead of dragging against
// a loaded disc. The release stroke then travels that much further, so net
// motion from the cell origin is unchanged.
// Set to 0.0f to disable entirely (unload move skipped, compensation term
// vanishes) and restore the original motion exactly.
const float FLIP_UNLOAD_X = 1.5f;
// Enlarged unload for a cell whose unload actually travels OFF the board.
//
// Stated the way the rig is operated (rows counted 0-based FROM THE BOTTOM):
//     even row -> 4 mm only on the RIGHTMOST column
//     odd  row -> 4 mm only on the LEFTMOST  column
// The bottom row is bitmap y = GRID_H-1, so an even bottom-up row is an ODD
// bitmap y, which is exactly an LTR (inverted) row. The test below is written on
// `inverted` because that is equivalent AND independent of which end you count
// rows from -- get the parity convention wrong and you invert the whole rule.
//
// The unload always runs OPPOSITE the clearing slide dx, so its direction is set
// by the row's sweep direction, not by the column:
//     LTR (inverted): dx = -FLIP_OFFSET_X  ->  unload moves +X (right)
//     RTL           : dx = +FLIP_OFFSET_X  ->  unload moves -X (left)
// and the free space lies beyond the outer columns:
//     column 0        sits at X ~ -752.7, ~25 mm of clearance further LEFT
//     column GRID_W-1 sits at X ~  -31.1, ~31 mm of clearance further RIGHT
// so the bigger backoff is only safe where those two agree:
//     RTL row + column 0        -> backs left, off the board   OK
//     LTR row + column GRID_W-1 -> backs right, off the board  OK
// Any other pairing backs INTO the neighbouring column (col 35 or col 1) and
// would drive the arm 4 mm into a populated cell — which is why this is keyed on
// `inverted` and not on the column alone. Do NOT "restore the symmetry" by
// applying it to both outer columns unconditionally.
//
// History: this was originally both outer columns unconditionally, then briefly
// the right column only. Both applied the 4 mm on rows that back inward.
const float FLIP_UNLOAD_X_EDGE = 4.0f;
static inline bool usesEdgeUnload(int gx, bool inverted) {
  return inverted ? (gx == GRID_W - 1) : (gx == 0);
}

// ---------------------------------------------------------------- lidar standoff
// Per-cell servo compensation from the VL53L4CD scan.
//   0 = OFF everywhere            (uncompensated release angle)
//   1 = ON everywhere             (production setting — the whole board)
// Mode 2 (randomized half-board paired trial) lives only in FlipAllMaskedTest,
// which carries the COMP_MASK table; PARMain deliberately has no mask.
#define LIDAR_COMP_MODE 1
#if LIDAR_COMP_MODE == 2
#error "LIDAR_COMP_MODE 2 needs COMP_MASK/cellCompensated — that lives in FlipAllMaskedTest, not PARMain"
#endif

// Flip-arm geometry. The arm is a lever pivoting against the disc platform; at
// ARM_FLAT_DEG it lies parallel to the platform, so its perpendicular reach is
//     reach(theta) = ARM_LEN_MM * sin(theta - ARM_FLAT_DEG)
// A cell whose standoff exceeds LIDAR_REF_MM by d needs d more reach, so the
// compensated angle solves
//     ARM_LEN_MM*sin(theta' - FLAT) = ARM_LEN_MM*sin(theta - FLAT) + d
// -> theta' = FLAT + asin( sin(theta - FLAT) + d/ARM_LEN_MM )
// ARM_LEN_MM is the ARM's length. Do NOT "fix" it to account for the pusher pin
// being shortened — the pin trim is a linear reach demand and belongs in
// RELEASE_EXTRA_REACH_MM below, not in the lever arm of the sine model.
const float ARM_LEN_MM   = 25.0f;
const float ARM_FLAT_DEG = 23.0f;
// GLOBAL BOARD TRIM. Extra linear reach demanded of the pusher pin's travel,
// applied to every cell, OUTSIDE every LIDAR_COMP_MODE gate. Two things set it,
// and only the first is geometric:
//
//   1. Pin length. The pin was physically shortened, so its tip sits further
//      back for the same servo angle and every release has to travel further
//      out to make contact.
//
//   2. ROD COMPLIANCE, which is what actually caps it. The discs hang on
//      vertical rods, and during the release stroke the rod flexes out of the
//      way. How much it flexes depends on where along the rod the cell sits:
//      mid-span rods give way easily, but THE BOTTOM ROW IS CLOSEST TO THE
//      MOUNTING POINT OF ANY ROW, so its rods are stiffest and barely deflect.
//      Reach that is merely generous mid-board becomes an over-push down there —
//      the bottom row was rotating to roughly 270 deg instead of 180 at 2.0 mm,
//      even though the lidar table commands it the SMALLEST angle on the whole
//      board (837 us vs 893 at the top). So this is NOT a standoff problem and
//      no change to LIDAR_FIT_CMM fixes it; do not go looking there again.
// 2026-08-20: 2.0 -> 4.496. See the LIDAR_FIT_CMM header for the re-tilt this
// pairs with. The re-tilt is mean-preserving, so on its own it would have taken
// up to 60 us AWAY from the top rows, which currently flip correctly. This term
// puts that back: +60 us on the board mean, chosen so row 0 lands within 1 us
// of what it commands today. Net effect is "hold the top, give the bottom what
// the lidar says it needs" -- row 17 goes 825 -> 946 us, row 0 stays at 881.
//
// !! This DOES push the bottom rows past the level the note above records as
// !! over-rotating (~270 deg at 837 us). Those two observations cannot both
// !! describe the same board, and the four 2026-08-17..20 lidar scans are the
// !! newer evidence, but WATCH THE BOTTOM TWO ROWS on the first full run.
const float RELEASE_EXTRA_REACH_MM = 4.496f;
// Reach trim applied ONLY to the two extreme rows (top and bottom). Both sit
// nearest their rod mounts, where the rod is stiffest and deflects least, so they
// need slightly less reach than the interior to avoid over-pushing. Rows 1..16
// are untouched. Worth 0.5 mm ~= -12 us of commanded RELEASE on those two rows.
const float RELEASE_EDGE_ROW_TRIM_MM = -0.5f;

static inline float edgeRowReachTrim(int gy) {
  return (gy == 0 || gy == GRID_H - 1) ? RELEASE_EDGE_ROW_TRIM_MM : 0.0f;
}
// Reference standoff: a cell at exactly this distance gets no correction. Set to
// the board mean so the correction is purely cell-to-cell rather than a
// whole-board push.
//
// PASS 4 (2026-08-03) moved this from 40.50 to 37.69 — the first scan taken on
// the Arduino Nano ESP32 after the RP2040 was retired. The absolute frame
// shifted ~2.8 mm because the board was reseated during the MCU swap; that
// shift is harmless, since compensatedUs() only ever uses (cell - REF) and both
// moved together. The DELTA span is essentially unchanged: 5.54 mm here vs
// 5.39 mm on pass 3.
//
// Pass 4 drifted +3.40 mm over its 71.5 min (pass 3: +1.6 mm) — larger than the
// board shape being measured, so the whole table is drift-corrected against the
// cyan calibration square interpolated in time. Skipping that step would have
// baked the sensor's own thermal ramp in as a fake top-to-bottom tilt.
//
// !! The 12 cells the scan tagged back=black are CLASSIFIER FALSE POSITIVES, not
// !! black backs — the board was physically uniform. Fitting each column on the
// !! remaining cells and measuring those 12 against it gives mean -0.00 mm,
// !! sd 0.80 — identical to their neighbours. They were NOT colour-corrected.
// !! Applying the cal squares' 10.03 mm cyan-vs-black bias to them (which IS
// !! real, for the flat reference squares) pushed them +3.7..+8.9 mm off their
// !! columns and inflated the fit rms from 0.51 to 1.28 mm.
// !! Their readings were 3291..5955 against the then-current threshold of 6000
// !! (both figures are on the pre-2026-08-20 channel/supply -- see classifyDisc),
// !! and 155 of 666 cells sit in a 5000-9000 grey zone — the separation the
// !! threshold assumes had degraded badly -- consistent with the classifier
// !! having been on CLEAR (1.6-2.1x) rather than BLUE the whole time.
//
// CAVEAT (unchanged from pass 3): with no genuinely black-back cell on the board,
// this scan still cannot measure the black-back disc offset. A mixed-colour board
// would be needed.
const float LIDAR_REF_MM = 37.69f;

// Servo-lib pulse mapping (544-2400us over 0-180deg), same as the constants above.
const float SERVO_US_0DEG    = 544.0f;
const float SERVO_US_PER_DEG = (2400.0f - 544.0f) / 180.0f;   // 10.3111 us/deg
const int   SERVO_US_MIN = 544;
const int   SERVO_US_MAX = 2400;

// Fitted standoff per cell, hundredths of a mm, [y][x].
//
// !! ROD-BEND MODEL TABLE (2026-08-04) — NOT the cubic. All the flip sketches
// !! carry this same rod table for a rig comparison; they remain in sync.
// !!   shape : pin-ended rod, d(s) = a + b·s + A·sin(πs), 3 params per column
// !! Fit quality on pass 4: rod RMS 0.572 mm / dof-adjusted σ 0.627, against the
// !! cubic's 0.513 / 0.582 and a 0.600 mm vertical-pair noise floor — so the rod
// !! leaves slightly more structure unexplained. Per-cell commanded RELEASE sits
// !! −26 .. +25 µs from the cubic table. Restore the cubic from the scan
// !! artifact's "Firmware output" block, or from git, when the comparison ends.
//
// The pass-4 board was UNIFORMLY BLANK — all 666 cells read cyan-back, so there
// is no black-back population in this scan and it cannot say anything about the
// black-back offset. The calibration squares put the true colour bias at
// 9.20 mm (cyan reads that much farther than black at identical standoff), so a
// mixed-colour board would need that re-derived before its geometry could be
// trusted.
// !! RE-TILTED 2026-08-20. The pass-4 vertical gradient was FICTITIOUS. That
// !! scan subtracted an apparent drift measured on two fixed calibration
// !! squares, and the squares move when the board does not -- so the correction
// !! was ~20-32x too large and it inverted the vertical axis. Because the scan
// !! walks rows monotonically, row index and elapsed time are the same variable
// !! (r = 0.999987), so ALL of that error landed on the vertical gradient.
// !!
// !! Corrected against four independent full-board scans (2026-08-17..20) read
// !! off the flash log. Every one of them says the bottom sits FARTHER from the
// !! arm than the top, on 128 of 148 column-passes:
// !!     scan 08-17 +1.24   08-18 +2.12   08-19 +1.04   08-20 +2.26 mm
// !!     pooled bottom-half - top-half = +1.67 mm, 95% CI [+1.35, +1.98]
// !!     this table (pre-fix) said                    -1.24 mm  (0/37 columns)
// !!
// !! The difference is a clean linear tilt: fitting (measured - table) against
// !! row index gives +0.2972 mm/row, residual sd 0.69 mm, no significant
// !! higher-order term. So the fix applied here is exactly
// !!     cell[y][x] += 0.2972 * (y - 8.5)   mm
// !! and NOTHING else -- the per-column/horizontal shape from pass 4 is kept
// !! untouched, because that axis was always separable from drift (only
// !! ~0.185 mm accrues within a row against a 0.921 mm horizontal sd).
// !!
// !! DO NOT rebuild this table cell-by-cell from the daily cadence scans. Their
// !! per-cell noise is sd 2.88 mm (SEM 1.44 mm even after averaging all four),
// !! against only 2.05 mm of real cell-to-cell structure -- an SNR near 1. The
// !! daily scans can measure a one-parameter tilt across 666 cells; they cannot
// !! measure 666 independent cells. The transform above is mean-preserving, so
// !! LIDAR_REF_MM is unchanged at 37.69 and the global push level is set purely
// !! by RELEASE_EXTRA_REACH_MM.
const int16_t LIDAR_FIT_CMM[GRID_H][GRID_W] = {
  { 3750, 3690, 3808, 3714, 3737, 3762, 3651, 3651, 3719, 3685, 3714, 3561, 3721, 3755, 3662, 3784, 3717, 3683, 3816, 3771, 3667, 3692, 3628, 3650, 3537, 3663, 3704, 3604, 3656, 3536, 3607, 3658, 3688, 3597, 3632, 3529, 3696 },
  { 3740, 3690, 3795, 3706, 3713, 3736, 3646, 3636, 3726, 3723, 3714, 3564, 3738, 3765, 3651, 3784, 3707, 3697, 3802, 3771, 3667, 3690, 3621, 3650, 3564, 3669, 3698, 3604, 3662, 3557, 3597, 3671, 3687, 3620, 3662, 3516, 3750 },
  { 3731, 3690, 3783, 3699, 3690, 3712, 3641, 3622, 3735, 3760, 3715, 3567, 3756, 3776, 3641, 3785, 3699, 3710, 3789, 3770, 3668, 3689, 3614, 3650, 3591, 3677, 3694, 3604, 3669, 3579, 3587, 3683, 3686, 3643, 3692, 3504, 3801 },
  { 3723, 3690, 3772, 3693, 3670, 3690, 3638, 3610, 3743, 3796, 3716, 3572, 3773, 3786, 3633, 3786, 3692, 3723, 3778, 3771, 3671, 3689, 3609, 3651, 3617, 3685, 3691, 3607, 3677, 3600, 3580, 3697, 3687, 3665, 3721, 3495, 3851 },
  { 3715, 3691, 3763, 3687, 3652, 3670, 3636, 3600, 3751, 3828, 3718, 3578, 3789, 3797, 3626, 3787, 3686, 3735, 3768, 3771, 3675, 3690, 3606, 3654, 3643, 3693, 3690, 3610, 3686, 3620, 3576, 3709, 3688, 3686, 3748, 3489, 3895 },
  { 3711, 3694, 3758, 3684, 3639, 3655, 3638, 3596, 3762, 3859, 3722, 3588, 3806, 3809, 3624, 3790, 3684, 3748, 3762, 3774, 3683, 3694, 3606, 3660, 3669, 3704, 3693, 3619, 3696, 3642, 3576, 3723, 3692, 3707, 3775, 3489, 3936 },
  { 3708, 3698, 3756, 3683, 3631, 3646, 3643, 3597, 3773, 3886, 3729, 3600, 3821, 3823, 3625, 3793, 3685, 3760, 3760, 3778, 3695, 3700, 3610, 3669, 3696, 3718, 3700, 3631, 3709, 3664, 3582, 3738, 3699, 3726, 3801, 3495, 3972 },
  { 3706, 3703, 3757, 3683, 3629, 3641, 3650, 3603, 3785, 3908, 3736, 3615, 3835, 3836, 3630, 3797, 3689, 3770, 3761, 3784, 3710, 3708, 3618, 3681, 3721, 3732, 3711, 3646, 3723, 3685, 3593, 3752, 3708, 3743, 3824, 3507, 4000 },
  { 3708, 3711, 3763, 3686, 3633, 3643, 3663, 3617, 3799, 3927, 3748, 3635, 3848, 3852, 3641, 3802, 3698, 3780, 3767, 3793, 3730, 3720, 3631, 3697, 3746, 3749, 3727, 3667, 3740, 3708, 3610, 3767, 3720, 3759, 3846, 3528, 4023 },
  { 3712, 3721, 3774, 3692, 3643, 3651, 3679, 3637, 3814, 3942, 3761, 3659, 3861, 3870, 3656, 3809, 3711, 3790, 3778, 3803, 3754, 3736, 3648, 3717, 3772, 3769, 3748, 3693, 3760, 3731, 3634, 3784, 3736, 3775, 3867, 3556, 4039 },
  { 3719, 3733, 3788, 3701, 3661, 3666, 3700, 3664, 3831, 3953, 3777, 3687, 3873, 3888, 3676, 3817, 3728, 3799, 3794, 3816, 3783, 3755, 3670, 3742, 3797, 3792, 3774, 3723, 3782, 3754, 3664, 3801, 3755, 3788, 3885, 3591, 4047 },
  { 3728, 3745, 3806, 3711, 3683, 3686, 3723, 3696, 3848, 3958, 3795, 3718, 3882, 3907, 3700, 3826, 3748, 3807, 3812, 3830, 3815, 3775, 3695, 3768, 3822, 3816, 3804, 3758, 3806, 3777, 3700, 3817, 3775, 3799, 3902, 3634, 4049 },
  { 3740, 3760, 3829, 3724, 3712, 3712, 3751, 3734, 3868, 3960, 3816, 3753, 3892, 3928, 3729, 3836, 3773, 3815, 3836, 3847, 3852, 3800, 3726, 3800, 3846, 3843, 3839, 3797, 3832, 3801, 3742, 3836, 3800, 3810, 3917, 3683, 4045 },
  { 3754, 3777, 3855, 3740, 3746, 3743, 3782, 3779, 3888, 3960, 3839, 3791, 3902, 3950, 3762, 3847, 3802, 3822, 3864, 3866, 3893, 3827, 3760, 3834, 3871, 3872, 3879, 3840, 3861, 3825, 3789, 3854, 3827, 3820, 3932, 3740, 4035 },
  { 3769, 3794, 3883, 3756, 3783, 3778, 3815, 3826, 3909, 3955, 3862, 3831, 3909, 3973, 3798, 3858, 3832, 3828, 3893, 3885, 3936, 3855, 3797, 3870, 3895, 3902, 3920, 3886, 3891, 3849, 3839, 3873, 3855, 3827, 3944, 3800, 4020 },
  { 3786, 3813, 3914, 3774, 3825, 3817, 3851, 3878, 3931, 3949, 3888, 3874, 3917, 3996, 3837, 3871, 3865, 3834, 3927, 3905, 3982, 3886, 3837, 3909, 3919, 3934, 3965, 3935, 3922, 3874, 3894, 3892, 3885, 3835, 3956, 3865, 4002 },
  { 3804, 3832, 3947, 3793, 3869, 3858, 3888, 3933, 3954, 3941, 3915, 3918, 3925, 4021, 3877, 3884, 3899, 3840, 3961, 3927, 4030, 3919, 3878, 3949, 3944, 3967, 4012, 3986, 3955, 3899, 3951, 3912, 3917, 3842, 3967, 3933, 3982 },
  { 3822, 3852, 3980, 3813, 3914, 3900, 3926, 3988, 3977, 3933, 3942, 3963, 3933, 4045, 3919, 3897, 3935, 3846, 3997, 3950, 4078, 3951, 3920, 3990, 3968, 4000, 4060, 4038, 3987, 3924, 4009, 3932, 3949, 3849, 3979, 4003, 3960 },
};

// Compensated pulse width for a contact position at cell (gx,gy).
int compensatedUs(int baseUs, int gx, int gy) {
  // Pin-length / rod-compliance trim first, and always — see
  // RELEASE_EXTRA_REACH_MM. This term is OUTSIDE the LIDAR_COMP_MODE gate.
  float delta = RELEASE_EXTRA_REACH_MM + edgeRowReachTrim(gy);
#if LIDAR_COMP_MODE == 0
  (void)gx;   // gy is used by edgeRowReachTrim above
#else
  delta += (LIDAR_FIT_CMM[gy][gx] / 100.0f) - LIDAR_REF_MM;
#endif
  if (delta == 0.0f) return baseUs;
  float baseDeg = (baseUs - SERVO_US_0DEG) / SERVO_US_PER_DEG;
  float s = sinf((baseDeg - ARM_FLAT_DEG) * DEG_TO_RAD) + delta / ARM_LEN_MM;
  if (s >  1.0f) s =  1.0f;          // beyond the arm's reach - clamp, don't wrap
  if (s < -1.0f) s = -1.0f;
  float deg = ARM_FLAT_DEG + asinf(s) * RAD_TO_DEG;
  long us = lroundf(SERVO_US_0DEG + deg * SERVO_US_PER_DEG);
  if (us < SERVO_US_MIN) us = SERVO_US_MIN;
  if (us > SERVO_US_MAX) us = SERVO_US_MAX;
  return (int)us;
}

// Step-3 second-catch pass: after the main flip+catch, drop the arm a further
// ~10° (to RELEASE2, ~36°) and sweep +X once more to push back any disc the
// first catch left over/under-rotated. Comment this out to remove the
// second-catch back-move (the main flip then runs without the extra pass).
//#define FLIP_SECOND_CATCH

// Servo control offloaded to a dedicated 5V Arduino Nano over a one-way 9600
// baud serial line on Arduino D9 (the old SERVO_PIN, freed once the SG90 moved
// to the 5V Nano) → 5V Nano D2 RX (SoftwareSerial; D0 is its USB debug echo), shared GND. The companion sketch
// (ServoNano.ino) listens on its hardware UART at the same baud and parses an
// integer µs value per line.
//
// On the RP2040 this was a BIT-BANGED software UART (mbed's UART class on an
// arbitrary PinName crashed the chip), which meant interrupts off for ~1 ms per
// byte and a hand-tuned SERVO_TX_BIT_US. The ESP32-S3's GPIO matrix can route
// any UART peripheral to any pin, so the same wire is now driven by real
// hardware UART2 — TX only, RX pin -1 (nothing comes back on this link; the ack
// is a separate level line, see below). No bit timing to tune, no interrupt
// blackout, and the FIFO makes the send effectively non-blocking.
const int SERVO_TX_PIN = D9;

// Every command is framed with a LEADING newline as well as a trailing one. If
// a byte is dropped on this one-way link, the leading newline of the next
// command terminates whatever partial line is stranded in the ServoNano's
// buffer, so two commands can never be glued into one number. (ServoNano
// ignores empty lines, so the extra newline costs nothing but one byte.)
// The link is ONE-WAY with no ack, so a dropped byte silently LOSES a command
// and the arm simply stays where it was. That broke the flip arm once: a lost
// REST left it at ENGAGE, and flipDisc then ran both X strokes with the arm
// buried in the board. A receiver-side check cannot help -- a command that never
// arrives cannot be rejected -- so every command is sent SERVO_TX_REPEATS times.
// writeMicroseconds() is idempotent, so the repeats are free: re-commanding the
// position the servo already holds does nothing. Losing a command now takes
// SERVO_TX_REPEATS independent dropouts instead of one.
//
// The repeats also fix LATE application: if only the trailing newline is lost,
// the stranded digits sit in the ServoNano's buffer until the NEXT command's
// leading newline flushes them -- which without repeats is up to a full settle
// period later, i.e. after the stroke has already started. The next repeat
// flushes them SERVO_TX_REPEAT_GAP_MS later instead.
// With the ack line fitted (SERVO_ACK_MODE 2) blind repeats are OBSOLETE and
// actively harmful: writeServoUs() now detects a lost command and retries, which
// is strictly better than sending 3 copies and hoping. Each extra copy costs the
// ServoNano ~6 ms with INTERRUPTS DISABLED (SoftwareSerial::recv holds them off
// for 9.75 bit times = 1.02 ms per byte), and the Servo library needs its Timer1
// ISR on time. At 3 copies every command spanned 1.56 servo frames at 59 %
// blocked duty, so a 544 us REST pulse was routinely stretched by up to 1016 us
// -> ~1560 us, which IS the ENGAGE command. The servo twitched toward engage on
// nearly every command (audible buzzing, no stall), and when enough consecutive
// pulses were stretched the arm never left ENGAGE inside the 300 ms settle --
// the arm-breaking failure, made ~40x more common by the repeats meant to fix it.
// Keep this at 1 whenever SERVO_ACK_MODE is 2.
const int SERVO_TX_REPEATS = 1;
const int SERVO_TX_REPEAT_GAP_MS = 6;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    // Block until the last stop bit is actually on the wire, so this call stays
    // synchronous like the old bit-bang did — callers (and the ack wait in
    // writeServoUs) time their settle delay from here.
    Serial2.flush();
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}


// ---------------------------------------------------------------- servo ack
// ServoNano D3 --[1.8k]--+--> this pin (D2);  3.3k from that junction to GND.
// The divider is MANDATORY: the ESP32-S3 is not 5V tolerant either (abs max
// VDD+0.3 = 3.6 V, same as the RP2040 this replaced) and the ServoNano drives
// 5 V. 5.0*3.3/(1.8+3.3) = 3.24 V. See ServoNano.ino. The other direction
// (D9 -> ServoNano) needs nothing, since 3.3 V clears the AVR's V_IH of
// 0.6*Vcc = 3.0 V.
//
// The line is a LEVEL, not a UART: idle LOW, driven HIGH for ~40 ms on every
// command the ServoNano accepts. We already know what we sent, so all we need
// is "it landed".
//
// !! POLARITY IS ACTIVE-HIGH (inverted 2026-08-17). It used to be idle-HIGH /
// !! pulse-LOW with INPUT_PULLUP, on the theory that a broken wire would read
// !! HIGH = "no ack" = fail loud. That is only true if the break is at THIS
// !! pin. The divider's bottom-leg resistor sits between this node and GND, so
// !! a break ANYWHERE UPSTREAM -- the ack wire, an unplugged or unpowered
// !! ServoNano -- pinned the node LOW through it, which the old code read as
// !! "acked". Every command then reported success and SERVO_ACK_MODE 2's
// !! retry-until-confirmed guarantee became vacuous. No divider ratio fixes
// !! that; the polarity has to be the other way round.
// !!
// !! Now the bottom-leg resistor IS the fail-safe: any upstream break parks the
// !! node LOW = "no ack" = the enforced retry actually fires. INPUT_PULLDOWN
// !! covers the remaining case where the divider itself is absent.
// !!
// !! REQUIRES A 3.3k BOTTOM LEG. INPUT_PULLDOWN (~45k) sits in parallel with
// !! it, so with 1.8k/3.3k the HIGH level is 5*(3.3||45)/(1.8+(3.3||45)) =
// !! 3.15 V, comfortably over the ESP32-S3's V_IH of 0.75*VDD = 2.475 V. With a
// !! 2k bottom leg it collapses to 2.45 V and the ack stops working. Do NOT
// !! flash this onto a rig whose divider is 2k/2k.
// !!
// !! BOTH SIDES MUST BE FLASHED TOGETHER. A ServoNano running the old
// !! active-LOW build idles HIGH, which this code would read as a permanent
// !! ack -- the exact failure the change removes. servoAckProbeIdle() below
// !! detects that at boot and says so loudly.
//
// SERVO_ACK_MODE  0 = off      (no wire fitted; original open-loop behaviour)
//                 1 = observe  (log every missing ack, keep running)
//                 2 = enforce  (retry FOREVER; never move without the ack)
// Mode 2 is live: the divider + ack wire are fitted.
#define SERVO_ACK_MODE 2
const int SERVO_ACK_PIN = D2;
const unsigned long SERVO_ACK_TIMEOUT_MS = 80;   // must exceed the ~6 ms frame
unsigned long servoAckMisses = 0;
unsigned long servoAckStuck  = 0;   // line stuck asserted -> unverifiable
// Idle-wait budget. Must exceed ACK_HOLD_MS (40) so a legitimate hold from the
// PREVIOUS command is never mistaken for a stuck line.
const unsigned long SERVO_ACK_IDLE_TIMEOUT_MS = 100;

#if SERVO_ACK_MODE > 0
// Read the ack as an ANALOG level, not a digital one.
//
// !! WHY: the divider feeds a 5 V swing into a 3.3 V pin, so the asserted level
// !! depends entirely on the divider ratio, and digitalRead compares it against
// !! the ESP32-S3's V_IH of 0.75*VDD = 2.475 V. The rig is physically wired
// !! 2k/2k, which puts the asserted level at 2.45-2.50 V -- straddling V_IH, so
// !! digitalRead is a coin flip (and with INPUT_PULLDOWN it reads LOW outright,
// !! which would hang SERVO_ACK_MODE 2 forever on the first command). The shield
// !! PCB uses 2k/3.3k and would be fine, but the two must run one firmware.
// !!
// !! Comparing against a threshold far below BOTH divider ratios' asserted level
// !! removes the dependency on V_IH entirely: idle is 0 V (the divider's bottom
// !! leg IS the pulldown), asserted is 2.45 V at worst. 1.20 V sits >1.2 V from
// !! either state. This is strictly more robust than digitalRead ever was here,
// !! and it works unchanged on 2k/2k, 2k/3.3k and 1.8k/3.3k.
// !!
// !! SERVO_ACK_PIN = D2 = GPIO5 = ADC1_CH4. ADC1 is mandatory: ADC2 is unusable
// !! while WiFi is running, and PARMain always has WiFi up.
const int SERVO_ACK_THRESHOLD_MV = 1200;
static inline bool servoAckHigh() {
  return analogReadMilliVolts(SERVO_ACK_PIN) > SERVO_ACK_THRESHOLD_MV;
}

// Let the previous command's 40 ms hold expire so it cannot be mistaken for ours.
// Returns FALSE if the line never returned to idle -- i.e. it is stuck asserted.
//
// !! THIS RETURN VALUE IS SAFETY-CRITICAL, DO NOT IGNORE IT. Under the
// !! active-HIGH protocol a line stuck HIGH (ServoNano wedged mid-ack, a short
// !! to the divider's top leg, or a ServoNano still running the old active-LOW
// !! build) makes servoAckSeen() return true instantly and unconditionally.
// !! The ack would then confirm every command without any command having
// !! landed, and SERVO_ACK_MODE 2's retry-until-confirmed guarantee -- the one
// !! thing stopping the carriage from moving on an unconfirmed arm position --
// !! becomes vacuous. A stuck line must be treated as a FAULT, never as an ack.
static bool servoAckWaitIdle() {
  unsigned long t0 = millis();
  while (servoAckHigh()) {
    if (millis() - t0 >= SERVO_ACK_IDLE_TIMEOUT_MS) return false;
  }
  return true;
}
static bool servoAckSeen() {
  unsigned long t0 = millis();
  while (millis() - t0 < SERVO_ACK_TIMEOUT_MS)
    if (servoAckHigh()) return true;
  return false;
}

// Boot-time firmware-match check. Under the active-HIGH protocol the line must
// IDLE LOW; a ServoNano still running the old active-LOW build idles HIGH, and
// this code would then read every command as instantly acked. Returns false if
// the line never goes LOW -- stale ServoNano firmware, or a short to 5 V.
static bool servoAckProbeIdle() {
  unsigned long t0 = millis();
  while (millis() - t0 < 250)
    if (!servoAckHigh()) return true;
  return false;
}
// Mode 2 RETRIES FOREVER rather than giving up. Blocking here is the safe
// failure: every writeServoUs() call site is reached with GRBL already idle
// (flipDisc waits for motion before each servo move, and scanGrid's mid-scan
// servo changes happen before the next move is queued), so a stall leaves the
// carriage stationary with no G-code in flight. It deliberately does NOT reset
// the MCU on failure -- a reset re-homes, and homing would drag the carriage
// with the arm possibly still at ENGAGE, which is exactly how the arm broke.
#endif

void writeServoUs(int us, int settle_ms) {
#if SERVO_ACK_MODE == 0
  servoTxLine(us);
  delay(settle_ms);
#else
  unsigned long t0 = millis();
  unsigned long attempt = 0;
  bool acked = false;
  do {
    attempt++;
    // Stuck-asserted line: an ack read now would be meaningless. Treat exactly
    // like a missing ack -- block and retry -- rather than believing it.
    if (!servoAckWaitIdle()) {
      servoAckStuck++;
      if (attempt <= 5 || (attempt % 100) == 0) {
        Serial.print("!! servo ack STUCK ASSERTED - cannot verify, blocking. us=");
        Serial.print(us); Serial.print(" attempt="); Serial.println(attempt);
      }
#if SERVO_ACK_MODE == 1
      break;                    // observe-only: record it and carry on
#else
      delay(50);
      continue;                 // mode 2: never accept an unverifiable ack
#endif
    }
    servoTxLine(us);
    acked = servoAckSeen();
    if (!acked) {
      servoAckMisses++;
      // Throttle: a disconnected ack wire would otherwise flood the log.
      if (attempt <= 5 || (attempt % 100) == 0) {
        Serial.print("!! servo ack MISSING us="); Serial.print(us);
        Serial.print(" attempt="); Serial.print(attempt);
        Serial.print(" totalMisses="); Serial.println(servoAckMisses);
      }
    }
#if SERVO_ACK_MODE == 1
    break;                      // observe-only: record the miss and carry on
#endif
  } while (!acked);             // mode 2: retry FOREVER, never move unconfirmed
  unsigned long spent = millis() - t0;
  if ((unsigned long)settle_ms > spent) delay(settle_ms - spent);
#endif
}

struct Coord {
  float x;
  float y;
};

// grid[GRID_H-1][0] is bottom-left at the home-relative origin; physical y
// increases upward while bitmap y=0 is the top row, so y is mirrored when
// computing gridY.
Coord grid[GRID_H][GRID_W];

// Currently-displayed disc colors: 0 = black, 1 = blue.
uint8_t gridState[GRID_H][GRID_W];

// Fixed-size buffer (digits-only id from the server) — avoids Arduino String
// heap fragmentation across many loop iterations. Empty when no display is
// pending confirmation.
char pendingGalleryId[16] = "";

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      //           starting offset⌄
      // Starting X offset is 25 mm; the addressable grid covers physical
      // cols 0..36 as logical x=0..36.
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
      //                                  ⌃grid spacing
      gridState[y][x] = 0;
    }
  }
}

// GRBL character-counting streaming protocol. GRBL's RX buffer is 128 bytes;
// keep a few bytes of slack so we never overrun.
const int RX_BUFFER_SAFE = 120;
const int QUEUE_SIZE = 32;

// If sendGcode/waitForIdle stall this long without seeing any GRBL progress
// (no `ok` consumed, no buffer drain), assume comms with the Mega are wedged
// and force an MCU reset so the device recovers on its own. 60s is well above
// any legitimate motion or homing time we issue at this granularity.
const unsigned long GRBL_STALL_TIMEOUT_MS = 60000;

// Ceiling on grblBringup's escalating retry backoff when GRBL will not answer
// `$H`. See the restart_grbl block for why the retry loop exists at all.
const unsigned long GRBL_RETRY_BACKOFF_MAX_MS = 60000;

// We also keep the command text per slot so error:N can be retried (re-sent)
// without the caller knowing. 40 bytes matches the largest snprintf buffer
// used by moveTo()/flipDisc()/etc. Anything longer than that is a bug.
const int MAX_CMD_LEN = 40;
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

// Diagnostic counters for the streaming path. A healthy sweep keeps gOksAcked
// tracking gCmdsSent; a growing gap (or bufferFill/queue depth that won't drain
// to ~0 between rows) means GRBL is not consuming what we send — i.e. moves are
// being dropped, which surfaces as flips that never physically happen even
// though displayBitmap issued them. Logged per row by displayBitmap().
unsigned long gCmdsSent = 0;
unsigned long gOksAcked = 0;

// error:N retry bookkeeping. When the same command errors repeatedly we cap
// at MAX_ERROR_RETRIES then MCU-reset as a last resort.
const int MAX_ERROR_RETRIES = 10;
const unsigned long ERROR_RETRY_DELAY_MS = 3000;
int errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";

// During setup's homing phase, an `error`/`ALARM` from GRBL is recoverable —
// bounce Serial1 and retry rather than wedging forever. Outside setup we keep
// the legacy hard-halt behavior so a fault mid-job stops the rig immediately.
bool inStartupPhase = false;
volatile bool grblStartupFault = false;

// True when gridState[] is trustworthy enough to draw against without a fresh
// scan. Set after each scanGrid() completes; at the end of a job it is carried
// over only when gridStateFromScan says the state is still *measured* (see
// below). Lets us skip the redundant re-scan on the first job after boot, and
// after any job whose last board-touching action was a scan.
bool gridStateFresh = false;

// True when gridState[] came straight from a scanGrid() and nothing has flipped
// a disc since — i.e. it is a measured picture of the board, not one inferred
// from the flips we *believe* landed. Cleared by displayBitmap() as soon as it
// commits to flipping anything. The check pass frequently ends with a scan and
// no fix (<= CHECK_FIX_MAX_SKIP wrong), and in that case this stays true, so
// the next job can reuse that scan instead of paying another ~70 min sweep.
bool gridStateFromScan = false;

// True when the steppers were released ($1=0) since the last homing cycle, so
// the gantry may have been nudged and must re-home before any motion. scanGrid()
// homes on its own; this covers the path where the scan is skipped.
bool needsRehome = false;
// GRBL brought up + homed this boot. False until grblBringup() runs — which
// loop() only calls once cadenceGate() says the rig is clear to operate
// (trusted NTP clock AND daytime). Never home from setup(): a nocturnal or
// clockless reset must not move the gantry (review F2/F3).
bool grblHomed = false;

// Park the servo at REST before any reset so the gantry doesn't reboot with the
// arm mid-flip — leaving it engaged can foul the next homing pass. The 5V Nano
// keeps driving the servo across our reset, so a single µs command is enough.
void parkServoForReset() {
  servoTxLine(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);
}

// ESP32-S3: esp_restart() from <esp_system.h> (was NVIC_SystemReset() on the
// RP2040's CMSIS headers). It shuts the peripherals down and reboots the SoC;
// esp_reset_reason() reports ESP_RST_SW afterwards, which is how setup()'s
// breadcrumb tells our own resets apart from a brownout.
void grblStallReset(const char* where) {
  plog::logf("GRBL stall in %s -> MCU reset", where);
  parkServoForReset();
  delay(50);
  esp_restart();
}

// WiFi reconnect: per-attempt association timeout, infinite retries, with a
// total stall watchdog that mirrors the GRBL pattern — if we can't get back on
// the network within 60s, reset the MCU so a wedged radio recovers on its own
// instead of silently failing every HTTPS call forever.
const unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 10000;
const unsigned long WIFI_STALL_TIMEOUT_MS = 60000;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  plog::log("wifi (re)connecting");
  unsigned long stallT0 = millis();
  for (int attempt = 1;; attempt++) {
    // WiFi.disconnect(true) also releases the driver/netif — the closest
    // equivalent of WiFiNINA's WiFi.end(). (The old _spi_init(spi1, 8000000)
    // that followed it reset the NINA co-processor's SPI bus; on the ESP32 the
    // radio is on-die, so there is no such bus and nothing to re-init.)
    WiFi.disconnect(true);
    delay(1000);  // let the driver fully tear down before re-init
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, PASSWORD);
    unsigned long t0 = millis();
    while (millis() - t0 < WIFI_ATTEMPT_TIMEOUT_MS) {
      int s = WiFi.status();
      plog::logf("a=%d t=%lu s=%d", attempt, millis() - t0, s);
      if (s == WL_CONNECTED) {
        plog::logf("wifi connected attempt=%d rssi=%d",
                   attempt, (int)WiFi.RSSI());
        return;
      }
      delay(500);
    }
    // Log the actual status code so we know what state it's stuck in
    plog::logf("wifi attempt %d timeout status=%d", attempt, (int)WiFi.status());
    if (millis() - stallT0 > WIFI_STALL_TIMEOUT_MS) {
      plog::log("wifi stall -> MCU reset");
      parkServoForReset();
      delay(50);
      esp_restart();
    }
  }
}

void enqueue(const char* cmd, int len) {
  // QUEUE_SIZE slots should always exceed the in-flight count (bufferFill is
  // capped at RX_BUFFER_SAFE), so a full ring here means accounting is already
  // wrong (an ok went missing) and we're about to clobber an un-acked slot —
  // which corrupts cmdLengths[] and permanently desyncs bufferFill.
  if ((qTail + 1) % QUEUE_SIZE == qHead) {
    plog::logf("QUEUE FULL overwrite qH=%d qT=%d buf=%d", qHead, qTail, bufferFill);
  }
  strncpy(cmdTexts[qTail], cmd, MAX_CMD_LEN - 1);
  cmdTexts[qTail][MAX_CMD_LEN - 1] = '\0';
  cmdLengths[qTail] = len;
  qTail = (qTail + 1) % QUEUE_SIZE;
}

int dequeue() {
  int len = cmdLengths[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return len;
}

// Direct UART send that bypasses the char-counting queue — used only inside
// recovery paths (drainResponses retry, ALARM recovery) which manage buffer
// accounting themselves.
void rawSerial1Line(const char* s) {
  Serial1.print(s);
  Serial1.write('\n');
}

// ---- GRBL response hygiene -------------------------------------------------
// A SINGLE noise byte on the Serial1 RX line used to cost an entire print.
//
// 2026-08-23: a 0xFF glitch arrived glued to an ack — the line read `\xffok`.
// drainResponses tests `resp == "ok"` exactly, and String::trim() strips only
// ASCII whitespace, never 0xFF, so the ack fell through to the catch-all "GRBL
// other" branch. The queue slot was never dequeued, bufferFill never drained,
// and 60 s later the stall watchdog reset the MCU mid-job. It happened twice in
// twelve minutes and killed two jobs: gallery 79 stopped after row y=7 with its
// top 7 rows undrawn, and gallery 80 was popped off the queue and lost outright
// (next.php had already consumed it; the reset meant nothing ever drew it).
//
// A start-bit glitch on an idle-high UART reads as exactly 0xFF, so that is the
// shape line noise takes here. GRBL's entire response vocabulary — `ok`,
// `error:N`, `ALARM:N`, `<...>` status, `[MSG:...]`, `$N=...`, the banner — is
// printable ASCII, so dropping every byte outside 0x20..0x7E cannot corrupt a
// legitimate response. It can only rescue a corrupted one.
//
// This MASKS THE SYMPTOM OF A HARDWARE FAULT (cable, connector, or Mega power),
// so the scrubbing is counted and logged rather than silent — a rising
// gRxScrubbed is the evidence that the underlying fault is still present.
const size_t GRBL_RESP_MAX = 96;
unsigned long gRxScrubbed = 0;       // total non-printable bytes dropped
unsigned long gRxScrubbedLines = 0;  // responses that needed scrubbing

// Drop non-printable bytes in place. Only reassigns the String when something
// was actually dropped, so the healthy path costs one stack buffer and no heap
// traffic. Runs BEFORE trim() — 0x0D is non-printable and goes here, spaces
// (0x20) are printable and are left for trim() to handle.
static void grblScrub(String& s) {
  char buf[GRBL_RESP_MAX];
  size_t n = 0;
  unsigned long dropped = 0;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s.charAt(i);
    if (c >= 0x20 && c <= 0x7E) {
      if (n < sizeof(buf) - 1) buf[n++] = (char)c;
    } else {
      dropped++;
    }
  }
  if (!dropped) return;
  buf[n] = '\0';
  gRxScrubbed += dropped;
  gRxScrubbedLines++;
  // Throttled like the servo-ack miss log: loud at first, then periodic, so a
  // permanently noisy line cannot flood the flash log.
  if (gRxScrubbedLines <= 5 || (gRxScrubbedLines % 100) == 0) {
    plog::logf("GRBL rx scrubbed %lu byte(s) -> '%.20s' (line %lu, total %lu)",
               dropped, buf, gRxScrubbedLines, gRxScrubbed);
  }
  s = buf;
}

// Read one GRBL response line, scrubbed and trimmed. Returns an empty String
// on a blank/timed-out line, which every caller already skips.
static String grblReadLine() {
  String r = Serial1.readStringUntil('\n');
  grblScrub(r);
  r.trim();
  return r;
}

// ---- tolerant response matching --------------------------------------------
// grblScrub above drops only NON-printable bytes, and that turned out to be
// exactly half a fix. On 2026-08-25 the noise arrived as PRINTABLE characters
// glued to the front of an ack -- `Dok`, `Pok`, `Tok`, `Vok`, `}S?xok` -- which
// sailed through the scrubber, failed `resp == "ok"`, and fell into the
// catch-all "GRBL other" branch. Every one of those was a LOST ACK: the queue
// slot was never dequeued, bufferFill never drained, and 60 s later the stall
// watchdog reset the MCU. It fired 7 times between 10:00 and 12:13, and because
// runDailyLidarScan restarted from cell 0 on every boot (see its resume note),
// the rig spent three hours re-running the first third of the sweep and never
// polled for a print at all.
//
// So match on CONTENT, not equality. GRBL's vocabulary makes that safe: the
// only response containing the substring "ok" is `ok` itself. `error:N`,
// `ALARM:N`, `<...>` status reports, `[MSG:...]`, `$N=...` and the
// `Grbl 1.1f ['$' for help]` banner all lack it -- including
// `[MSG:'$H'|'$X' to unlock]`, whose "unlock" is u-n-l-o-c-k with no "ok".
// The length bound is the second half of the guard: a long line that merely
// happens to contain "ok" can never be mistaken for an ack, so only a short
// line that is an ack plus a few junk bytes qualifies.
//
// This still MASKS A HARDWARE FAULT, so recoveries are counted and logged for
// the same reason gRxScrubbed is -- see the note above grblScrub.
const size_t GRBL_OK_MAX_JUNK_LEN = 8;   // the worst seen, `}S?xok`, is 6
unsigned long gRxCorruptOks = 0;         // acks recovered from a corrupted line

static bool grblIsOk(const String& r) {
  if (r == "ok") return true;                        // the overwhelmingly common case
  if (r.length() > GRBL_OK_MAX_JUNK_LEN) return false;
  if (r.indexOf("ok") < 0) return false;
  // Never swallow a response the branches below know how to handle properly.
  if (r.indexOf("error") >= 0 || r.indexOf("ALARM") >= 0) return false;
  gRxCorruptOks++;
  if (gRxCorruptOks <= 5 || (gRxCorruptOks % 20) == 0) {
    plog::logf("GRBL ok recovered from '%.12s' (total %lu)", r.c_str(), gRxCorruptOks);
  }
  return true;
}

// Same reasoning for the two responses we must never miss. `startsWith` fails
// on a leading junk byte exactly as `== "ok"` did, and a dropped `error:N`
// leaks a queue slot while a dropped `ALARM` lets the sweep keep streaming into
// a GRBL that is no longer executing anything.
static inline bool grblHasAlarm(const String& r) { return r.indexOf("ALARM") >= 0; }
static inline bool grblHasError(const String& r) { return r.indexOf("error") >= 0; }

// ---- idle-line noise --------------------------------------------------------
// Every corrupted ack on record was junk PREFIXED to a real `ok`, never junk
// alone. That is the signature of a byte that arrives while the RX line is
// idle: it carries no newline, so readStringUntil() cannot terminate on it and
// it simply waits in the UART buffer until the NEXT genuine response completes
// the line -- turning `ok` into `Dok`.
//
// The window is widest during the daily lidar sweep, which parks the carriage
// for ~4.95 s per cell while the ranger takes ~83 I2C samples. Measured over
// the 2026-08-25 logs, stray bytes per 1000 GRBL lines: draw 0.0, colour scan
// 0.4, lidar scan 63.4. Roughly 160x, and confined to the phase where the
// ranger is active -- so the ranger (I2C at 400 kHz on A4/A5, or supply
// transients from its emitter) is implicated, not merely the longer idle gap.
// THIS IS A SOFTWARE GUARD AGAINST A HARDWARE PROBLEM. It stops the fragment
// from corrupting an ack; it does not stop the fragment.
//
// Called wherever nothing is outstanding, so anything already buffered is
// either a complete unsolicited line (logged -- a bare `Grbl ` banner here
// means the MEGA reset itself) or exactly such an orphan fragment.
unsigned long gRxIdleDropped = 0;
unsigned long gRxIdleEvents  = 0;

static void grblDropIdleNoise() {
  if (bufferFill != 0 || qHead != qTail) return;   // a real ack may still be owed
  if (!Serial1.available()) return;

  char pend[GRBL_RESP_MAX];
  size_t n = 0;
  while (Serial1.available() && n < sizeof(pend)) pend[n++] = (char)Serial1.read();

  // Everything up to the LAST newline is complete line(s); the tail after it is
  // the fragment. Reading the bytes ourselves keeps this off readStringUntil(),
  // whose 1 s timeout would otherwise be paid on every fragment.
  int nl = -1;
  for (size_t i = 0; i < n; i++) if (pend[i] == '\n') nl = (int)i;

  if (nl >= 0) {
    pend[nl] = '\0';
    String line(pend);
    grblScrub(line);       // also flattens any embedded \r\n of earlier lines
    line.trim();
    if (line.length()) plog::logf("GRBL idle line: %.40s", line.c_str());
  }

  size_t frag = n - (size_t)(nl + 1);
  if (frag == 0) return;
  gRxIdleDropped += frag;
  gRxIdleEvents++;
  if (gRxIdleEvents <= 5 || (gRxIdleEvents % 50) == 0) {
    plog::logf("GRBL dropped %u idle-noise byte(s) (event %lu, total %lu)",
               (unsigned)frag, gRxIdleEvents, gRxIdleDropped);
  }
}

// Synchronous "send + wait for ok" used during ALARM recovery. Does not use
// the queue — recovery runs with bufferFill=0 and we want one ack at a time.
// Returns true on ok, false on timeout/error/ALARM (caller will MCU-reset).
bool recoverySendAndWait(const char* cmd, unsigned long timeout_ms) {
  while (Serial1.available()) Serial1.read();
  rawSerial1Line(cmd);
  unsigned long t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (Serial1.available()) {
      String r = grblReadLine();
      if (r.length() == 0) continue;
      if (grblIsOk(r)) return true;
      if (grblHasError(r) || grblHasAlarm(r)) {
        plog::logf("recovery '%s' got %.40s", cmd, r.c_str());
        return false;
      }
      // Ignore status reports, [MSG:...], banner lines, $$ output.
    }
  }
  plog::logf("recovery '%s' timeout", cmd);
  return false;
}

// ALARM recovery: soft-reset GRBL (Ctrl-X / 0x18), wait for boot, re-home,
// reassert modal state. On any sub-step failure we MCU-reset as a fallback —
// matches the existing stall-watchdog policy. Clears the command queue on
// success and lets the in-flight sendGcode/waitForIdle return as if the
// pending motion completed; whatever the caller was mid-way through is lost,
// but the rig stays alive.
void grblAlarmRecover() {
  plog::log("ALARM recovery: soft reset + rehome");
  Serial1.write(0x18);  // Ctrl-X — GRBL soft reset
  delay(2000);          // GRBL boot wait
  while (Serial1.available()) Serial1.read();

  qHead = qTail = 0;
  bufferFill = 0;
  errorRetryCount = 0;
  lastErrorCmd[0] = '\0';

  if (!recoverySendAndWait("$H", 60000)) {
    plog::log("ALARM recovery $H failed -> MCU reset");
    parkServoForReset();
    delay(50);
    esp_restart();
  }
  if (!recoverySendAndWait("$1=255", 5000) ||
      !recoverySendAndWait("G21", 5000) ||
      !recoverySendAndWait("G90", 5000)) {
    plog::log("ALARM recovery modal-set failed -> MCU reset");
    parkServoForReset();
    delay(50);
    esp_restart();
  }

  // The board state is unknown after an ALARM — force a re-scan next job.
  gridStateFresh = false;
  gridStateFromScan = false;
  plog::log("ALARM recovery complete");
}

// (USB Serial removed — flash log is the only debug trail. Serial1 is the
// GRBL UART and is retained.)

void drainResponses() {
  while (Serial1.available()) {
    String resp = grblReadLine();
    if (resp.length() == 0) continue;

    if (grblIsOk(resp)) {
      // grbl-Mega can emit a duplicate `ok` after $H (one when alarm clears,
      // one when homing completes). Without this guard the spurious ack
      // dequeues a stale slot, desyncing bufferFill so waitForIdle hangs.
      if (qHead != qTail) {
        bufferFill -= dequeue();
        gOksAcked++;
        // A clean ok in between errors resets the consecutive-error counter —
        // we only MCU-reset when the *same* command keeps failing.
        errorRetryCount = 0;
        lastErrorCmd[0] = '\0';
      } else {
        // An ok with nothing queued is a spurious/duplicate ack. The guard
        // keeps it from under-flowing bufferFill, but each one means GRBL
        // emitted more oks than we sent commands — log it; a run of these is
        // the accounting desync that lets later sends overrun GRBL's RX buffer.
        plog::log("GRBL ok but queue empty (spurious ack)");
      }
    } else if (grblHasAlarm(resp)) {
      // Log the command GRBL was processing when it alarmed — for ALARM:2
      // (soft-limit) this is the move whose target left the envelope, which is
      // exactly what we need to pinpoint the offending coordinate.
      if (qHead != qTail) {
        plog::logf("GRBL ALARM: %.16s on '%.18s'", resp.c_str(), cmdTexts[qHead]);
      } else {
        plog::logf("GRBL ALARM: %.34s (queue empty)", resp.c_str());
      }
      if (inStartupPhase) {
        grblStartupFault = true;
        return;
      }
      grblAlarmRecover();
      // After recovery the queue is empty. The caller's send-buffer-wait or
      // waitForIdle loop will see bufferFill==0 and exit cleanly.
      return;
    } else if (grblHasError(resp)) {
      if (inStartupPhase) {
        plog::logf("GRBL startup error: %.40s", resp.c_str());
        grblStartupFault = true;
        return;
      }
      // error:N is non-fatal — GRBL still consumed the line (char-counting
      // protocol acks error the same as ok), so drop the queue slot, then
      // re-send the same command after a delay. Cap at MAX_ERROR_RETRIES for
      // the *same* command in a row before giving up and MCU-resetting.
      if (qHead == qTail) {
        plog::logf("GRBL error with empty queue: %.40s", resp.c_str());
        continue;
      }
      char failedCmd[MAX_CMD_LEN];
      strncpy(failedCmd, cmdTexts[qHead], MAX_CMD_LEN);
      failedCmd[MAX_CMD_LEN - 1] = '\0';
      bufferFill -= dequeue();

      if (strcmp(failedCmd, lastErrorCmd) == 0) {
        errorRetryCount++;
      } else {
        strncpy(lastErrorCmd, failedCmd, MAX_CMD_LEN);
        lastErrorCmd[MAX_CMD_LEN - 1] = '\0';
        errorRetryCount = 1;
      }

      plog::logf("GRBL %.20s on '%.20s' retry %d/%d",
                 resp.c_str(), failedCmd, errorRetryCount, MAX_ERROR_RETRIES);

      if (errorRetryCount > MAX_ERROR_RETRIES) {
        plog::logf("error retries exhausted for '%s' -> MCU reset", failedCmd);
        parkServoForReset();
        delay(50);
        esp_restart();
      }

      delay(ERROR_RETRY_DELAY_MS);

      // Re-send inline (don't recurse through sendGcode — we're already in
      // its drain loop). Subsequent commands queued behind this one were
      // sent earlier and may already be in flight; re-enqueuing at tail
      // means the retry runs *after* them. For the small atomic commands
      // that typically error (modal switches, $-settings) that ordering is
      // benign; motion-sensitive sequences would have already faulted via
      // ALARM rather than error.
      int rlen = strlen(failedCmd) + 1;
      rawSerial1Line(failedCmd);
      bufferFill += rlen;
      strncpy(cmdTexts[qTail], failedCmd, MAX_CMD_LEN - 1);
      cmdTexts[qTail][MAX_CMD_LEN - 1] = '\0';
      cmdLengths[qTail] = rlen;
      qTail = (qTail + 1) % QUEUE_SIZE;
    } else {
      // Anything else — status reports `<...>` from `?`, settings lines
      // `$N=...` from `$$`, `[MSG:...]`, welcome banner `Grbl ...` — is not
      // tied to a queued command, so don't dequeue and don't halt. Log it:
      // a mid-sweep `Grbl ` banner means the MEGA itself reset (brownout /
      // watchdog). That clears GRBL's planner + RX buffer and drops it into
      // alarm, while our bufferFill/queue still think commands are in flight —
      // so every flip we stream afterward is silently discarded by GRBL. That
      // is a gradual, no-MCU-reset way for the tail of the sweep (the top
      // rows) to never execute.
      plog::logf("GRBL other: %.30s", resp.c_str());
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;  // +1 for the newline GRBL counts

  // Nothing is owed to us at the top of a fresh command run, so clear out any
  // orphan fragment the idle line collected before it can glue itself to this
  // command's ack. Costs one available() check on the hot path.
  grblDropIdleNoise();

  // Reset the stall timer every time the buffer actually drains so a
  // long-but-progressing motion sequence doesn't trip the watchdog.
  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
    if (inStartupPhase && grblStartupFault) return;
    if (bufferFill != lastFill) {
      lastFill = bufferFill;
      stallT0 = millis();
    }
    if (millis() - stallT0 > GRBL_STALL_TIMEOUT_MS) {
      // During bring-up a stall is recoverable in place — see waitForIdle.
      if (inStartupPhase) {
        plog::log("GRBL stall in sendGcode (startup) -> bounce + retry");
        grblStartupFault = true;
        return;
      }
      grblStallReset("sendGcode");
    }
  }

  // Send `\n` only, not `\r\n` — grbl-Mega treats `\r` as a line end then
  // acks the trailing `\n` as an empty line, producing a duplicate ok per
  // command. The duplicate desyncs cmdLengths queue accounting.
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmd, cmdLen);
  gCmdsSent++;
}

void waitForIdle() {
  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill > 0) {
    drainResponses();
    if (inStartupPhase && grblStartupFault) return;
    if (bufferFill != lastFill) {
      lastFill = bufferFill;
      stallT0 = millis();
    }
    if (millis() - stallT0 > GRBL_STALL_TIMEOUT_MS) {
      // A stall during bring-up must NOT reset the MCU. grblBringup already
      // has a recovery path for a GRBL that won't answer — bounce Serial1 and
      // retry homing — but until 2026-08-23 the watchdog fired first and
      // rebooted the SoC, so that path was unreachable and the rig degenerated
      // into an 80-second reboot loop instead: 12 boots in 17 minutes, each one
      // re-running WiFi association, NTP sync and lidar init, and each one
      // discarding the accumulated in-RAM state for nothing. A dead Mega is not
      // something the ESP32 can fix by restarting itself.
      //
      // Signalling grblStartupFault instead hands control back to grblBringup,
      // which bounces the UART, backs off, and keeps trying — WiFi stays up,
      // the log stays continuous, and the rig recovers on its own the moment
      // GRBL starts answering again.
      if (inStartupPhase) {
        plog::log("GRBL stall in waitForIdle (startup) -> bounce + retry");
        grblStartupFault = true;
        return;
      }
      grblStallReset("waitForIdle");
    }
  }
}

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Travel to (targetX, targetY) such that any vertical (Y) component happens
// with X pinned to the nearest absolute machine limit (X = 0 or X = -X_TRAVEL).
// Emits pure-X → pure-Y → pure-X, so the Y leg never drags the head across
// the disc area at a non-limit X. Use for entry into a phase or any cross-row
// transition; within-row moves can use plain moveTo (Y is constant there).
void moveToYSafe(float targetX, float targetY) {
  char cmd[40];
  float xLimit = (targetX > -X_TRAVEL / 2.0f) ? 0.0f : -X_TRAVEL;
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", xLimit);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", targetY);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", targetX);
  sendGcode(cmd);
}

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

// (Re-)assert the color-sensor pin config. S0/S1 = HIGH/LOW selects 20% output
// frequency scaling. Also brings up the illumination-LED pin (left off; the
// ambient-subtracted read toggles it per cell).
void initColorSensor() {
  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  pinMode(TCS_LED, OUTPUT);
  digitalWrite(TCS_LED, LOW);
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);  // idle on the channel classifyDisc reads
}

// OUT is a 50%-duty square wave whose frequency tracks light intensity for
// the active filter. pulseIn times one half-period; doubling gives the full
// period. 100 ms timeout keeps a dark / disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Average 5 consecutive RGBC frames at the current head position (2ms-paced),
// at whatever the current illumination is. Used twice per cell by
// readAmbientSubtracted() — once LEDs-off, once LEDs-on.
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);
    delay(2);
    sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN);
    delay(2);
    sg += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR);
    delay(2);
    sc += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);
    delay(2);
    sb += tcsReadFrequencyHz();
  }
  r = sr / 5;
  g = sg / 5;
  b = sb / 5;
  c = sc / 5;
}

// LED settle after toggling the illumination bank before reading.
const int LED_SETTLE_MS = 20;
// Number of LED off/on flashes averaged per cell (each flash's off & on reads
// are themselves 5-frame averages). The class gap is ~10x so one would do, but
// a few flashes buys margin against transient noise for ~0.1s/flash.
const int AMBIENT_FLASHES = 3;

// One ambient-subtracted RGBC read, averaged over AMBIENT_FLASHES off/on
// flashes. Each flash subtracts a 5-frame LEDs-OFF (ambient) read from a 5-frame
// LEDs-ON (lit) read: whatever room light is present shows up in both and
// cancels, so the result depends only on the disc + our own LEDs — immune to the
// ambient-light shifts that used to wreck the scan (the "blown regime").
// Per-flash negatives clamped to 0. LEDs left off.
void readAmbientSubtracted(long& r, long& g, long& b, long& c) {
  long sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < AMBIENT_FLASHES; i++) {
    unsigned long ar, ag, ab, ac, lr, lg, lb, lc;
    digitalWrite(TCS_LED, LOW);  delay(LED_SETTLE_MS); tcsReadRGBC(ar, ag, ab, ac);
    digitalWrite(TCS_LED, HIGH); delay(LED_SETTLE_MS); tcsReadRGBC(lr, lg, lb, lc);
    long dr = (long)lr - (long)ar; if (dr < 0) dr = 0;
    long dg = (long)lg - (long)ag; if (dg < 0) dg = 0;
    long db = (long)lb - (long)ab; if (db < 0) db = 0;
    long dc = (long)lc - (long)ac; if (dc < 0) dc = 0;
    sr += dr; sg += dg; sb += db; sc += dc;
  }
  digitalWrite(TCS_LED, LOW);
  r = sr / AMBIENT_FLASHES; g = sg / AMBIENT_FLASHES;
  b = sb / AMBIENT_FLASHES; c = sc / AMBIENT_FLASHES;
}

// Diagnostic: log per-cell raw RGBC + pre-threshold logit for EVERY scanned
// cell (not just the zero-reading rows). ~666 lines/scan — relies on the raised
// PLOG_MAX_BYTES in persistent_log.cpp. Comment out to log only rows 0,1,2,9,14.
#define SCAN_PX_LOG_ALL

// Sensor head sits offset from the flip actuator on the gantry. X was
// calibrated by jogging the head over the top-left squisk (cell center -752.695,
// head at -776.700 → -24.005); Y = +8 (up 4 mm from the old +4), which needed
// the $131 402→406 headroom bump.
const float SCAN_OFFSET_X = -24.005f;
const float SCAN_OFFSET_Y = 8.0f;

// The sensor sits +SCAN_OFFSET_Y in Y from the flip head, so the top bitmap
// row's scan target is the closest any move gets to the Y=0 soft-limit edge
// (GRBL rejects target > 0 with ALARM:2 under $20=1). With Y_TRAVEL=$131=412
// the top-row scan lands at -412 + 23.40*17 + 8.0 = -6.2 — inside the
// envelope — so keep this clamp as a safety net so future offset/pitch tweaks
// can't silently push a scan target past 0. (History: $131 went 399.695 → 402
// → 406 → 412; at 399.695 the target was +2.105 → deterministic ALARM:2,
// 402→406 gave the sensor 4 mm of top headroom for the +8 offset above, and
// 406→412 gave ScanColorLidarTest's lidar +14 Y offset top-row headroom —
// its top-row target is -14.2 + 14 = -0.2.)
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }

// Disc classification by a SIMPLE THRESHOLD on one ambient-subtracted channel.
// gridState holds the FRONT/displayed state (1 = cyan/on, 0 = black/off). The
// sensor views the BACK of each disc: an ON disc (cyan front) shows its BLACK
// back and reads LOW; an OFF disc (black front) shows its cyan back and reads
// HIGH. So on = channel below the threshold.
//
// !! THE INVARIANT: classifyDisc THRESHOLDS THE `c` SLOT -- TcsFilter value 2,
// !! commanded (S2=H, S3=L). Re-derived 2026-08-22 from full-board ground
// !! truth, and this REVERSES the 2026-08-20 decision to threshold `b`.
// !!
// !! What went wrong: the 2026-08-20 note claimed BLUE separated 4.61x and
// !! CLEAR only 1.60x, from 222 cells of rows 0-5. That is backwards. The
// !! channel it measured as "blue" is the one this code now reads as `c`; the
// !! labels, not the physics, were swapped. Measured on rows 0-5 of the SAME
// !! kind of board, ambient-subtracted at 3V3, against the source bitmap:
// !!     slot `c` (value 2): cyan-front 170..494,  black-front 2137..2992
// !!     slot `b` (value 1): cyan-front 0..2381,   black-front 427..1669
// !! Slot `b`'s populations OVERLAP -- no cut can separate them.
// !!
// !! Full-board ground truth (666 cells, gallery id 68, check-pass scan,
// !! target bitmap decoded from gallery.php), best achievable per slot:
// !!     r: 7 errors   g: 6   b: 14   c: 6
// !! Live, on `b` at 1112: 24 errors. On `c`: 6.
// !!
// !! Corroborated across 40 earlier jobs (ids 26..66, 26,640 cells): the
// !! pre-2026-08-17 firmware thresholded `c` and scored 89/26,640 = 0.33%.
// !! On job 42 it scored 0 errors while the best possible score on `b` was 17
// !! -- proof by contradiction that `b` was never the channel being read.
// !!
// !! Threshold: geometric mean of the clean cluster edges, sqrt(494 * 1651)
// !! = 903 -> 900. The error count is flat (6-7) anywhere from ~700 to ~1400,
// !! so this sits mid-plateau. Separation at 3V3 is 4.93x (black p05 / cyan
// !! p95) against a 5V-era median of 5.52x.
// !!
// !! SUPPLY-DEPENDENT. The 5V->3V3 move on 2026-08-20 scaled every channel
// !! down; if the sensor goes back to 5V this MUST be re-derived. Old logs are
// !! not column-comparable either -- slot semantics have changed twice.
// !!
// !! THIS CONSTANT IS ONLY A COLD-START SEED. Since 2026-08-24 the live cut is
// !! derived from each scan's own two populations -- see chooseClearCut() -- and
// !! the last accepted value is persisted to flash. SCAN_ON_CLEAR_MAX is used
// !! only when there is no value on record yet (first scan after a fresh flash)
// !! AND the current scan cannot supply one.
const long SCAN_ON_CLEAR_MAX = 900;   // ambient-sub `c` < this => cyan/on (front)
static inline uint8_t classifyDisc(long c, long cut) { return (c < cut) ? 1 : 0; }

// ------------------------------------------------- adaptive scan threshold
// WHY THIS EXISTS. A fixed cut has now silently broken the board twice, both
// times because the HARDWARE moved under it while the disc CONTRAST did not:
//   * 2026-08-20, TCS3200 5V -> 3V3: every channel scaled down ~3.5x, so the
//     then-live 3535 sat above BOTH populations and every cell read "cyan".
//   * 2026-08-23: the return signal halved again (black cluster ~2300 -> ~1000)
//     and 900 landed INSIDE the black cluster -- it cut 124 of that scan's 213
//     black cells onto the wrong side, and the draw then "corrected" the board
//     into noise. That scan was NOT degraded: measured with no labels at all it
//     was cleanly bimodal at 4.79x separation. Only the cut was wrong.
//
// The invariant that actually holds across all of it is the RATIO: the two disc
// faces have separated by ~4.8-5.5x in every healthy scan on record, at 5V and
// at 3V3 alike. So derive the cut from the scan instead of pinning it.
//
// BACKTEST (50 check-pass scans with decoded-bitmap ground truth, both supply
// regimes). Error rate, cells misclassified:
//        method            5V (40 jobs)   3V3 (6)   3V3 post-fix (4)
//        fixed 3535           0.33%        47.42%      48.50%
//        fixed 900           22.00%         1.60%       0.23%
//        per-scan Otsu        1.71%         1.75%       0.38%
//        per-scan GAP CUT     1.45%         1.58%       0.15%
//        hybrid (below)  ---- 0.49% over all 50 scans ----
// Each fixed constant is excellent in its own regime and catastrophic in the
// other. The adaptive cut picked ~5500-6900 at 5V and ~800-1450 at 3V3 on its
// own, with no retuning, and beat the hand-tuned constant on recent jobs.
//
// GAP CUT, NOT RAW OTSU, AND A GAP SEED TOO. The cut is the geometric mean of
// the two cluster EDGES (low p95, high p05), not Otsu's variance split: that
// criterion gets dragged around by the ~10x dynamic range, while the gap
// midpoint is what we actually want, and it won in all three regimes above.
// The SEED that splits the two sets is now the widest adjacent ratio as well --
// Otsu seeded there until 2026-08-24, when its class-balance term walked the
// split into the majority cluster on a 36-cyan image and wedged the rig for
// three jobs. Full account and backtest at the seed loop in chooseClearCut().
//
// THE GUARD, AND WHY "TOO LOW -> RESCAN" IS ONLY HALF RIGHT. Adaptive
// thresholding has exactly one catastrophic failure and it is in the data: job
// 56's target bitmap has ZERO cyan cells, so there is no second cluster and the
// algorithm invents a split -- 280 errors against 1 for a carried-forward cut.
// But rescanning cannot help there: the cluster is missing because the IMAGE is
// uniform, not because the sensor is sick, and a rescan returns the same thing.
// Low separation has two causes and they need opposite responses:
//   both clusters populated AND sep >= MIN  -> trust it, adopt and persist
//   both clusters populated AND sep <  MIN  -> populations OVERLAP = sensor
//                                              fault -> rescan, then refuse
//   one cluster sparse                      -> uniform image, normal -> use the
//                                              last accepted cut, do NOT rescan
// The carry-forward is the useful part: the board learns its cut from the most
// recent full-contrast scan, so a sparse image inherits a threshold already
// correct for the current supply and LED brightness, with no constant involved.
const int   SCAN_MIN_CLUSTER = 40;     // cells; below this a cluster is "sparse"
// TUNED BY BACKTEST, do not raise on intuition. Swept over the same 50 scans:
//   MIN_SEP  1.50  1.80  2.00  2.20  2.50  3.00
//   refused     1     1     2     2     4     5
//   err rate 0.53% 0.53% 0.53% 0.53% 0.51% 0.50%
// Accuracy on drawn jobs is FLAT across the whole range while refusals climb,
// so strictness buys ~24 cell-errors and costs 4 refused prints. 1.80 refuses
// exactly one scan in 50 — job 56, whose target bitmap has zero cyan cells and
// which is genuinely undecidable — and passes every scan that was actually fine
// (including four sparse-image 5V scans that sit at 2.26-2.53 and classify
// correctly). It also still sits well clear of every healthy scan on record,
// which measure 4.8-6.2x.
const float SCAN_MIN_SEP     = 1.8f;   // hi_p05 / lo_p95 for a scan to be trusted
const long  SCAN_CUT_MIN     = 60;     // sanity band on any derived cut
const long  SCAN_CUT_MAX     = 20000;

// This scan's raw per-cell reads. Module scope, not stack: 4 x 666 x 4 = ~10.6 KB
// and the loop task's stack is not the place for it. Classification is deferred
// to a second pass because the cut is not known until every cell has been read.
static long scanR[GRID_H][GRID_W], scanG[GRID_H][GRID_W];
static long scanB[GRID_H][GRID_W], scanC[GRID_H][GRID_W];

// Last accepted cut, mirrored from flash. 0 = nothing on record yet.
static long lastGoodCut = 0;
void cutSaveRecord(long cut);          // defined with the other flash records

static int cmpLongAsc(const void* a, const void* b) {
  long x = *(const long*)a, y = *(const long*)b;
  return (x > y) - (x < y);
}

// Derive a cut from scanC[][]. Always fills cut/sep100/nlo/nhi; returns true
// only when both clusters are populated AND separated, i.e. the scan is
// trustworthy enough to adopt its cut as the new last-good.
bool chooseClearCut(long& cutOut, int& sep100Out, int& nloOut, int& nhiOut) {
  const int N = GRID_W * GRID_H;
  static long v[GRID_W * GRID_H];
  int n = 0;
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++) v[n++] = scanC[y][x];
  qsort(v, n, sizeof(long), cmpLongAsc);

  // WIDEST-RATIO-GAP SEED, NOT OTSU. Otsu maximises w*(m2-m1)^2, and that `w`
  // term is a class-BALANCE weight: it collapses toward zero as either side of
  // the split gets small, so on a lopsided picture Otsu will happily walk the
  // split INTO the majority cluster to buy back balance. That is not a corner
  // case, it wedged the rig on 2026-08-24:
  //   job 79 ("Ak") draws 36 cyan cells out of 666. The scan was perfectly
  //   healthy -- cyan p95 = 164, black p05 = 832, a clean 5.07x gap -- but
  //   Otsu seeded ~10 cells deep into the black cluster, so the "low" set came
  //   back as 46 cells with ~10 black ones in it. That contaminated loP95, and
  //   sep collapsed 5.07 -> 1.48. With nlo = 46 >= SCAN_MIN_CLUSTER the sparse
  //   branch below could not catch it either, so scanGrid returned
  //   untrustworthy, runPendingJob refused to draw, and the job was abandoned
  //   after 3 attempts. The board never changes when nothing is drawn, so every
  //   following job re-decided identically: 80 and 81 were abandoned, 82 was
  //   next. Each abandoned job still leaves a gallery entry pending AND still
  //   gets a YouTube recording (next.php arms that at pop time), which is why
  //   those entries showed a video of the PREVIOUS print's board.
  //
  // The gap between two disc faces is multiplicative (~4.8-6.2x on every
  // healthy scan on record), so seed on the widest ADJACENT RATIO instead. It
  // has no balance term, so a 36/630 split is found exactly as well as a
  // 330/336 one. Outer 1% is skipped so a single dark or blown outlier cannot
  // present itself as the gap.
  //
  // BACKTEST, 45 check-pass scans from bug_logs_2/plog_2026-08-24.txt with
  // decoded-bitmap ground truth (gallery.php), same method as SCAN_MIN_SEP:
  //        seed          refused   err rate
  //        Otsu             2       0.43%     (refuses 56 and 79)
  //        widest gap       0       0.36%     (refuses neither)
  // It is strictly better on the two scans that matter and never worse
  // elsewhere: job 79 comes back sep=5.10 nlo=38 (correctly SPARSE, not a
  // fault) and job 27/35, the other two lopsided scans, drop 5->2 and 3->0
  // errors. Result is insensitive to the skip band over 0.5%-5% (identical
  // error counts), so 1% is not a tuned number.
  //
  // The OVERLAP guard still works -- verified by compressing job 49's two
  // populations toward their common mean: sep tracks the compression down
  // (5.89 / 2.81 / 1.98 / 1.58 / 1.20) and trips REFUSE below 1.8 exactly as
  // before. Fixing the seed removed the FALSE positive without removing the
  // real one.
  double bestGap = -1.0;
  long seed = v[n / 2];
  const int band = n / 100;            // skip the outer 1% on each side
  for (int i = (band > 1 ? band : 1); i < n - band; i++) {
    if (v[i] <= v[i - 1] || v[i - 1] < 1) continue;   // no gap / unusable ratio
    double r = (double)v[i] / (double)v[i - 1];
    if (r > bestGap) { bestGap = r; seed = v[i]; }
  }

  int nlo = 0;
  while (nlo < n && v[nlo] < seed) nlo++;
  int nhi = n - nlo;
  nloOut = nlo; nhiOut = nhi;

  if (nlo < 1 || nhi < 1) {            // degenerate: everything on one side
    cutOut = lastGoodCut ? lastGoodCut : SCAN_ON_CLEAR_MAX;
    sep100Out = 0;
    return false;
  }
  long loP95 = v[nlo - 1 - (int)((nlo - 1) * 0.05f)];   // 95th pct of the low set
  long hiP05 = v[nlo + (int)((nhi - 1) * 0.05f)];       //  5th pct of the high set
  if (loP95 < 1) loP95 = 1;
  long cut = (long)sqrt((double)loP95 * (double)hiP05);
  if (cut < SCAN_CUT_MIN) cut = SCAN_CUT_MIN;
  if (cut > SCAN_CUT_MAX) cut = SCAN_CUT_MAX;
  cutOut = cut;
  sep100Out = (int)((100.0f * hiP05) / loP95);
  return (nlo >= SCAN_MIN_CLUSTER && nhi >= SCAN_MIN_CLUSTER &&
          sep100Out >= (int)(SCAN_MIN_SEP * 100));
}

// Re-home + reassert mm/absolute modes. $1=255 lives in EEPROM so it would
// survive on its own, but matching the boot sequence keeps state predictable.
void rehome() {
  sendGcode("$H");
  waitForIdle();
  sendGcode("$1=255");
  waitForIdle();  // sync past the $1 EEPROM write before pipelining more — grbl
                  // disables interrupts during the commit and drops Serial1 RX
                  // bytes (same fault that error:2'd the end-of-job jog).
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
  needsRehome = false;
}

// One full scan sweep: re-home, drop the flip arm to SCAN, sweep the sensor over
// every cell, read the ambient-subtracted BLUE channel and threshold it into
// gridState. (The old blown-regime ceiling / re-init recovery is gone — ambient
// subtraction removes the room-light sensitivity that caused that failure.)
// Serpentine scan order as a flat index 0..GRID_W*GRID_H-1: even rows L→R, odd
// rows R→L (row 0 starts L→R). Lets scanGrid's pipeline refer to "the next cell".
static inline void cellAt(int i, int& y, int& x) {
  y = i / GRID_W;
  int col = i % GRID_W;
  x = (y & 1) ? (GRID_W - 1 - col) : col;
}
static inline float cellScanX(int y, int x) { return grid[y][x].x + SCAN_OFFSET_X; }
static inline float cellScanY(int y, int x) { return clampScanY(grid[y][x].y + SCAN_OFFSET_Y); }

bool scanGrid() {
  rehome();
  // Run the scan sweep with the flip arm dropped to SCAN (~33.5°, 12.5° below
  // RELEASE) rather than parked at REST. The sensor trails the flip head by SCAN_OFFSET_X (−24mm,
  // ~one cell pitch), so the lowered arm brushes the whole board over the
  // serpentine and pushes through any squisk accidentally left at stage 1 (90°,
  // half-rotated). The HOMING moves keep the arm at REST — the initial rehome()
  // above runs before this drop (the caller always enters scanGrid with the arm
  // parked), and the mid-scan rehome below lifts it first — because a dropped
  // arm dragged diagonally across the populated board is what snapped the flip
  // arm before. Parked back at REST once the scan completes.
  writeServoUs(SERVO_US_SCAN, SERVO_50_DEG_SETTLE_MS);
  // Serpentine top-to-bottom (alternating row direction so cross-row Y travel is
  // at an X soft-limit via moveToYSafe). PIPELINED to hide the flash-log writes
  // behind GRBL motion: at each cell we (1) sense while stationary, (2) START the
  // move to the NEXT cell (sendGcode hands the short G0(s) straight to GRBL, so
  // it's already moving), then (3) write this cell's px/row log WHILE that move
  // runs, (4) waitForMotion before sensing the next cell. Sensing still happens
  // only when the head is stationary; only the plog write overlaps the travel.
  const int N = GRID_W * GRID_H;
  int y0, x0;
  cellAt(0, y0, x0);
  moveToYSafe(cellScanX(y0, x0), cellScanY(y0, x0));
  waitForMotion();
  for (int i = 0; i < N; i++) {
    int y, x;
    cellAt(i, y, x);

    // 1. Sense this cell (stationary). Classification is DEFERRED: the cut is
    //    derived from the whole scan's two populations, which do not exist yet.
    long r, g, b, c;
    readAmbientSubtracted(r, g, b, c);
    scanR[y][x] = r; scanG[y][x] = g; scanB[y][x] = b; scanC[y][x] = c;
    bool last = (i == N - 1);

    // 2. Start the move to the next cell (non-blocking — GRBL begins moving).
    if (!last) {
      int ny, nx;
      cellAt(i + 1, ny, nx);
      if (ny == y) {
        moveTo(cellScanX(ny, nx), cellScanY(ny, nx));       // intra-row (pure X)
      } else {
        // Re-home after row 8 so accumulated step drift can't skew the rest of
        // the scan. Lift the arm to REST first (a dropped arm dragged across the
        // populated board snapped it before), drop back to SCAN after.
        if (y == 8) {
          writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
          rehome();
          writeServoUs(SERVO_US_SCAN, SERVO_50_DEG_SETTLE_MS);
        }
        moveToYSafe(cellScanX(ny, nx), cellScanY(ny, nx));  // inter-row edge legs
      }
    }

    // 3. Ensure the move finished before sensing the next cell. The px logging
    //    that used to fill this slot (hiding flash writes behind GRBL travel)
    //    now runs after the sweep, because a cell's verdict is not known until
    //    the cut is. That costs a few seconds of flash writes at the end of a
    //    ~17 min scan, and the log format is unchanged.
    if (!last) waitForMotion();
  }
  // Scan done (and any stage-1 squisks swept through) — park the flip arm at REST.
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);

  // --- derive this scan's cut from its own two populations -------------------
  long cut = 0; int sep100 = 0, nlo = 0, nhi = 0;
  const bool clean = chooseClearCut(cut, sep100, nlo, nhi);
  const bool populated = (nlo >= SCAN_MIN_CLUSTER && nhi >= SCAN_MIN_CLUSTER);
  long useCut;
  const char* mode;
  bool trustworthy = true;
  if (clean) {
    useCut = cut; mode = "adaptive";
    if (cut != lastGoodCut) { lastGoodCut = cut; cutSaveRecord(cut); }
  } else if (populated) {
    // Both clusters exist but they OVERLAP — the sensor is not separating the
    // faces any more. Drawing on this would scramble the board, which is
    // exactly what happened on 2026-08-23. Caller re-scans, then gives up.
    useCut = lastGoodCut ? lastGoodCut : SCAN_ON_CLEAR_MAX;
    mode = "OVERLAP"; trustworthy = false;
  } else {
    // One cluster is sparse: a near-uniform image. Perfectly normal — there is
    // simply nothing to split, so carry the last accepted cut forward. Do NOT
    // rescan; the missing cluster is a property of the picture, not the sensor.
    useCut = lastGoodCut ? lastGoodCut : SCAN_ON_CLEAR_MAX;
    mode = "sparse";
  }
  plog::logf("scan cut: %s t=%ld sep=%d.%02d nlo=%d nhi=%d used=%ld",
             mode, cut, sep100 / 100, sep100 % 100, nlo, nhi, useCut);

  // --- classify + log, in scan order so the log reads exactly as before ------
  int rowOn = 0;
  for (int i = 0; i < N; i++) {
    int y, x;
    cellAt(i, y, x);
    uint8_t on = classifyDisc(scanC[y][x], useCut);
    gridState[y][x] = on;
    rowOn += on;
    bool rowEnd = (i == N - 1) || ((i + 1) % GRID_W == 0);
#ifdef SCAN_PX_LOG_ALL
    const bool logpx = true;
#else
    const bool logpx = (y == 0 || y == 1 || y == 2 || y == 9 || y == 14);
#endif
    if (logpx) plog::logf("px y%dc%d r%ld g%ld b%ld c%ld on%d", y, x,
                          scanR[y][x], scanG[y][x], scanB[y][x], scanC[y][x], (int)on);
    if (rowEnd) { plog::logf("scan y=%d on=%d", y, rowOn); rowOn = 0; }
  }

  // An untrustworthy scan must not be carried into the next job as a "measured"
  // picture of the board, and must not be re-used to skip the next scan.
  gridStateFresh = trustworthy;
  gridStateFromScan = trustworthy;
  return trustworthy;
}

// `G4 P0` is a dwell that GRBL syncs through the planner before acking, so
// once its `ok` lands every queued motion has actually finished — not just
// been planned. Pair with waitForIdle() before any non-GRBL action.
void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Two-stage 180° flip, plus a second error-reduction catch pass:
//   1) Servo to ENGAGE (75°) above the disc — this rotates the squisk 90°.
//      Back off FLIP_UNLOAD_X (opposite the coming release stroke) to take the
//      contact load off the arm, then servo back to REST. The squisk is now
//      half-flipped and stays put.
//   2) Slide X by ±16.8 mm (plus the unload back) so the arm clears the disc
//      column, drop the arm to the lidar-COMPENSATED RELEASE (~25.5° base), then
//      slide back the other way by dx + FLIP_CATCH_EXTRA_X so the arm sweeps
//      PAST the cell origin, catching the half-rotated squisk and pushing it
//      through the final 90°.
//   3) Second catch (compile-time optional): drop the arm a further ~10°
//      (RELEASE2) and sweep once more in the +dx direction (opposite the return)
//      over ≥16.8 mm, to push back any disc the first catch left
//      over/under-rotated.
//
// `inverted` mirrors the whole X excursion: dx = −FLIP_OFFSET_X and the flip
// target shifts right by FLIP_INVERT_OFFSET_X (see that constant). Pass it for
// LEFT-TO-RIGHT sweep rows: their +X return then ends the way the sweep is
// already heading (no backtrack), and half the rows unwind the column-rod twist
// the other half wind in. RTL rows keep the original flip, whose −X return
// already points along their sweep.
//
// `catchByNextMove` used to let the caller fold step 3 into a move it was
// already making. That's no longer possible: the step-3 sweep is always
// OPPOSITE the return slide, and under the mirror the return slide runs with
// the sweep on both row directions — so step 3 always runs against the sweep
// and never matches the next move. Callers pass false and flipDisc emits its
// own +dx stroke and re-parks at REST. The parameter is kept so re-enabling
// FLIP_SECOND_CATCH doesn't need a signature change.
void flipDisc(int gx, int gy, bool catchByNextMove, bool inverted) {
  // Skew-corrected flip X for this row (see flipSkewX), plus the mirrored-flip
  // shift on inverted rows. The Y excursions below are all relative (G91), so
  // only the absolute X target shifts.
  float fx = grid[gy][gx].x + FLIP_TARGET_OFFSET_X + flipSkewX(gy)
            + (inverted ? FLIP_INVERT_OFFSET_X : FLIP_NONINVERT_OFFSET_X);
  moveTo(fx, grid[gy][gx].y);
  waitForMotion();

  // Cap the X excursion so the repositioning slide and the matching finish
  // slide never command a position outside the work area (X=0 on the positive
  // side, −X_TRAVEL on the negative — inverted (LTR) rows slide toward the
  // latter, plain (RTL) rows toward the former).
  // With the current grid this never binds (worst-case margins are 12.77 mm at
  // X=0 and 19.70 mm at −X_TRAVEL, over all strokes); it's the safety net for
  // future offset, pitch, or skew changes.
  //
  // Hoisted above the ENGAGE call because the unload move needs its sign.
  float dx = inverted ? -FLIP_OFFSET_X : FLIP_OFFSET_X;
  if (fx + dx > 0.0f) dx = -fx;
  if (fx + dx < -X_TRAVEL) dx = -X_TRAVEL - fx;

  // Signed unload, opposite the release stroke. Skipped (left at 0) when the
  // feature is off, when the release stroke was clamped to nothing, or when
  // backing off would breach either X soft limit (the unload runs opposite
  // the release stroke, so on inverted rows it heads toward X=0).
  float unloadX = usesEdgeUnload(gx, inverted) ? FLIP_UNLOAD_X_EDGE : FLIP_UNLOAD_X;
  float unload = 0.0f;
  if (unloadX > 0.0f && dx != 0.0f) {
    float sign = (dx >= 0.0f) ? 1.0f : -1.0f;
    float offX = fx - sign * unloadX;  // where the unload move lands
    if (offX >= -X_TRAVEL && offX <= 0.0f) {
      unload = sign * unloadX;
    }
  }

  // Per-cell compensated release angle (lidar standoff + global reach trim +
  // edge-row trim). ENGAGE stays uncompensated — see SERVO_US_ENGAGE.
  const int relUs = compensatedUs(SERVO_US_RELEASE, gx, gy);

  char cmd[32];

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);  // fixed 75°, not compensated

  // Unload the arm from the squisk, while it is still at ENGAGE.
  if (unload != 0.0f) {
    sendGcode("G91");
    snprintf(cmd, sizeof(cmd), "G0 X%.3f", -unload);
    sendGcode(cmd);
    sendGcode("G90");
    waitForMotion();
  }

  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  // Release stroke — travels the unload back plus the usual dx, so the arm
  // lands at the same absolute X (cell origin + dx) as an unload-free flip.
  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx + unload);
  sendGcode(cmd);
  sendGcode("G90");
  // Write this cell's record WHILE the release stroke runs. sendGcode hands the
  // move straight to GRBL, so the flash write is hidden behind carriage travel
  // -- the same pipelining scanGrid uses. unload/dx are logged in tenths of a
  // mm as ints (no %f in the log path).
  plog::logf("f x%d y%d %s rel%d unl%d dx%d miss%lu", gx, gy,
             inverted ? "L" : "R", relUs, (int)(unload * 10.0f),
             (int)(dx * 10.0f), servoAckMisses);
  waitForMotion();

  writeServoUs(relUs, SERVO_50_DEG_SETTLE_MS);

  // Catch stroke: -dx plus FLIP_CATCH_EXTRA_X further in the SAME direction, so
  // the arm sweeps past the cell origin rather than stopping on it. Clamped
  // against both soft limits — the start of this stroke is at fx + dx.
  float catchDx = -dx - ((dx >= 0.0f) ? FLIP_CATCH_EXTRA_X : -FLIP_CATCH_EXTRA_X);
  {
    const float startX = fx + dx;
    if (startX + catchDx > 0.0f)        catchDx = -startX;
    if (startX + catchDx < -X_TRAVEL)   catchDx = -X_TRAVEL - startX;
  }
  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", catchDx);
  sendGcode(cmd);
  sendGcode("G90");
  waitForMotion();

#ifdef FLIP_SECOND_CATCH
  // Step 3 — second catch pass. Drop the arm ~10° below RELEASE first either
  // way; the difference is only whether we emit our own +X stroke.
  writeServoUs(compensatedUs(SERVO_US_RELEASE2, gx, gy), SERVO_10_DEG_SETTLE_MS);
  if (!catchByNextMove) {
    // Same caps so the +dx stroke never commands past either soft limit.
    float dx2 = inverted ? -FLIP_OFFSET_X : FLIP_OFFSET_X;
    if (fx + dx2 > 0.0f) dx2 = -fx;
    if (fx + dx2 < -X_TRAVEL) dx2 = -X_TRAVEL - fx;
    sendGcode("G91");
    snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx2);
    sendGcode(cmd);
    sendGcode("G90");
    waitForMotion();
    writeServoUs(SERVO_US_REST, SERVO_10_DEG_SETTLE_MS);
  }
#else
  // Second catch disabled — there's no extra pass to leave the arm down for, so
  // park it at REST regardless of catchByNextMove. The main flip+catch stands
  // alone and the caller's next move runs with the arm parked.
  (void)catchByNextMove;
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
#endif
}

uint8_t bitmapBit(const uint8_t* bitmap, int x, int y) {
  int idx = y * GRID_W + x;
  return (bitmap[idx / 8] >> (7 - (idx % 8))) & 1;
}

// Check-pass tolerance. Two short-circuits hang off this threshold (see loop()):
//   1. If the first displayBitmap() flipped this few cells or fewer, the job was
//      tiny — few chances to fail — so skip the whole check pass (no re-scan).
//   2. Otherwise re-scan; only re-flip if MORE than this many cells are still
//      wrong. The color sensor is ~99.5% accurate, so a full 666-cell scan
//      misreads ~3 cells on average — a handful of mismatches sits within that
//      noise floor and is more likely a misread than a real mechanical miss.
//      Re-flipping on a misread flips a *correct* disc the wrong way, so
//      tolerating <=5 doesn't lower display accuracy; it avoids corrupting good
//      cells that the noisy re-scan only thinks are wrong.
const int CHECK_FIX_MAX_SKIP = 5;

// Count cells whose desired bit differs from the current gridState[] — i.e. how
// many discs are "wrong" relative to the target bitmap. Used to gate the check
// pass's corrective re-flip.
int countMismatches(const uint8_t* bitmap) {
  int n = 0;
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) n++;
    }
  }
  return n;
}

// Bottom-to-top (bitmap y = GRID_H-1 → 0) over the band of rows containing at
// least one flip. Rows with no differing cells are skipped entirely — we don't
// even move to them; the next flipping row is entered via moveToYSafe regardless,
// so jumping over an empty row keeps the only Y travel at an X soft-limit.
// Flipping rows alternate sweep direction (serpentine) so consecutive entries
// land on the same side. Returns the number of cells flipped (= cells that were
// wrong vs the target), which the caller uses to gate the check pass.
int displayBitmap(uint8_t* bitmap) {
  int firstY = -1;  // bottom-most changing row (largest bitmap-y)
  int lastY = -1;   // top-most changing row (smallest bitmap-y)
  for (int y = GRID_H - 1; y >= 0; y--) {
    for (int x = 0; x < GRID_W; x++) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) {
        if (firstY < 0) firstY = y;
        lastY = y;
        break;
      }
    }
  }
  if (firstY < 0) return 0;
  plog::logf("dB band y=%d..%d", lastY, firstY);
  // From here on we will move discs, so gridState[] stops being a measured scan
  // and becomes what we *believe* the flips achieved. Cleared before the first
  // flip so an abort mid-sweep can't leave the stale "measured" claim standing.
  gridStateFromScan = false;

  int flipped = 0;
  bool ltr = true;
  for (int y = firstY; y >= lastY; y--) {
    // Per-row diff count, logged so a plog dump shows exactly how many flips
    // each row attempts. Reading the dump against the known target:
    //   * row visibly still on the OLD image but logged diff=0  -> the flip was
    //     never queued because gridState already matched the target here, i.e.
    //     a scan misread (skip happens AT the diff, gridState is wrong).
    //   * row logged the expected diff but didn't change physically -> it WAS
    //     attempted and the flip itself failed (botch / dropped GRBL move).
    // This pins skip-vs-botch per row without guessing.
    int rowFlips = 0;
    for (int x = 0; x < GRID_W; x++) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) rowFlips++;
    }
    // Snapshot at row entry (the previous row's waitForMotion has drained, so
    // buf/qd should be ~0 here). buf or qd stuck high, or snt running away from
    // ack as the sweep climbs, means GRBL stopped consuming our stream — the
    // top rows (streamed last) would then never execute even though diff>0.
    plog::logf("dB y=%d diff=%d buf=%d qd=%d snt=%lu ack=%lu", y, rowFlips,
               bufferFill, (qTail - qHead + QUEUE_SIZE) % QUEUE_SIZE,
               gCmdsSent, gOksAcked);
    if (rowFlips == 0) continue;  // no changes — don't move to this row at all

    int startCol = ltr ? 0 : GRID_W - 1;
    int endCol = ltr ? GRID_W - 1 : 0;
    int step = ltr ? +1 : -1;

    // Enter this row (or transition from the previous flipping row) at an X
    // soft-limit so the Y leg never drags the head across populated discs.
    moveToYSafe(grid[y][startCol].x, grid[y][startCol].y);

    // Collect the columns needing a flip in sweep order.
    int flipCols[GRID_W];
    int nFlips = 0;
    for (int x = startCol; x != endCol + step; x += step) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) flipCols[nFlips++] = x;
    }
    for (int i = 0; i < nFlips; i++) {
      int x = flipCols[i];
      // LTR rows get the mirrored flip: its return stroke runs +X, the way the
      // sweep is already heading, so the head doesn't backtrack before the next
      // cell. RTL rows keep the original flip, whose -X return already points
      // along their sweep. See FLIP_INVERT_OFFSET_X.
      //
      // The second catch always runs opposite the return stroke, so under the
      // mirror it never lines up with the caller's next move — it can no longer
      // be folded in on either row direction.
      const bool catchByNextMove = false;
      flipDisc(x, y, catchByNextMove, ltr);
      gridState[y][x] = bitmapBit(bitmap, x, y);
    }
    flipped += nFlips;
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    ltr = !ltr;
  }
  plog::logf("dB end flipped=%d buf=%d snt=%lu ack=%lu", flipped, bufferFill,
             gCmdsSent, gOksAcked);
  return flipped;
}

// ============================================================ daily cadence
// Three things live here, and they only make sense together:
//   * a real wall clock (NTP over the WiFi link the poller already keeps up),
//   * a daily VL53L4CD standoff scan of all 666 cells at 10:00 local, whose
//     results + completion date are written to flash so a reboot doesn't
//     re-run (or skip) it,
//   * a night sleep from 20:00 to 10:00 during which the rig ingests nothing.
//
// The board is assumed to be powered 24/7, so this is a *schedule*, not a
// power-management feature — nothing here uses deep sleep. Deep sleep would
// reboot the SoC on wake, which would show up as ESP_RST_DEEPSLEEP in the
// reset-cause breadcrumb and re-run the whole homing sequence, and the whole
// point of that breadcrumb is to make an unexpected reset mean something. The
// night wait is an ordinary delay() loop: the loop task yields to FreeRTOS in
// delay(), exactly as the existing 10-minute post-display linger already does.

// US Mountain with DST, in POSIX TZ form (M3.2.0 = 2nd Sunday in March,
// M11.1.0 = 1st Sunday in November). Named so a move to another timezone is a
// one-line change; the rest of the cadence works in local broken-down time.
const char* PAR_TZ = "MST7MDT,M3.2.0,M11.1.0";
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

// Sanity floor for "the clock has actually been set". The ESP32 RTC starts at
// the epoch, and SNTP fills it in asynchronously some seconds after WiFi comes
// up — so time() is READABLE long before it is TRUE. Every cadence decision is
// gated on this: with a garbage clock we must not scan, must not sleep, and
// must not mark a day done. 2025-01-01 UTC is comfortably before this firmware
// could ever run and comfortably after 1970.
const time_t TIME_VALID_FLOOR = 1735689600;

// Daily schedule, local time. The lidar scan runs at LIDAR_SCAN_HOUR; night
// sleep covers [NIGHT_START_HOUR, LIDAR_SCAN_HOUR), so waking always lands on
// the scan check.
const int LIDAR_SCAN_HOUR  = 10;   // 10:00 am
const int NIGHT_START_HOUR = 20;   // 8:00 pm
// A scan still owed for today is only STARTED before this hour. The sweep runs
// ~50 min, and a late boot shouldn't spend the evening scanning and then print
// into the night — at/after 18:00 the rig sleeps to 10:00 and scans then.
const int LIDAR_SCAN_CUTOFF_HOUR = 18;   // 6:00 pm

static bool timeConfigured = false;

// HARD RULE: time comes from a TIME SERVER, never from the internal clock
// alone. The RTC only interpolates between SNTP fixes, so a trusted clock
// requires (a) at least one fix this boot and (b) a fix recent enough that the
// interpolation is still anchored. onNtpSync() stamps every fix SNTP lands;
// clockValid() enforces both. Without a valid clock the rig NEVER MOVES — no
// polling, no scanning, not even homing (see cadenceGate / grblBringup).
static volatile time_t lastNtpSyncEpoch = 0;
static void onNtpSync(struct timeval*) { lastNtpSyncEpoch = time(nullptr); }
// Max fix age. Must exceed the longest legitimate NTP-less stretch — the 16 h
// past-cutoff sleep with WiFi dropped — with margin; 24 h keeps "one missed
// nightly re-sync" from parking the rig, while a router dead for a day does
// park it (that is the intent: no fresh server time, no motion).
const time_t CLOCK_MAX_SYNC_AGE_S = 24L * 3600L;

bool clockValid() {
  time_t now = time(nullptr);
  if (lastNtpSyncEpoch == 0) return false;          // never synced this boot
  if (now <= TIME_VALID_FLOOR) return false;        // clock is garbage
  if (now - lastNtpSyncEpoch > CLOCK_MAX_SYNC_AGE_S) return false;  // fix too old
  return true;
}

// Start SNTP. Idempotent — configTzTime() is cheap but re-running it restarts
// the sync, so it is guarded. Called once WiFi is up (in setup and at the top
// of loop); SNTP then re-polls on its own and survives a WiFi bounce. The
// notification callback must be registered BEFORE the first sync can land.
void timeBegin() {
  if (timeConfigured) return;
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(PAR_TZ, NTP_SERVER_1, NTP_SERVER_2);
  timeConfigured = true;
  plog::logf("ntp start tz=%s", PAR_TZ);
}

// Local broken-down time, or false unless the clock is currently trusted
// (synced from a time server, recently — see clockValid). EVERY cadence and
// motion-gating path goes through this.
bool localNow(struct tm& out) {
  if (!clockValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

// Bounded wait for the first SNTP fix, used once at boot so the first gate
// pass has a clock to look at. Bounded because a dead network must not wedge
// setup() forever — but failure here does NOT unlock anything: with no valid
// clock the loop holds parked (no motion, no polling) and just keeps retrying.
bool timeWaitForSync(unsigned long timeout_ms) {
  unsigned long t0 = millis();
  struct tm t;
  while (millis() - t0 < timeout_ms) {
    if (localNow(t)) {
      plog::logf("ntp synced %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
      return true;
    }
    delay(500);
  }
  plog::logf("ntp NOT synced after %lu ms - rig holds parked", timeout_ms);
  return false;
}

// 20:00 .. 09:59 local. Written as one predicate so the post-print check and
// the boot-time check can never disagree about where the night boundary is.
static inline bool isNightHour(int hour) {
  return hour >= NIGHT_START_HOUR || hour < LIDAR_SCAN_HOUR;
}

// ---------------------------------------------------------------- lidar scan
// VL53L4CD ToF ranger on the Nano's fixed I2C pins (A4/A5), ported from
// ScanColorLidarTest: free-running at a 50 ms timing budget, one 4 s window per
// cell reduced to a 20%-trimmed mean in tenths of a mm.
//
// RAW, exactly like the pass-4 calibration run (that sketch's
// LIDAR_APPLY_CALIBRATION 0): no back-colour offset is applied here. The
// colour-dependent offsets are only meaningful against a same-pass colour
// classification, and this scan does not sweep the colour sensor — so we store
// what the sensor said and leave the correction to the offline fit that
// produces LIDAR_FIT_CMM. Storing a half-applied correction would be worse
// than storing none.
VL53L4CD lidar;
static bool lidarReady = false;

const unsigned long LIDAR_WINDOW_MS = 4000;   // ~80 samples at the 50 ms budget
const int LIDAR_MAX_SAMPLES = 100;            // cap; 4 s / 50 ms ≈ 80 + slack
const int LIDAR_INIT_ATTEMPTS = 10;

// Head offsets, from ScanColorLidarTest. The lidar sits ~55 mm right of and
// 6 mm above the colour sensor; LIDAR_OFFSET_X is calibrated so col 36 (cell
// x = -31.075) targets exactly the X=0 machine limit.
const float LIDAR_OFFSET_X = 31.075f;
const float LIDAR_OFFSET_Y = SCAN_OFFSET_Y + 6.0f;   // +14.0

// X clamp, the mirror of clampScanY. A no-op at the current calibration (col 36
// lands exactly on X=0, which GRBL allows) — it is the safety net for future
// offset tweaks, same role SCAN_Y_MAX plays on the other axis.
const float SCAN_X_MAX = 0.0f;
static inline float clampScanX(float x) { return x > SCAN_X_MAX ? SCAN_X_MAX : x; }
static inline float lidarTargetX(int y, int x) { return clampScanX(grid[y][x].x + LIDAR_OFFSET_X); }
static inline float lidarTargetY(int y, int x) { return clampScanY(grid[y][x].y + LIDAR_OFFSET_Y); }

// Bring the ranger up. Boot init is flaky (it failed roughly half of observed
// power-ons on the bench, then ran a full 71-minute pass flawlessly once up),
// so the I2C bus is re-inited between attempts.
//
// UNLIKE ScanColorLidarTest, a permanent failure does NOT halt: this is the
// production firmware and a dead ranger must not stop the rig printing. The
// caller records the failure in the day's record and carries on.
bool lidarEnsure() {
  if (lidarReady) return true;
  Wire.begin();
  Wire.setClock(400000);  // 400 kHz fast mode
  lidar.setTimeout(500);
  for (int attempt = 1; attempt <= LIDAR_INIT_ATTEMPTS; attempt++) {
    if (lidar.init()) {
      lidar.setRangeTiming(50, 0);   // 50 ms budget, back-to-back (free-running)
      lidar.startContinuous();
      lidarReady = true;
      plog::logf("lidar ready (attempt %d)", attempt);
      return true;
    }
    plog::logf("lidar init failed (attempt %d)", attempt);
    Wire.end();
    delay(1000);
    Wire.begin();
    Wire.setClock(400000);
  }
  plog::log("lidar init FAILED - scan skipped, rig continues");
  return false;
}

// One per-cell distance estimate: collect blocking reads of the free-running
// ranger for LIDAR_WINDOW_MS, then reduce with ScanColorLidarTest's estimator —
// sort, drop the top and bottom 20%, average the rest. Result in TENTHS of a mm
// (per-cell noise is sub-mm, so whole-mm rounding would quantize the fit).
// Returns the sample count; 0 means every read timed out.
// Caller must have the head stationary — the window starts fresh here.
int lidarWindowRead(uint32_t& avg10, uint16_t& mn, uint16_t& mx) {
  static uint16_t s[LIDAR_MAX_SAMPLES];   // scratch, overwritten every call
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < LIDAR_WINDOW_MS) {
    uint16_t raw = lidar.read();
    if (lidar.timeoutOccurred()) continue;
    if (n < LIDAR_MAX_SAMPLES) s[n++] = raw;
  }
  if (n == 0) { avg10 = 0; mn = mx = 0; return 0; }
  for (int i = 1; i < n; i++) {           // insertion sort
    uint16_t v = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  int trim = n / 5;                       // 20% off each end
  int lo = trim, hi = n - trim;
  if (hi <= lo) { lo = 0; hi = n; }
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += s[i];
  avg10 = (sum * 10 + (uint32_t)(hi - lo) / 2) / (uint32_t)(hi - lo);
  mn = s[0];
  mx = s[n - 1];
  return n;
}

// ------------------------------------------------------- cadence flash record
// (struct LidarScanRecord itself is declared near the top of the file, above
// the first function — see the note there.)
#define CADENCE_PATH "/cadence.bin"
#define CADENCE_TMP  "/cadence.tmp"
const uint32_t CADENCE_MAGIC   = 0x5041524CUL;  // 'PARL'
// v1 -> v2 on 2026-08-25 added the rowsDone/progYear/progYday checkpoint. The
// version bump makes cadenceLoadRecord reject any v1 record rather than
// misreading its bytes; the cost is one extra sweep on the first boot after the
// upgrade, and only the dist10 capture is lost -- which the flash log's `ld`
// lines already carry verbatim.
const uint16_t CADENCE_VERSION = 2;

// Module-level (not a stack local): ~1.4 KB, and the loop task's stack is not
// the place for it. Doubles as the in-RAM cache of what is on flash.
static LidarScanRecord cadenceRec;
static bool cadenceRecValid = false;   // cadenceRec mirrors a good on-flash record

static uint32_t cadenceChecksum(const LidarScanRecord& r) {
  const uint8_t* p = (const uint8_t*)&r;
  size_t n = sizeof(LidarScanRecord) - sizeof(r.checksum);
  uint32_t h = 2166136261UL;           // FNV-1a, plenty for a torn-write check
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

// LittleFS is already mounted by plog::begin(); this call is the guard for the
// case where that mount FAILED (plog then silently no-ops, and we would happily
// write to nothing). LittleFSFS::begin() returns true immediately when the
// label is already mounted, so re-calling it is free and cannot disturb plog.
bool cadenceFsReady() {
  static bool checked = false, ok = false;
  if (!checked) {
    checked = true;
    ok = LittleFS.begin(true, "/littlefs", 10, "ffat");
    if (!ok) plog::log("cadence: LittleFS mount FAILED - schedule is RAM-only");
  }
  return ok;
}

// Load the record into cadenceRec. Anything unexpected (missing, short, wrong
// magic/version/geometry, bad checksum) leaves cadenceRecValid false, which
// simply means "no scan on record" — the scan then runs, which is the safe
// direction to fail in.
void cadenceLoadRecord() {
  cadenceRecValid = false;
  if (!cadenceFsReady()) return;
  File f = LittleFS.open(CADENCE_PATH, "r");
  if (!f) { plog::log("cadence: no scan record on flash"); return; }
  if (f.size() != sizeof(LidarScanRecord)) {
    plog::logf("cadence: record size %u != %u, ignoring",
               (unsigned)f.size(), (unsigned)sizeof(LidarScanRecord));
    f.close();
    return;
  }
  size_t got = f.read((uint8_t*)&cadenceRec, sizeof(LidarScanRecord));
  f.close();
  if (got != sizeof(LidarScanRecord) ||
      cadenceRec.magic != CADENCE_MAGIC ||
      cadenceRec.version != CADENCE_VERSION ||
      cadenceRec.gridW != GRID_W || cadenceRec.gridH != GRID_H ||
      cadenceRec.checksum != cadenceChecksum(cadenceRec)) {
    plog::log("cadence: scan record corrupt/foreign, ignoring");
    return;
  }
  cadenceRecValid = true;
  plog::logf("cadence: record y%d d%d ok%lu sensor%d rows%u",
             (int)(cadenceRec.year + 1900), (int)cadenceRec.yday,
             (unsigned long)cadenceRec.cellsOk, (int)cadenceRec.sensorOk,
             (unsigned)cadenceRec.rowsDone);
}

// Write cadenceRec through a temp file, then swap — same crash policy as
// plog's rotation. A brownout mid-swap can lose the record, and losing it costs
// exactly one extra scan.
void cadenceSaveRecord() {
  // The RAM copy is complete and coherent no matter what the flash write does,
  // so it is marked valid UP FRONT — that is the RAM-only fallback (review
  // F6): with a broken FS the schedule still knows today's scan ran, and the
  // failure costs one extra scan after the next reboot instead of a 50-minute
  // sweep on every gate pass all day.
  cadenceRec.checksum = cadenceChecksum(cadenceRec);
  cadenceRecValid = true;
  if (!cadenceFsReady()) return;
  File t = LittleFS.open(CADENCE_TMP, "w");
  if (!t) { plog::log("cadence: record open failed (RAM-only until reboot)"); return; }
  size_t wrote = t.write((const uint8_t*)&cadenceRec, sizeof(LidarScanRecord));
  t.close();
  if (wrote != sizeof(LidarScanRecord)) {
    plog::logf("cadence: record short write %u (RAM-only until reboot)", (unsigned)wrote);
    LittleFS.remove(CADENCE_TMP);
    return;
  }
  LittleFS.remove(CADENCE_PATH);
  LittleFS.rename(CADENCE_TMP, CADENCE_PATH);
  plog::logf("cadence: record saved y%d d%d ok%lu rows%u",
             (int)(cadenceRec.year + 1900), (int)cadenceRec.yday,
             (unsigned long)cadenceRec.cellsOk, (unsigned)cadenceRec.rowsDone);
}

// ------------------------------------------------- last-good cut flash record
// The adaptive cut survives reboots so a fresh boot does not have to fall back
// to the compile-time seed. Same crash policy as cadenceSaveRecord: temp file
// then rename, and a failed write only costs the seed on the next cold start.
#define CUT_PATH "/clearcut.bin"
#define CUT_TMP  "/clearcut.tmp"
const uint32_t CUT_MAGIC   = 0x50415243UL;  // 'PARC'
const uint16_t CUT_VERSION = 1;

static uint32_t cutChecksum(const ClearCutRecord& r) {
  const uint8_t* p = (const uint8_t*)&r;
  size_t n = sizeof(ClearCutRecord) - sizeof(r.checksum);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

// Anything unexpected leaves lastGoodCut at 0, which just means "no cut on
// record" — the first scan then supplies one, or the seed is used. Safe.
void cutLoadRecord() {
  lastGoodCut = 0;
  if (!cadenceFsReady()) return;
  File f = LittleFS.open(CUT_PATH, "r");
  if (!f) { plog::log("cut: none on flash, seeding from SCAN_ON_CLEAR_MAX"); return; }
  ClearCutRecord r;
  if (f.size() != sizeof(r)) { f.close(); plog::log("cut: record size mismatch, ignoring"); return; }
  size_t got = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (got != sizeof(r) || r.magic != CUT_MAGIC || r.version != CUT_VERSION ||
      r.checksum != cutChecksum(r) || r.cut < SCAN_CUT_MIN || r.cut > SCAN_CUT_MAX) {
    plog::log("cut: record corrupt/out of range, ignoring");
    return;
  }
  lastGoodCut = r.cut;
  plog::logf("cut: loaded last-good %ld", lastGoodCut);
}

void cutSaveRecord(long cut) {
  if (!cadenceFsReady()) return;         // RAM-only until reboot, which is fine
  ClearCutRecord r;
  r.magic = CUT_MAGIC; r.version = CUT_VERSION; r.pad = 0;
  r.cut = (int32_t)cut;
  r.checksum = cutChecksum(r);
  File t = LittleFS.open(CUT_TMP, "w");
  if (!t) { plog::log("cut: save open failed (RAM-only until reboot)"); return; }
  size_t wrote = t.write((const uint8_t*)&r, sizeof(r));
  t.close();
  if (wrote != sizeof(r)) {
    plog::log("cut: short write (RAM-only until reboot)");
    LittleFS.remove(CUT_TMP);
    return;
  }
  LittleFS.remove(CUT_PATH);
  LittleFS.rename(CUT_TMP, CUT_PATH);
  plog::logf("cut: saved %ld", cut);
}

static inline bool lidarScanDoneOn(const struct tm& t) {
  return cadenceRecValid && cadenceRec.year == t.tm_year && cadenceRec.yday == t.tm_yday;
}

// One full lidar standoff sweep of all 666 cells, then persist.
//
// Motion conventions are the same ones scanGrid() obeys, and for the same
// reasons: the flip arm is parked at REST for the whole pass (a dropped arm
// dragged across a populated board is what snapped it before), every cross-row
// leg goes through moveToYSafe so pure-Y travel only ever happens at an X soft
// limit, targets are clamped on both axes, and nothing is sensed until
// waitForMotion() says the carriage has actually stopped. Serpentine order via
// cellAt(), so end-of-row X equals start-of-next-row X. Mid-scan re-home after
// row 8, as in both source sweeps, so accumulated step drift can't skew the
// second half.
//
// gridState[] is NOT touched: this sweep never flips a disc, so whatever the
// colour scan last measured is still true and gridStateFresh stays as it was.
// It DOES re-home, so needsRehome is cleared by rehome() itself.
//
// ~4.3 s/cell ≈ 50 minutes. That is why it is a once-a-day scheduled job and
// not something the print loop pays for.
void runDailyLidarScan(const struct tm& day) {
  plog::logf("cadence: lidar scan begin %04d-%02d-%02d %02d:%02d",
             day.tm_year + 1900, day.tm_mon + 1, day.tm_mday,
             day.tm_hour, day.tm_min);

  // RESUME POINT. The record used to be stamped only on completion, so a
  // mid-sweep MCU reset threw the entire pass away and the next boot started
  // again at cell 0. On 2026-08-25 that turned a recoverable serial glitch
  // (see grblIsOk) into 7 restarts between 10:00 and 12:13 -- 304, 292, 207,
  // 291, 295 and 70 cells, none past row 8 of 18 -- and because cadenceGate()
  // only returns once the scan is done, the rig did not poll or print for the
  // whole three hours. Left alone it would have looped to the 18:00 cutoff and
  // printed nothing all day.
  //
  // Now every completed row is checkpointed, so a restart costs at most one row
  // and the sweep converges even if the underlying fault comes back. Resume
  // only within the SAME local day: a checkpoint from yesterday describes a
  // board that has been printed on since, so it is not partial data any more.
  int startRow = 0;
  if (cadenceRecValid && cadenceRec.sensorOk &&
      cadenceRec.progYear == day.tm_year && cadenceRec.progYday == day.tm_yday &&
      cadenceRec.rowsDone > 0 && cadenceRec.rowsDone < GRID_H) {
    startRow = (int)cadenceRec.rowsDone;
    plog::logf("cadence: resuming lidar scan at row %d/%d (%lu cells already measured)",
               startRow, GRID_H, (unsigned long)cadenceRec.cellsOk);
  } else {
    memset(&cadenceRec, 0, sizeof(cadenceRec));
    cadenceRec.magic   = CADENCE_MAGIC;
    cadenceRec.version = CADENCE_VERSION;
    cadenceRec.gridW   = GRID_W;
    cadenceRec.gridH   = GRID_H;
  }

  if (lidarEnsure()) {
    cadenceRec.sensorOk = 1;
    // The gate runs this scan before loop() reaches its grblBringup() call, so
    // the first motion of a fresh boot can be this very sweep — bring GRBL up
    // (homing) first. No-op every day thereafter.
    grblBringup();
    rehome();
    // Explicit park even though every path into here leaves it parked — the
    // arm's position is the one thing a 50-minute sweep must not get wrong.
    writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);

    const int N = GRID_W * GRID_H;
    const int i0 = startRow * GRID_W;      // serpentine visit index of the resume row
    int y0, x0;
    cellAt(i0, y0, x0);
    moveToYSafe(lidarTargetX(y0, x0), lidarTargetY(y0, x0));
    waitForMotion();

    for (int i = i0; i < N; i++) {
      int y, x;
      cellAt(i, y, x);

      // 1. Sense (stationary). Discard one read first: the free-running
      //    measurement in flight may straddle the tail of the move.
      lidar.read();
      uint32_t avg10;
      uint16_t mn, mx;
      int n = lidarWindowRead(avg10, mn, mx);
      if (avg10 > 0xFFFF) avg10 = 0xFFFF;
      cadenceRec.dist10[y * GRID_W + x] = (uint16_t)avg10;
      if (n > 0) cadenceRec.cellsOk++;
      bool last = (i == N - 1);

      // 2. Start the move to the next cell (non-blocking — GRBL begins moving).
      if (!last) {
        int ny, nx;
        cellAt(i + 1, ny, nx);
        if (ny == y) {
          moveTo(lidarTargetX(ny, nx), lidarTargetY(ny, nx));      // intra-row (pure X)
        } else {
          if (y == 8) rehome();                                    // arm is already at REST
          moveToYSafe(lidarTargetX(ny, nx), lidarTargetY(ny, nx)); // inter-row edge legs
        }
      }

      // 3. Log this cell to flash WHILE GRBL travels — the same pipelining
      //    scanGrid uses. Tenths printed as int.int so no %f is needed.
      plog::logf("ld y%dc%d %lu.%lu n%d mn%u mx%u", y, x,
                 (unsigned long)(avg10 / 10), (unsigned long)(avg10 % 10),
                 n, (unsigned)mn, (unsigned)mx);

      // 4. Checkpoint the row that just finished (see the resume note above).
      //    ~1.4 KB through the temp-file swap, 17 times a sweep, and it sits in
      //    the same pipelined window as the plog write -- hidden behind GRBL's
      //    travel to the next cell. The final row is skipped here because the
      //    completion stamp below writes the record anyway.
      if (!last && (i + 1) % GRID_W == 0) {
        cadenceRec.rowsDone = (uint16_t)((i + 1) / GRID_W);
        cadenceRec.progYear = day.tm_year;
        cadenceRec.progYday = day.tm_yday;
        cadenceSaveRecord();
      }

      // 5. Ensure the move finished before sensing the next cell.
      if (!last) waitForMotion();
    }
    cadenceRec.rowsDone = GRID_H;          // sweep complete
  } else {
    // The date is still stamped. A dead ranger must not put the rig into a
    // retry loop that spends the whole day re-attempting a 50-minute sweep;
    // sensorOk=0 says plainly that this day has no data.
    //
    // sensorOk is deliberately NOT forced to 0 here. On the resume path it may
    // already be 1 with a checkpoint's worth of real cells behind it, and it
    // means "the ranger produced the cells that are present" -- cellsOk says
    // how many. Only the fresh path reaches here with sensorOk=0, from memset.
    plog::logf("cadence: lidar unavailable - writing off today (rows%u cells%lu)",
               (unsigned)cadenceRec.rowsDone, (unsigned long)cadenceRec.cellsOk);
  }

  // Stamp with the time the scan FINISHED, re-read rather than reusing `day`:
  // the sweep takes ~50 minutes and could cross midnight if it ever started
  // late, and the day we mark done must be the day the record describes.
  struct tm done;
  if (localNow(done)) {
    cadenceRec.year = done.tm_year;
    cadenceRec.yday = done.tm_yday;
    cadenceRec.epoch = (uint32_t)time(nullptr);
  } else {
    // Clock died mid-scan (shouldn't happen — the RTC free-runs). Fall back to
    // the day we started, so we at least don't immediately re-scan.
    cadenceRec.year = day.tm_year;
    cadenceRec.yday = day.tm_yday;
  }
  cadenceSaveRecord();
  plog::logf("cadence: lidar scan end cells=%lu", (unsigned long)cadenceRec.cellsOk);
}

// ----------------------------------------------------------------- night sleep
// Release the steppers for a long idle. $1=0 only takes effect on the next idle
// transition, so a tiny jog (X-0.1, away from the X=0 soft limit) triggers the
// disable. Factored out of loop()'s end-of-job path so the night sleep releases
// them exactly the same way — including the waitForIdle AFTER $1=0, which is
// mandatory: $1=0 changes the value, so grbl commits the settings block to the
// Mega's EEPROM under cli() with the Serial1 RX ISR dead, and anything
// pipelined behind it arrives garbled (error:2 -> desynced ok accounting -> the
// 60 s waitForIdle watchdog -> MCU reset). G21/G90 are reasserted first because
// error:2 often traces back to mm/inch or abs/rel mode drifting in a recovery
// path.
void releaseSteppers() {
  sendGcode("G21");
  sendGcode("G90");
  sendGcode("$1=0");
  needsRehome = true;   // steppers are about to go limp: home before any motion
  waitForIdle();
  sendGcode("G91");
  sendGcode("G0 X-0.1");
  sendGcode("G90");
  waitForIdle();
}

// Idle until LIDAR_SCAN_HOUR local. No polling, no server ingestion, carriage
// stationary with the steppers released and the servo at REST.
//
// Deliberately a plain delay() loop rather than esp_light_sleep/deep sleep:
// delay() yields to FreeRTOS (so the idle task feeds the watchdogs), the
// millis()-based GRBL/WiFi stall watchdogs stay coherent, and no reset-cause
// breadcrumb is fabricated. WiFi is left to its own devices — it may well drop
// over 14 hours — and is re-established on wake before anything needs it. The
// RTC free-runs without the network, so the wake decision does not depend on
// NTP staying reachable.
void sleepUntilMorning() {
  struct tm t;
  if (!localNow(t)) return;                 // no clock: never sleep blind

  // Wake target: the NEXT 10:00 local, as an epoch. An epoch compare rather
  // than "hour left the night window" because this is no longer only entered
  // at night — the past-cutoff owed-scan case enters as early as 18:00, where
  // an hour-window test would bounce straight out. mktime with tm_isdst = -1
  // re-resolves DST for the target day, so a sleep spanning the spring/fall
  // change still ends at 10:00 wall-clock.
  struct tm tgt = t;
  tgt.tm_hour = LIDAR_SCAN_HOUR;
  tgt.tm_min = 0;
  tgt.tm_sec = 0;
  tgt.tm_isdst = -1;
  time_t wake = mktime(&tgt);
  if (wake <= time(nullptr)) {              // 10:00 already passed today -> tomorrow's
    tgt.tm_mday += 1;                       // mktime normalizes month/year rollover
    tgt.tm_isdst = -1;
    wake = mktime(&tgt);
  }
  plog::logf("cadence: sleep at %02d:%02d until %02d:00",
             t.tm_hour, t.tm_min, LIDAR_SCAN_HOUR);

  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
  // Release only if GRBL has been brought up this boot: before grblBringup()
  // the Mega is in its power-on alarm state and any G-code would error:9 into
  // the retry-then-reset path. An un-brought-up GRBL has its steppers
  // unpowered anyway — there is nothing to release.
  if (grblHomed && !needsRehome) releaseSteppers();  // needsRehome set <=> already limp

  // Backstop against a wedged/garbage clock: at most 17 h, which is longer than
  // the longest legitimate sleep (18:00 -> 10:00 = 16 h, the past-cutoff case)
  // and short enough that a stuck clock costs at most one day rather than
  // forever.
  const unsigned long MAX_SLEEP_MS = 17UL * 60UL * 60UL * 1000UL;
  const unsigned long TICK_MS = 30000;
  unsigned long t0 = millis();
  unsigned long ticks = 0;
  while (millis() - t0 < MAX_SLEEP_MS) {
    delay(TICK_MS);
    ticks++;
    if (!clockValid()) continue;            // clock lost/stale — hold under the backstop
    if (time(nullptr) >= wake) break;
    if ((ticks % 120) == 0 && localNow(t))  // ~hourly heartbeat so the log shows life
      plog::logf("cadence: sleeping %02d:%02d", t.tm_hour, t.tm_min);
  }

  plog::log("cadence: wake");
  ensureWiFi();
  timeBegin();
}

// The clear-to-operate gate. Every loop iteration passes through here before
// it is allowed to poll — or MOVE. Returns true only when the rig holds a
// trusted time-server clock AND is inside the awake window with today's
// obligations met; false means "stay parked, do nothing, retry later".
//
// HARD RULE (review F3): no valid NTP-anchored clock => no motion of any
// kind — no polling, no scanning, not even homing. There is deliberately no
// "uncadenced" fallback anymore.
bool cadenceGate() {
  struct tm t;
  if (!localNow(t)) {
    static unsigned long lastWarn = 0;
    if (lastWarn == 0 || millis() - lastWarn > 300000UL) {
      lastWarn = millis();
      plog::log("cadence: no trusted clock - holding parked (no motion, no polling)");
    }
    return false;
  }

  // Sleep whenever it is night, OR today's scan is still owed at/after the
  // 18:00 cutoff (the day is written off — scan at 10:00 tomorrow instead of
  // sweeping ~50 min into the evening). A WHILE, not an if (review F1): the
  // condition is re-tested after EVERY wake, so a spurious exit from
  // sleepUntilMorning() — e.g. the 17 h backstop firing after SNTP stepped the
  // clock backwards — lands straight back in sleep instead of falling through
  // to a scan and a poll at 04:00.
  while (isNightHour(t.tm_hour) ||
         (!lidarScanDoneOn(t) && t.tm_hour >= LIDAR_SCAN_CUTOFF_HOUR)) {
    sleepUntilMorning();
    if (!localNow(t)) return false;         // woke with no trusted clock: hold parked
  }

  // Daytime, before the cutoff, scan owed — covers "it just turned 10:00",
  // "we booted at 14:00", and the morning after a written-off day.
  if (!lidarScanDoneOn(t)) {
    runDailyLidarScan(t);
    // The sweep runs ~50 min (review F4): re-verify before letting the caller
    // poll — a mid-scan SNTP step could have landed us inside the night
    // window. Returning false re-enters this gate, which then sleeps.
    if (!localNow(t) || isNightHour(t.tm_hour)) return false;
  }
  return true;
}

// GRBL bring-up + homing, moved OUT of setup() (review F2/F3). loop() calls
// this on the first pass where cadenceGate() says the rig is clear to operate,
// so a nocturnal or clockless reset sits parked — servo at REST, not one byte
// of G-code — until 10:00 with a trusted clock. Retries on GRBL error/ALARM by
// bouncing Serial1 (forces GRBL to re-init its half of the link) and re-running
// the full startup sequence; without that, a power-on alarm during $H would
// wedge the rig until a manual reset.
void grblBringup() {
  if (grblHomed) return;
  delay(2000);  // GRBL boot margin — cheap, and covers a Mega that reset late

  inStartupPhase = true;
  for (int attempt = 1;; attempt++) {
    grblStartupFault = false;
    qHead = qTail = 0;
    bufferFill = 0;
    while (Serial1.available()) Serial1.read();

    plog::logf("homing attempt %d", attempt);
    sendGcode("$H");
    waitForIdle();
    if (grblStartupFault) goto restart_grbl;

    // $1=255 keeps steppers energized while idle so the gantry holds position
    // between motions; we drop back to $1=0 + a tiny jog at job end to release.
    sendGcode("$1=255");
    waitForIdle();  // sync past the $1 EEPROM write before pipelining more (see rehome)
    if (grblStartupFault) goto restart_grbl;
    sendGcode("G21");
    sendGcode("G90");
    waitForIdle();
    if (grblStartupFault) goto restart_grbl;

    plog::log("homed");
    break;

restart_grbl:
    // Escalating backoff. A transient fault clears on the first bounce, so the
    // early retries stay fast; a Mega that is unpowered, unplugged or hung will
    // never clear, and hammering it every ~80 s (the stall timeout plus a fixed
    // 2 s wait) just burns the flash log and the servo park cycle. Cap at 60 s
    // so recovery is still prompt once the hardware comes back.
    {
      unsigned long backoff = 2000UL * (unsigned long)attempt;
      if (backoff > GRBL_RETRY_BACKOFF_MAX_MS) backoff = GRBL_RETRY_BACKOFF_MAX_MS;
      plog::logf("GRBL restart + retry homing (attempt %d, backoff %lums)",
                 attempt, backoff);
      Serial1.end();
      delay(200);
      Serial1.begin(115200, SERIAL_8N1, D0, D1);
      delay(backoff);  // GRBL boot wait after re-opening Serial1
    }
  }
  inStartupPhase = false;
  grblHomed = true;
}

void setup() {
  // Servo is driven by a dedicated 5V Arduino Nano over Serial2, TX-only on D9
  // → Nano D2 RX (SoftwareSerial), shared GND. RX pin is -1 (nothing comes back on this link).
  // One-way; the companion sketch parses integer µs values per line. Bring the
  // UART up first so the very first park command below is actually received.
  // The ESP32-S3 GPIO matrix routes UART2's TX to D9, so the WIRING IS
  // UNCHANGED from the RP2040 bit-bang that used to drive the same pin.
  // Shield peripheral rail FIRST -- the ServoNano this park command is aimed at
  // is powered from it, and so is GRBL. No-op on a hand-wired rig.
  pinMode(SHIELD_PWR_PIN, OUTPUT);
  digitalWrite(SHIELD_PWR_PIN, HIGH);

  pinMode(SERVO_ACK_PIN, INPUT);  // divider's bottom leg is the pulldown

  // Long enough for the rail to rise AND the ServoNano's bootloader to hand
  // over, or the park below is sent into a board that is not listening yet and
  // the arm stays wherever it was.
  //
  // !! Serial2.begin() is DELIBERATELY AFTER this delay. Opening the UART
  // !! drives D9 to the idle-HIGH state immediately, and D9 goes to an input on
  // !! the ServoNano -- which is on the switched rail and therefore still at
  // !! 0 V during the ramp. Driving 3.3 V into an input whose VDD is 0 V
  // !! forward-biases that pin's protection diode and injects current into the
  // !! dead rail. Leaving the UART shut keeps D9 high-Z (the ESP32 reset
  // !! default) until the ServoNano is actually powered, which removes the
  // !! injection entirely -- and does it without raising R1, whose value is set
  // !! by noise immunity on the servo command line, not by this.
  delay(SHIELD_PWR_SETTLE_MS);

  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);
  servoTxLine(SERVO_US_REST);

  delay(10000);
  // GRBL Mega link. On the Nano ESP32 `Serial` is USB CDC and `Serial0` is the
  // D0/D1 UART, while `Serial1` has NO default pins — so UART0 must be released
  // and Serial1 explicitly pinned to D0/D1. Again: the wiring is unchanged, only
  // which peripheral drives those header pins.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  // The 2-second delay below covers the GRBL/Serial1 boot wait. USB Serial
  // is intentionally not initialized — when the rig runs headless on wall
  // power, an undrained CDC stream stalls writes; the flash log is the only
  // debug trail.

  // Mount the flash log so this boot's WiFi/HTTP/poll events get recorded.
  // The log persists across resets.
  plog::begin();
  plog::log("boot");
#if SERVO_ACK_MODE > 0
  // Active-HIGH ack: the line must idle LOW. Idling HIGH means the
  // ServoNano still has the old active-LOW firmware, under which every
  // command would read as acked and SERVO_ACK_MODE 2 would enforce
  // nothing. By here the boot REST command's 40 ms pulse is long gone.
  if (!servoAckProbeIdle())
    plog::log("ACK LINE IDLES HIGH - ServoNano is probably still running the OLD active-LOW build. The ack is NOT protecting you: flash ServoNano.ino before running any job.");
#endif
  // Flip/servo build variables. A boot line is the only record of which firmware
  // a run actually used, and the flip regime (engage/release base angles, comp
  // mode, unload) is exactly what a bad print has to be read against. Mirrors
  // FlipAllTest's BOOT line so the two logs can be compared directly.
  plog::logf("BOOT eng%d rel%d scan%d tx%d ack%d comp%d unl%d/%d reach%d",
             SERVO_US_ENGAGE, SERVO_US_RELEASE, SERVO_US_SCAN,
             SERVO_TX_REPEATS, SERVO_ACK_MODE, LIDAR_COMP_MODE,
             (int)(FLIP_UNLOAD_X * 10.0f), (int)(FLIP_UNLOAD_X_EDGE * 10.0f),
             (int)(RELEASE_EXTRA_REACH_MM * 10.0f));

  // Reset-cause breadcrumb — THE key diagnostic for the silent reboots we're
  // chasing. On the RP2040 this had to be INFERRED: a magic cookie was stashed
  // in a watchdog scratch register (survives a soft reset, wiped by a power
  // event), so a missing cookie meant "cold/power" and that was as specific as
  // it got. The ESP32-S3 records the cause in hardware and esp_reset_reason()
  // reports it directly, so we now get the actual verdict instead of a guess:
  //   BROWNOUT  -> the supply sagged past the BOD threshold. THIS is the one
  //                we're hunting; it is an electrical fault, not firmware.
  //   POWERON   -> genuine cold start (plug-in, power cycle, RST button).
  //   SW        -> one of our own esp_restart() paths — the reason line logged
  //                just above this one (GRBL stall / WiFi stall / error retries
  //                exhausted / ALARM recovery failure) says which.
  //   PANIC     -> firmware crash (exception / abort). On the RP2040 a hard
  //                fault just LOCKED UP, so a reboot could never be firmware;
  //                on the ESP32 it can, and this is how we'd see it.
  //   TASK_WDT / INT_WDT / WDT -> a watchdog fired: something blocked too long.
  // Keep the "reset cause: <NAME>" line shape — the plog dumps are grepped for it.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* name;
    switch (rr) {
      case ESP_RST_POWERON:  name = "POWERON";  break;
      case ESP_RST_BROWNOUT: name = "BROWNOUT"; break;
      case ESP_RST_SW:       name = "SW";       break;
      case ESP_RST_PANIC:    name = "PANIC";    break;
      case ESP_RST_TASK_WDT: name = "TASK_WDT"; break;
      case ESP_RST_INT_WDT:  name = "INT_WDT";  break;
      case ESP_RST_WDT:      name = "WDT";      break;
      case ESP_RST_EXT:      name = "EXT";      break;
      case ESP_RST_DEEPSLEEP:name = "DEEPSLEEP";break;
      case ESP_RST_SDIO:     name = "SDIO";     break;
      default:               name = "UNKNOWN";  break;
    }
    plog::logf("reset cause: %s (%d)", name, (int)rr);
  }

  initGrid();

  // S0/S1 = 1/0 → 20% output frequency scaling. Full-speed (HIGH/HIGH) tops
  // out near 600 kHz, which is past what pulseIn can resolve cleanly here;
  // 20% keeps us well inside that envelope. (Helper so scanGrid's bad-regime
  // recovery can re-assert the same config mid-run.) UNVERIFIED ON ESP32-S3:
  // pulseIn() is a busy-wait on a GPIO and the ESP32 runs FreeRTOS with the WiFi
  // stack taking interrupts, so it jitters more than the bare-metal RP2040 did.
  // The scan is ambient-subtracted and thresholded with a ~10x margin, so modest
  // jitter should wash out — but confirm the on/off clusters still separate
  // (ColorSensorTest / ScanColorAmbientTest) before trusting a print.
  initColorSensor();

  // GRBL bring-up + homing happens LAZILY in loop() via grblBringup(), never
  // here (review F2/F3): homing is full-travel motion, and setup() runs on
  // every reset — including a brownout at 02:00 or a boot with no NTP. The
  // gantry must not move until cadenceGate() confirms a trusted clock and
  // daytime.

  // scanGrid();

  // Trust anchors for the (vestigial) shared client. mbedTLS has no built-in
  // root store, so every WiFiClientSecure needs the bundle before it connects;
  // the per-call clients in fetchNext()/onDisplayComplete()/sendSnapshotRequest()
  // each do their own parSecure().
  parSecure(wifi);
  client.setHttpResponseTimeout(15000);

  // Daily cadence bring-up. WiFi has to be up first because the wall clock
  // comes from NTP; the wait is bounded only so a dead network can't wedge
  // setup() — failure unlocks nothing. With no time-server fix, loop()'s gate
  // holds the rig parked (no motion, no polling) and retries until one lands.
  ensureWiFi();
  timeBegin();
  timeWaitForSync(30000);
  cadenceLoadRecord();
  cutLoadRecord();
  // Bring the ranger up now rather than lazily at the first 10:00 scan, so a
  // wiring fault surfaces in the log at boot. Bounded (10 attempts) and
  // idempotent — runDailyLidarScan()'s own lidarEnsure() re-tries anyway if
  // this one fails, so a dead ranger costs ~10 s of boot, never the rig.
  lidarEnsure();
  // Nothing is scheduled from here — loop()'s cadenceGate() owns the boot-time
  // night sleep and the "we owe today's scan" case, so both live in one place.
}

// Try /complete.php with exponential backoff for up to 5 minutes. If the
// server is reachable but returns a non-2xx, or the network is flaky, we keep
// retrying so a transient blip doesn't strand the gallery entry as pending.
// After the 5-minute budget is exhausted we give up and clear pendingGalleryId
// — the entry stays as pending.json server-side; same behavior as a verify-fix
// exhaustion. Caller is expected to always reach this only after a clean
// verify pass.
void onDisplayComplete() {
  if (pendingGalleryId[0] == '\0') return;
  plog::logf("complete.php start id=%s", pendingGalleryId);

  const unsigned long TOTAL_BUDGET_MS = 5UL * 60UL * 1000UL;
  unsigned long backoff = 1000;
  unsigned long start = millis();

  while (millis() - start < TOTAL_BUDGET_MS) {
    bool attemptOk = false;
    WiFiClientSecure ssl;
    parSecure(ssl);
    if (ssl.connect(SERVER, PORT)) {
      ssl.print("GET /complete.php?id=");
      ssl.print(pendingGalleryId);
      ssl.print(" HTTP/1.1\r\n");
      ssl.print("Host: ");
      ssl.print(SERVER);
      ssl.print("\r\n");
      ssl.print("User-Agent: P.A.R./1.0\r\n");
      ssl.print("Accept: */*\r\n");
      ssl.print("X-Snapshot-Secret: ");
      ssl.print(SNAPSHOT_SECRET);
      ssl.print("\r\n");
      ssl.print("Connection: close\r\n");
      ssl.print("\r\n");

      String statusLine = ssl.readStringUntil('\n');
      ssl.stop();
      // statusLine looks like "HTTP/1.1 200 OK". Treat any 2xx as success.
      int sp = statusLine.indexOf(' ');
      if (sp >= 0 && statusLine.charAt(sp + 1) == '2') attemptOk = true;
    } else {
      plog::log("complete.php connect() failed");
    }

    if (attemptOk) {
      plog::logf("complete.php ok id=%s", pendingGalleryId);
      pendingGalleryId[0] = '\0';
      return;
    }

    plog::logf("complete.php retry in %lu ms", backoff);
    delay(backoff);
    backoff *= 2;
    if (backoff > 60000UL) backoff = 60000UL;
  }

  plog::logf("complete.php abandoned id=%s", pendingGalleryId);
  pendingGalleryId[0] = '\0';
}

// Fire-and-forget POST to /snapshot-request.php telling the Mac Mini that the
// board is now settled and a snapshot should be captured for this gallery id.
// Called after the check pass (scan + re-fix) so the photo reflects the final
// corrected state, not the first-pass draw. No retries — a missed snapshot
// just leaves gallery/<id>/image.* absent; the modal already falls back to
// "No P.A.R. image available."
void sendSnapshotRequest(const char* galleryId) {
  if (!galleryId || galleryId[0] == '\0') return;
  plog::logf("snapshot-request start id=%s", galleryId);

  WiFiClientSecure ssl;
  parSecure(ssl);
  if (!ssl.connect(SERVER, PORT)) {
    plog::log("snapshot-request connect() failed");
    return;
  }

  char body[64];
  int bodyLen = snprintf(body, sizeof(body), "id=%s", galleryId);
  if (bodyLen <= 0 || bodyLen >= (int)sizeof(body)) {
    ssl.stop();
    return;
  }

  ssl.print("POST /snapshot-request.php HTTP/1.1\r\n");
  ssl.print("Host: ");
  ssl.print(SERVER);
  ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Content-Type: application/x-www-form-urlencoded\r\n");
  ssl.print("Content-Length: ");
  ssl.print(bodyLen);
  ssl.print("\r\n");
  ssl.print("X-Snapshot-Secret: ");
  ssl.print(SNAPSHOT_SECRET);
  ssl.print("\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");
  ssl.print(body);

  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 10000) break;
    delay(10);
  }
  String statusLine = ssl.readStringUntil('\n');
  ssl.stop();
  int sp = statusLine.indexOf(' ');
  if (sp >= 0 && statusLine.charAt(sp + 1) == '2') {
    plog::logf("snapshot-request ok id=%s", galleryId);
  } else {
    plog::logf("snapshot-request unexpected: %.40s", statusLine.c_str());
  }
}

// ---- in-flight job persistence --------------------------------------------
// next.php POPS a queue item — the server hands it over exactly once and keeps
// no copy in the queue. So from the moment fetchNext() returns, the ONLY record
// that this print was ever requested lives in this MCU's RAM. Any reset before
// the job completes destroys it: the item is gone from queue.txt, no snapshot
// is ever requested, and the gallery entry sits `pending` forever.
//
// That is precisely how gallery 80 ("Ak") was lost on 2026-08-23 — popped at
// 14:38, killed one second into scanGrid by the GRBL stall watchdog, and never
// seen again. Job 79 died the same way mid-draw.
//
// Fix: write the bitmap and gallery id to flash the moment we accept a job, and
// resume from flash on the next boot instead of polling for new work. The
// record is cleared only once the print is finished and its snapshot requested.
// Redrawing is idempotent — a resumed job re-scans the board first (gridState
// is not trusted across a reset), so it fixes up whatever was half-drawn rather
// than starting from a false picture.
#define JOB_PATH "/job.bin"
#define JOB_TMP  "/job.tmp"
const uint32_t JOB_MAGIC   = 0x50414A42UL;  // 'PAJB'
const uint16_t JOB_VERSION = 1;

// A job that resets the MCU every time it runs would otherwise resume forever
// and brick the rig. Give up after this many starts and move on.
const uint8_t JOB_MAX_ATTEMPTS = 3;

struct PendingJobRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t  attempts;      // how many times this job has been STARTED
  uint8_t  reserved;
  char     galleryId[16];
  uint8_t  bitmap[84];
  uint32_t checksum;
};

// Takes void* rather than const PendingJobRecord& on purpose: the .ino
// preprocessor hoists a prototype for every function to the top of the file,
// ahead of the struct definition, so a user type in the signature fails to
// compile. Plain types sidestep that entirely.
static uint32_t jobChecksum(const void* rec, size_t n) {
  const uint8_t* p = (const uint8_t*)rec;
  uint32_t h = 2166136261UL;  // FNV-1a, same as the cadence record
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}
// Bytes covered by the checksum: the whole record except the trailing field.
#define JOB_CHECKED_BYTES (sizeof(PendingJobRecord) - sizeof(uint32_t))

// Temp-file-then-rename, matching cadenceSaveRecord's crash policy. A brownout
// mid-swap loses the record, which costs exactly what today's behaviour costs.
void jobSavePending(const uint8_t* bitmap, const char* galleryId, uint8_t attempts) {
  if (!cadenceFsReady()) {
    plog::log("job: LittleFS unavailable - in-flight job NOT crash-safe");
    return;
  }
  PendingJobRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = JOB_MAGIC;
  r.version = JOB_VERSION;
  r.attempts = attempts;
  strncpy(r.galleryId, galleryId, sizeof(r.galleryId) - 1);
  memcpy(r.bitmap, bitmap, sizeof(r.bitmap));
  r.checksum = jobChecksum(&r, JOB_CHECKED_BYTES);

  File t = LittleFS.open(JOB_TMP, "w");
  if (!t) { plog::log("job: save open failed"); return; }
  size_t wrote = t.write((const uint8_t*)&r, sizeof(r));
  t.close();
  if (wrote != sizeof(r)) {
    plog::logf("job: short write %u", (unsigned)wrote);
    LittleFS.remove(JOB_TMP);
    return;
  }
  LittleFS.remove(JOB_PATH);
  LittleFS.rename(JOB_TMP, JOB_PATH);
  plog::logf("job: saved id=%s attempt=%u", r.galleryId, (unsigned)attempts);
}

// Anything unexpected (missing, short, wrong magic/version, bad checksum) means
// "no job on record" — the rig polls for new work, which is the safe direction.
bool jobLoadPending(uint8_t* bitmapOut, char* idOut, size_t idCap, uint8_t& attemptsOut) {
  if (!cadenceFsReady()) return false;
  File f = LittleFS.open(JOB_PATH, "r");
  if (!f) return false;
  if (f.size() != sizeof(PendingJobRecord)) {
    plog::logf("job: record size %u != %u, ignoring",
               (unsigned)f.size(), (unsigned)sizeof(PendingJobRecord));
    f.close();
    LittleFS.remove(JOB_PATH);
    return false;
  }
  PendingJobRecord r;
  size_t got = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (got != sizeof(r) || r.magic != JOB_MAGIC || r.version != JOB_VERSION ||
      r.checksum != jobChecksum(&r, JOB_CHECKED_BYTES)) {
    plog::log("job: record corrupt/foreign, discarding");
    LittleFS.remove(JOB_PATH);
    return false;
  }
  r.galleryId[sizeof(r.galleryId) - 1] = '\0';
  if (strlen(r.galleryId) == 0 || strlen(r.galleryId) >= idCap) {
    plog::log("job: record has unusable gallery id, discarding");
    LittleFS.remove(JOB_PATH);
    return false;
  }
  memcpy(bitmapOut, r.bitmap, sizeof(r.bitmap));
  strncpy(idOut, r.galleryId, idCap - 1);
  idOut[idCap - 1] = '\0';
  attemptsOut = r.attempts;
  return true;
}

void jobClearPending() {
  if (!cadenceFsReady()) return;
  if (LittleFS.remove(JOB_PATH)) plog::log("job: cleared");
}

// The whole print, from board scan through snapshot request. Extracted from
// loop() so a job resumed from flash runs the exact same path as a freshly
// polled one — there is no second, subtly-different code path to drift.
// `fallbackId` is used only when pendingGalleryId is empty (a job whose id
// failed validation, which is never persisted).
void runPendingJob(uint8_t* bitmap, const char* fallbackId) {
  // Re-scan the board before each job IF gridState[] isn't already a
  // trustworthy *measured* picture of the discs. Two ways it can be:
  //   * the very first job after boot reuses setup()'s scan;
  //   * the previous job ended on a scan with no fix after it (check pass
  //     found <= CHECK_FIX_MAX_SKIP wrong, or the draw flipped nothing at
  //     all) — see the end-of-job carry-over below.
  // Any flip since that scan makes the state inferred rather than measured,
  // so we pay the full sweep again.
  if (!gridStateFresh) {
    plog::log("scanGrid begin");
    if (!scanGrid()) {
      // Both populations present but overlapping — the sensor is not
      // separating the faces. One retry, in case it was transient.
      plog::log("scan quality bad (overlap) — re-scanning once");
      if (!scanGrid()) {
        // Still bad. Drawing against this is how the board gets scrambled, so
        // do not draw at all. The job record is left pending (jobClearPending
        // only runs on the success path below), so the next loop pass resumes
        // it and JOB_MAX_ATTEMPTS bounds the retries.
        plog::log("scan still bad — REFUSING TO DRAW, job left pending");
        releaseSteppers();
        return;
      }
    }
  } else {
    // The scan homes on its own; skipping it means we still owe a homing
    // cycle if the steppers were released for the idle (the gantry can be
    // nudged with the motors off).
    if (needsRehome) {
      plog::log("rehome (scan skipped)");
      rehome();
    }
    plog::log("scanGrid skipped (state fresh)");
  }

  plog::log("displayBitmap begin");
  int flipped = displayBitmap(bitmap);
  waitForIdle();
  plog::logf("displayBitmap flipped %d cells", flipped);

  // Check pass: re-scan the board (which reseeds gridState[] from the
  // physical discs, catching any that didn't flip cleanly), then run
  // displayBitmap again so its diff-against-gridState logic re-flips just
  // the cells that are still wrong. Two short-circuits (CHECK_FIX_MAX_SKIP):
  //   1. If the first draw flipped that few cells or fewer, the job was tiny
  //      (few chances to fail) — skip the whole check pass, re-scan and all.
  //   2. Otherwise re-scan, but only re-flip when MORE than that many cells
  //      are still wrong. The color sensor is ~99.5% accurate, so a 666-cell
  //      scan misreads ~3 cells on average — <=5 mismatches sit in that noise
  //      floor, so re-flipping them would more likely flip a correct disc
  //      than fix a real miss. Tolerating <=5 doesn't lower accuracy.
  if (flipped <= CHECK_FIX_MAX_SKIP) {
    plog::logf("check pass skipped (only %d flipped)", flipped);
  } else {
    plog::log("check pass: scanGrid begin");
    if (!scanGrid()) {
      // The board is already drawn; the only thing a bad check-pass scan can do
      // is re-flip correct cells. Skip the fix rather than retry the 17 min
      // scan. gridStateFresh is already false, so the next job re-measures.
      plog::log("check pass: scan quality bad (overlap) — skipping fix");
    } else {
      int wrong = countMismatches(bitmap);
      if (wrong > CHECK_FIX_MAX_SKIP) {
        plog::logf("check pass: %d wrong, re-fixing", wrong);
        displayBitmap(bitmap);
        waitForIdle();
      } else {
        plog::logf("check pass: %d wrong (<=%d), skip fix", wrong, CHECK_FIX_MAX_SKIP);
      }
    }
  }

  plog::log("display done");
  // Trigger the snapshot now that the board reflects the final corrected
  // state (after the check pass), rather than relying on next.php to have
  // armed it at job start — that earlier armed window let the snapshot
  // poller grab a photo mid-draw.
  sendSnapshotRequest(pendingGalleryId[0] ? pendingGalleryId : fallbackId);
  onDisplayComplete();
  // The print is done and the snapshot requested, so the job can no longer be
  // lost by a reset. Drop the flash record before the long idle — leaving it
  // would make the next boot redraw a print that already completed.
  jobClearPending();
  // Carry the scan over to the next job when gridState[] is still a
  // measured picture of the board — i.e. the last thing that touched the
  // discs was a scan, with no fixing after it (check pass re-scanned and
  // found <= CHECK_FIX_MAX_SKIP wrong, or the draw itself flipped nothing).
  // That scan already describes the final board, so re-running it next job
  // costs ~70 min to learn what we just measured. If anything flipped after
  // the last scan, the state is only inferred and the next job re-scans.
  gridStateFresh = gridStateFromScan;
  // Release steppers for the long idle (the $1=0 + jog dance, with its
  // mandatory post-$1=0 sync — see releaseSteppers()).
  releaseSteppers();
  // The print is finished, so this is the cadence's decision point: if the
  // day is over, sleep through the night instead of lingering 10 minutes and
  // polling again. Checked HERE rather than only at the top of loop() so the
  // rig never starts a ~1 h job it would finish deep into the evening and
  // then immediately start another.
  struct tm nowT;
  if (localNow(nowT) && isNightHour(nowT.tm_hour)) {
    sleepUntilMorning();
  } else {
    // Post-display linger: paces polling and lets the board settle before the
    // next job. The recording is already stopped by now — the Mac Mini ends it
    // when it captures this print's snapshot (the snapshot request above is the
    // single "print done" signal), so no stream-end is sent from here.
    delay(10UL * 60UL * 1000UL);
  }
}

void loop() {
  ensureWiFi();
  timeBegin();
  // Clear-to-operate gate, BEFORE any motion or server ingestion: trusted
  // NTP clock + awake window + today's lidar scan. A rig that boots at 2 am
  // sleeps; a rig with no time-server fix holds parked and just retries here
  // (HARD RULE: no server time => no motion, not even homing).
  if (!cadenceGate()) {
    delay(30000);
    return;
  }
  // First clear daytime pass since boot: bring GRBL up and home now. No-op on
  // every later pass.
  grblBringup();

  // Resume an interrupted job BEFORE asking the server for new work. next.php
  // has no copy of a popped item, so this flash record is the only thing that
  // can bring one back after a reset.
  uint8_t bitmap[128];
  bool haveJob = false;
  {
    char savedId[sizeof(pendingGalleryId)];
    uint8_t attempts = 0;
    if (jobLoadPending(bitmap, savedId, sizeof(savedId), attempts)) {
      if (attempts >= JOB_MAX_ATTEMPTS) {
        // Something about this job kills the MCU every time. Abandon it rather
        // than resuming forever — the gallery entry stays pending, but the rig
        // stays useful.
        plog::logf("job: id=%s abandoned after %u attempts", savedId, (unsigned)attempts);
        jobClearPending();
      } else {
        attempts++;
        plog::logf("job: resuming id=%s attempt %u/%u",
                   savedId, (unsigned)attempts, (unsigned)JOB_MAX_ATTEMPTS);
        jobSavePending(bitmap, savedId, attempts);
        strncpy(pendingGalleryId, savedId, sizeof(pendingGalleryId) - 1);
        pendingGalleryId[sizeof(pendingGalleryId) - 1] = '\0';
        // The board was left in an unknown, possibly half-drawn state, so the
        // job must re-measure it rather than draw against a stale picture.
        gridStateFresh = false;
        gridStateFromScan = false;
        haveJob = true;
      }
    }
  }

  int status = 0;
  String galleryId = "";
  String body = "";

  if (!haveJob) {
    plog::log("poll start");
    bool ok = fetchNext(status, galleryId, body);
    if (!ok) {
      plog::log("poll fetchNext failed");
      delay(10000);
      return;
    }
    plog::logf("poll status=%d bodyLen=%u", status, (unsigned)body.length());
  }

  if (haveJob) {
    // Resumed from flash — bitmap and pendingGalleryId are already populated.
    runPendingJob(bitmap, pendingGalleryId);
  } else if (status == 200 && body != "NONE" && body.length() > 0) {
    // 37 cols × 18 rows = 666 bits → 84 bytes (last byte has 6 padding bits).
    // 84 bytes encodes to exactly 112 base64 chars. decode_base64() does no
    // output-bounds-check, so reject anything longer before we hand it the
    // buffer, and decode into a scratch buffer that tolerates a small amount
    // of extra input as defense-in-depth.
    const size_t MAX_BODY_CHARS = 112;
    if (body.length() > MAX_BODY_CHARS) {
      plog::logf("body too long: %u", (unsigned)body.length());
      delay(10000);
      return;
    }
    // Decodes into loop()'s `bitmap` (declared above for the resume path) —
    // one buffer serves both, so there is no shadowing copy to get out of sync.
    int decoded = decode_base64((unsigned char*)body.c_str(), bitmap);

    if (decoded == 84) {
      plog::log("bitmap rx ok");

      // Validate gallery id is digits-only and fits the buffer before storing
      // it — it gets interpolated straight into the /complete.php URL, so a
      // stray \r\n or other junk would let the server inject extra HTTP.
      if (!isDigitsOnlyId(galleryId) || galleryId.length() >= sizeof(pendingGalleryId)) {
        plog::log("bad gallery id, ignoring");
        pendingGalleryId[0] = '\0';
      } else {
        strncpy(pendingGalleryId, galleryId.c_str(), sizeof(pendingGalleryId) - 1);
        pendingGalleryId[sizeof(pendingGalleryId) - 1] = '\0';
      }

      // Persisted BEFORE any motion: from here on a reset can recover the job
      // instead of losing it with the queue item that no longer exists server-side.
      if (pendingGalleryId[0]) jobSavePending(bitmap, pendingGalleryId, 1);
      runPendingJob(bitmap, galleryId.c_str());
    } else {
      // MUST back off here. loop() ends immediately below, so returning with no
      // delay re-enters fetchNext() at once — and next.php POPS A QUEUE ITEM per
      // call. A single short/corrupt body would therefore drain the whole queue
      // in seconds, stranding a pending gallery entry (and an unsendable bound
      // submitter email) for every item. Matches every sibling error path.
      plog::logf("bad decode length: %d", decoded);
      delay(10000);
    }
  } else {
    delay(10000);
  }
}

bool isDigitsOnlyId(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s.charAt(i);
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool fetchNext(int& outStatus, String& outGalleryId, String& outBody) {
  WiFiClientSecure ssl;
  parSecure(ssl);
  if (!ssl.connect(SERVER, PORT)) {
    plog::log("next.php connect() failed");
    return false;
  }

  // POST (not GET) so Cloudflare / any intermediate proxy won't replay the
  // request on origin error — next.php pops a queue item per call, so a
  // silent retry drains items the Arduino never sees.
  ssl.print("POST /next.php HTTP/1.1\r\n");
  ssl.print("Host: ");
  ssl.print(SERVER);
  ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  // next.php is authenticated as of 2026-08-31 — it pops a queue item per call,
  // so it must not be open to the world. FLASH THIS SKETCH BEFORE deploying the
  // server change: old firmware sends no secret and would get 401 on every poll.
  ssl.print("X-Snapshot-Secret: ");
  ssl.print(SNAPSHOT_SECRET);
  ssl.print("\r\n");
  ssl.print("Content-Length: 0\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");

  // Wait for first byte (with timeout)
  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 15000) {
      plog::log("next.php response timeout");
      ssl.stop();
      return false;
    }
    delay(10);
  }

  // Status line: "HTTP/1.1 200 OK"
  String statusLine = ssl.readStringUntil('\n');
  int sp1 = statusLine.indexOf(' ');
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) {
    ssl.stop();
    return false;
  }
  outStatus = statusLine.substring(sp1 + 1, sp2).toInt();

  // Headers — cap total header bytes at 4 KB so a misbehaving server (or a
  // malicious response) can't grow the String until we OOM. Cloudflare's real
  // response headers come in well under this.
  const size_t MAX_HEADER_BYTES = 4096;
  size_t headerBytes = 0;
  bool chunked = false;
  while (ssl.connected()) {
    String line = ssl.readStringUntil('\n');
    headerBytes += line.length() + 1;
    if (headerBytes > MAX_HEADER_BYTES) {
      plog::log("next.php headers too large");
      ssl.stop();
      return false;
    }
    line.trim();
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    name.trim();
    value.trim();
    if (name.equalsIgnoreCase("X-Gallery-Id")) outGalleryId = value;
    if (name.equalsIgnoreCase("Transfer-Encoding") && value.equalsIgnoreCase("chunked")) chunked = true;
  }

  // Body: read until connection closes (Connection: close), decoding chunked
  // if needed. Both paths bail after BODY_TIMEOUT_MS without any forward
  // progress — without this, a stuck TLS connection mid-body hangs forever.
  const unsigned long BODY_TIMEOUT_MS = 60000;
  outBody = "";
  if (chunked) {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        plog::log("next.php chunked body timeout");
        ssl.stop();
        return false;
      }
      String sizeLine = ssl.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      lastProgress = millis();
      while (chunkSize > 0) {
        if (ssl.available()) {
          outBody += (char)ssl.read();
          chunkSize--;
          lastProgress = millis();
        } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
          plog::log("next.php chunk read timeout");
          ssl.stop();
          return false;
        }
      }
      // Trailing \r\n — read with a short timeout so a slow server can't
      // wedge us, and a missing CRLF doesn't desync the next chunk header.
      unsigned long tr = millis();
      int got = 0;
      while (got < 2 && millis() - tr < 1000) {
        if (ssl.available()) {
          ssl.read();
          got++;
        }
      }
    }
  } else {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (ssl.available()) {
        outBody += (char)ssl.read();
        lastProgress = millis();
      } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        plog::log("next.php body timeout");
        ssl.stop();
        return false;
      }
    }
  }

  outBody.trim();
  ssl.stop();
  return true;
}
