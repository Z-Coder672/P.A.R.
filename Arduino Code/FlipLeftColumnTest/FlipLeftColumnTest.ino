// Flips ONE column of the 37x18 grid, bottom to top, then stops. No scan, no
// bitmap, no classifier. A quick single-column stand-in for FlipAllTest when
// you want to check flip geometry without paying for all 666 cells.
//
// DERIVED FROM FlipAllTest.ino AND KEPT IDENTICAL TO IT except for the column
// restriction below — same release angle, same per-cell lidar compensation,
// same FLIP_TARGET_OFFSET_X, same mirrored-flip-on-LTR-rows logic, same unload.
// That is the whole point: results here transfer directly to a full-board run.
// Re-sync it whenever FlipAllTest's motion changes.
//
// !! REWRITTEN 2026-08-03. The previous version of this sketch predated the
// !! lidar work and had drifted badly out of sync: SERVO_US_RELEASE 1018 (~46
// !! deg) vs FlipAllTest's 807 (~25.5 deg), no standoff compensation, and it
// !! applied flipSkewX() which FlipAllTest does not use at all. Flipping with
// !! it would have driven the arm ~20 deg deeper than any recent run.
// !! Its original job was calibrating FLIP_SKEW_X_TOP against an UNcompensated
// !! baseline; that baseline is gone. Recover it from git if you need it.

#include <esp_system.h>      // esp_reset_reason()
#include "persistent_log.h"

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131, so this anchors the grid

// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°): REST≈2°,
// RELEASE≈25.5°, ENGAGE=75°.
const int SERVO_US_REST    = 565;
// RELEASE is a BASE angle that lidar compensation then shifts per cell. It is
// chosen so the bottom-left cell (0,17) — standoff 40.31 mm, i.e. -0.19 mm from
// LIDAR_REF_MM — comes out of compensatedUs() at 802µs = 25.02°, the commanded
// release angle the rig is tuned for. Uncompensated it is 25.51°.
const int SERVO_US_RELEASE = 807;
// ENGAGE is deliberately NOT lidar-compensated — the 90° stage-1 rotation only
// has to clear the disc, so it is a fixed 75° (1317µs = 74.97°) everywhere.
const int SERVO_US_ENGAGE  = 1317;  // 75°, uncompensated
const int SERVO_90_DEG_SETTLE_MS  = 300;
const int SERVO_50_DEG_SETTLE_MS  = 100;
// Lowered arm angle ~10° below RELEASE (scan sweep + second-catch pass in
// PARMain; second-catch only here). 544–2400µs over 0–180° (~10.3µs/°), so
// 10° ≈ 103µs. Mirrors PARMain.ino.
const int SERVO_US_10_DEG = 103;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;
// Extra travel on the CATCH stroke — the one that runs with the arm down at
// RELEASE, sweeping back across the cell to drive the half-rotated squisk
// through its final 90 deg. The clearing slide (which runs with the arm at REST,
// and which the comment below unhelpfully calls the "release stroke") is
// UNCHANGED at FLIP_OFFSET_X; only the return is lengthened, so the arm sweeps
// this far PAST the cell origin instead of stopping on it.
//
// Set 2.5 mm on 2026-08-07 to complete the rotation more consistently. Each cell
// re-establishes absolute X with moveTo(fx) at the top of flipDisc, so the extra
// travel cannot accumulate across cells. It IS clamped against both soft limits
// below, same as dx.
const float FLIP_CATCH_EXTRA_X = 3.5f;
// Inverted flip, applied on LEFT-TO-RIGHT sweep rows: the clearing slide runs
// -X and the catch/return slide runs +X, the opposite of the original flip.
// Two effects: the catch drives the squisk through its final 90 deg in the
// opposite rotational sense, so LTR rows unwind the column-rod twist the RTL
// rows wind in; and the return stroke now ends in the direction the sweep is
// already heading, so an LTR row stops backtracking FLIP_OFFSET_X before every
// next cell. (RTL rows keep the original flip, whose -X return already points
// along their sweep.) Approaching from the other side puts the arm on the
// opposite face of the squisk, so the flip X target shifts right by this much
// on inverted rows to keep the contact geometry identical. Mirrors PARMain.ino.
const float FLIP_INVERT_OFFSET_X = 11.0f;
// Direction-dependent X trim for the NON-inverted (RTL) rows. The bottom row is
// swept with ltr=true, i.e. INVERTED, and its landing positions were confirmed
// correct on the rig — so the inverted direction is the reference and this shifts
// only the other one. Negative = left / toward -X / toward the homing corner.
// Set -0.5 mm on 2026-08-07 from a full-board run: RTL rows landed slightly right
// of the LTR rows' contact point. This is separate from FLIP_INVERT_OFFSET_X,
// which compensates for approaching the squisk from the opposite face; this one
// is a residual registration difference between the two sweep directions.
const float FLIP_NONINVERT_OFFSET_X = -0.5f;
// Global flip-target trim. Shifts the absolute X the flip head drives to for
// EVERY cell, +X = right = away from the homing corner. Independent of
// FLIP_OFFSET_X (the relative clear/catch stroke) and of FLIP_INVERT_OFFSET_X
// (which applies to inverted rows only) — this one moves the landing position
// itself. Bumped to 0.5 mm on 2026-08-03: the arm was landing marginally left
// of the squisk centre. Keep FlipAllTest and FlipAllMaskedTest in sync.
const float FLIP_TARGET_OFFSET_X = 2.0f;

// Column-lean skew. The disc columns are not perfectly square to the flip head:
// the higher up the board a cell sits, the further LEFT (-X, toward homing) its
// true flip position is. Modelled as a linear X shift in physical height,
// ANCHORED AT THE BOTTOM ROW: 0 at gy = GRID_H-1, FLIP_SKEW_X_TOP at gy = 0.
// (bitmap y=0 is the TOP row, and physical Y increases upward.)
//
// Set 2026-08-03 from a column-1 run: with FLIP_TARGET_OFFSET_X alone the top
// landed correctly and the bottom needed 1.5 mm more to the right. That is
// expressed here as "+1.5 mm everywhere, then 1.5 mm back off at the top" —
// hence FLIP_TARGET_OFFSET_X 0.5 -> 2.0 and this term at -1.5. Net: the top row
// is unchanged at +0.5 mm, the bottom row gains +1.5 mm.
//
// Same sign convention as PARMain's FLIP_SKEW_X_TOP, but NOT the same value
// (PARMain still carries -2.0 and a different release-angle regime) — do not
// blind-copy between them.
const float FLIP_SKEW_X_TOP = -1.5f;

static inline float flipSkewX(int gy) {
  float heightFrac = (float)((GRID_H - 1) - gy) / (float)(GRID_H - 1);
  return FLIP_SKEW_X_TOP * heightFrac;
}

// The single column this sketch sweeps, 0..GRID_W-1. 0 = leftmost (the old
// hardcoded behaviour), 36 = rightmost. Everything else is untouched, so the
// cells flipped here are exactly the cells FlipAllTest would flip in this
// column — including each row's serpentine `inverted` sense (see below).
const int TEST_COLUMN = 1;
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
// `inverted` and not on the column alone.
//
// History: this was originally both outer columns unconditionally, then briefly
// the right column only. Both applied the 4 mm on rows that back inward.
const float FLIP_UNLOAD_X_EDGE = 4.0f;
static inline bool usesEdgeUnload(int gx, bool inverted) {
  return inverted ? (gx == GRID_W - 1) : (gx == 0);
}

// ---------------------------------------------------------------- lidar standoff
// Per-cell servo compensation from the VL53L4CD scan.
//   0 = OFF everywhere            (uncompensated control firmware)
//   1 = ON everywhere             (runs 2 and 3)
//   2 = randomized half-board     (paired trial -- see FlipAllMaskedTest.ino)
//
// Mode 2 exists because runs 2 and 3 -- the same firmware run twice -- produced
// 9 and 14 failures against the uncompensated run's 17. Run-to-run spread was as
// large as the effect being chased, so comparing whole runs cannot resolve it.
// Splitting the board puts both arms under identical conditions in a single run,
// which removes that variance entirely and makes each run its own paired trial.
// That measurement now lives in its own sketch (FlipAllMaskedTest.ino); this one
// compensates the whole board.
#define LIDAR_COMP_MODE 1

// Flip-arm geometry. The arm is a lever pivoting against the disc platform; at
// ARM_FLAT_DEG it lies parallel to the platform, so its perpendicular reach is
//     reach(theta) = ARM_LEN_MM * sin(theta - ARM_FLAT_DEG)
// A cell whose standoff exceeds LIDAR_REF_MM by d needs d more reach, so the
// compensated angle solves
//     ARM_LEN_MM*sin(theta' - FLAT) = ARM_LEN_MM*sin(theta - FLAT) + d
// -> theta' = FLAT + asin( sin(theta - FLAT) + d/ARM_LEN_MM )
const float ARM_LEN_MM   = 25.0f;
const float ARM_FLAT_DEG = 23.0f;
// GLOBAL BOARD TRIM. Extra linear reach demanded of the pusher pin's travel,
// applied to every cell. Two things set it, and only the first is geometric:
//
//   1. Pin length. The pin was physically shortened (2026-08-03), so its tip
//      sits further back for the same servo angle and every release has to
//      travel further out to make contact.
//
//   2. ROD COMPLIANCE, which is what actually caps it (2026-08-04). The discs
//      hang on vertical rods, and during the release stroke the rod flexes out
//      of the way. How much it flexes depends on where along the rod the cell
//      sits: mid-span rods give way easily, but THE BOTTOM ROW IS CLOSEST TO
//      THE MOUNTING POINT OF ANY ROW, so its rods are stiffest and barely
//      deflect. Reach that is merely generous mid-board becomes an over-push
//      down there — the bottom row was rotating to roughly 270 deg instead of
//      180 at 2.0 mm, even though the lidar table commands it the SMALLEST
//      angle on the whole board (837 us vs 893 at the top). So this is NOT a
//      standoff problem and no change to LIDAR_FIT_CMM fixes it; do not go
//      looking there again.
//
// REPEATABILITY TEST (2026-08-04). Back to 2.0 to re-run the exact config of
// the earlier trim-2.0 pass and see whether the physical result repeats. The
// firmware is deterministic, so the commanded values WILL be identical — logged
// row 17 was 805..868 us, mean 837.2. If the flips behave differently this time
// with the same numbers, the variability is mechanical, not in the code.
//
// Observed so far at row 17 (matched cells, servo miss=0 in every run, so no
// dropped commands):
//     mean 786.5 us (trim 0.0, cubic, 12 cells) -> flipped acceptably
//     mean 813.5 us (trim 1.0, rod,  37 cells) -> under-reached badly
//     mean 837.2 us (trim 2.0, rod,  37 cells) -> over-pushed to ~270 deg
// That ordering is NOT monotonic in reach, which is what this re-run is meant
// to test: either 786-vs-813 is real and the mechanism is mechanical, or the
// trim-0 result rests on too few cells (12) to compare against 37.
const float RELEASE_EXTRA_REACH_MM = 2.0f;
// Reach trim applied ONLY to the two extreme rows (top and bottom). Both sit
// nearest their rod mounts, where the rod is stiffest and deflects least, so they
// need slightly less reach than the interior to avoid over-pushing. Rows 1..16
// are untouched. Added 2026-08-07 after a clean full-board run at trim 2.0.
// Worth 0.5 mm ~= -12 us of commanded RELEASE on those two rows only.
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
// !! Their clear-channel readings were 3291..5955 against the then-current threshold of 6000 (now SCAN_ON_BLUE_MAX=3535),
// !! and 155 of 666 cells sit in a 5000-9000 grey zone — the ~10x separation the
// !! threshold assumes has degraded badly. Re-tuned: SCAN_ON_BLUE_MAX is now 3535.
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

// Fitted standoff per cell, hundredths of a mm, [y][x]. Plain uniform-weight
// least-squares CUBIC fits of the PASS-3 scan (2026-07-28, one cubic per column
// over its 18 rows), NOT the raw per-cell readings -- the fit removes sensor
// noise and keeps only the rod's real shape. Cubic rms 0.432 mm (pass 2 was
// 0.497 mm against a 0.600 mm vertical-pair noise floor).
//
// The pass-3 board was UNIFORMLY BLANK -- all 666 cells read cyan-back, clear
// channel 10763..19089 against the 6000 threshold, so there is no black-back
// population in this scan. The cyan-back offset of -20.0 mm is applied here
// rather than in firmware (the scan ran with LIDAR_APPLY_CALIBRATION 0), which
// keeps the table in the same absolute frame as the pass-2 one it replaces.
//
// CAVEAT: because every cell was cyan-back, this table cannot say anything
// about the black-back offset. The calibration squares put the true colour bias
// at 9.20 mm (cyan reads that much farther than black at identical standoff),
// vs the 7.2 mm the retired -12.8/-20.0 pair assumed -- so a mixed-colour board
// would need that re-derived before its geometry could be trusted.
// !! ROD-BEND MODEL TABLE (2026-08-04) — NOT the cubic. All three flip sketches
// !! carry this same rod table for a rig comparison; they remain in sync.
// !!   shape : pin-ended rod, d(s) = a + b·s + A·sin(πs), 3 params per column
// !! Fit quality on pass 4: rod RMS 0.572 mm / dof-adjusted σ 0.627, against the
// !! cubic's 0.513 / 0.582 and a 0.600 mm vertical-pair noise floor — so the rod
// !! leaves slightly more structure unexplained. Per-cell commanded RELEASE sits
// !! −26 .. +25 µs from the cubic table. Restore the cubic from the scan
// !! artifact's "Firmware output" block, or from git, when the comparison ends.
const int16_t LIDAR_FIT_CMM[GRID_H][GRID_W] = {
  { 4003, 3943, 4061, 3967, 3990, 4015, 3904, 3904, 3972, 3938, 3967, 3814, 3974, 4008, 3915, 4037, 3970, 3936, 4069, 4024, 3920, 3945, 3881, 3903, 3790, 3916, 3957, 3857, 3909, 3789, 3860, 3911, 3941, 3850, 3885, 3782, 3949 },
  { 3963, 3913, 4018, 3929, 3936, 3959, 3869, 3859, 3949, 3946, 3937, 3787, 3961, 3988, 3874, 4007, 3930, 3920, 4025, 3994, 3890, 3913, 3844, 3873, 3787, 3892, 3921, 3827, 3885, 3780, 3820, 3894, 3910, 3843, 3885, 3739, 3973 },
  { 3924, 3883, 3976, 3892, 3883, 3905, 3834, 3815, 3928, 3953, 3908, 3760, 3949, 3969, 3834, 3978, 3892, 3903, 3982, 3963, 3861, 3882, 3807, 3843, 3784, 3870, 3887, 3797, 3862, 3772, 3780, 3876, 3879, 3836, 3885, 3697, 3994 },
  { 3886, 3853, 3935, 3856, 3833, 3853, 3801, 3773, 3906, 3959, 3879, 3735, 3936, 3949, 3796, 3949, 3855, 3886, 3941, 3934, 3834, 3852, 3772, 3814, 3780, 3848, 3854, 3770, 3840, 3763, 3743, 3860, 3850, 3828, 3884, 3658, 4014 },
  { 3849, 3825, 3897, 3821, 3786, 3804, 3770, 3734, 3885, 3962, 3852, 3712, 3923, 3931, 3760, 3921, 3820, 3869, 3902, 3905, 3809, 3824, 3740, 3788, 3777, 3827, 3824, 3744, 3820, 3754, 3710, 3843, 3822, 3820, 3882, 3623, 4029 },
  { 3815, 3798, 3862, 3788, 3743, 3759, 3742, 3700, 3866, 3963, 3826, 3692, 3910, 3913, 3728, 3894, 3788, 3852, 3866, 3878, 3787, 3798, 3710, 3764, 3773, 3808, 3797, 3723, 3800, 3746, 3680, 3827, 3796, 3811, 3879, 3593, 4040 },
  { 3782, 3772, 3830, 3757, 3705, 3720, 3717, 3671, 3847, 3960, 3803, 3674, 3895, 3897, 3699, 3867, 3759, 3834, 3834, 3852, 3769, 3774, 3684, 3743, 3770, 3792, 3774, 3705, 3783, 3738, 3656, 3812, 3773, 3800, 3875, 3569, 4046 },
  { 3751, 3748, 3802, 3728, 3674, 3686, 3695, 3648, 3830, 3953, 3781, 3660, 3880, 3881, 3675, 3842, 3734, 3815, 3806, 3829, 3755, 3753, 3663, 3726, 3766, 3777, 3756, 3691, 3768, 3730, 3638, 3797, 3753, 3788, 3869, 3552, 4045 },
  { 3723, 3726, 3778, 3701, 3648, 3658, 3678, 3632, 3814, 3942, 3763, 3650, 3863, 3867, 3656, 3817, 3713, 3795, 3782, 3808, 3745, 3735, 3646, 3712, 3761, 3764, 3742, 3682, 3755, 3723, 3625, 3782, 3735, 3774, 3861, 3543, 4038 },
  { 3697, 3706, 3759, 3677, 3628, 3636, 3664, 3622, 3799, 3927, 3746, 3644, 3846, 3855, 3641, 3794, 3696, 3775, 3763, 3788, 3739, 3721, 3633, 3702, 3757, 3754, 3733, 3678, 3745, 3716, 3619, 3769, 3721, 3760, 3852, 3541, 4024 },
  { 3674, 3688, 3743, 3656, 3616, 3621, 3655, 3619, 3786, 3908, 3732, 3642, 3828, 3843, 3631, 3772, 3683, 3754, 3749, 3771, 3738, 3710, 3625, 3697, 3752, 3747, 3729, 3678, 3737, 3709, 3619, 3756, 3710, 3743, 3840, 3546, 4002 },
  { 3654, 3671, 3732, 3637, 3609, 3612, 3649, 3622, 3774, 3884, 3721, 3644, 3808, 3833, 3626, 3752, 3674, 3733, 3738, 3756, 3741, 3701, 3621, 3694, 3748, 3742, 3730, 3684, 3732, 3703, 3626, 3743, 3701, 3725, 3828, 3560, 3975 },
  { 3636, 3656, 3725, 3620, 3608, 3608, 3647, 3630, 3764, 3856, 3712, 3649, 3788, 3824, 3625, 3732, 3669, 3711, 3732, 3743, 3748, 3696, 3622, 3696, 3742, 3739, 3735, 3693, 3728, 3697, 3638, 3732, 3696, 3706, 3813, 3579, 3941 },
  { 3620, 3643, 3721, 3606, 3612, 3609, 3648, 3645, 3754, 3826, 3705, 3657, 3768, 3816, 3628, 3713, 3668, 3688, 3730, 3732, 3759, 3693, 3626, 3700, 3737, 3738, 3745, 3706, 3727, 3691, 3655, 3720, 3693, 3686, 3798, 3606, 3901 },
  { 3606, 3631, 3720, 3593, 3620, 3615, 3652, 3663, 3746, 3792, 3699, 3668, 3746, 3810, 3635, 3695, 3669, 3665, 3730, 3722, 3773, 3692, 3634, 3707, 3732, 3739, 3757, 3723, 3728, 3686, 3676, 3710, 3692, 3664, 3781, 3637, 3857 },
  { 3593, 3620, 3721, 3581, 3632, 3624, 3658, 3685, 3738, 3756, 3695, 3681, 3724, 3803, 3644, 3678, 3672, 3641, 3734, 3712, 3789, 3693, 3644, 3716, 3726, 3741, 3772, 3742, 3729, 3681, 3701, 3699, 3692, 3642, 3763, 3672, 3809 },
  { 3581, 3609, 3724, 3570, 3646, 3635, 3665, 3710, 3731, 3718, 3692, 3695, 3702, 3798, 3654, 3661, 3676, 3617, 3738, 3704, 3807, 3696, 3655, 3726, 3721, 3744, 3789, 3763, 3732, 3676, 3728, 3689, 3694, 3619, 3744, 3710, 3759 },
  { 3569, 3599, 3727, 3560, 3661, 3647, 3673, 3735, 3724, 3680, 3689, 3710, 3680, 3792, 3666, 3644, 3682, 3593, 3744, 3697, 3825, 3698, 3667, 3737, 3715, 3747, 3807, 3785, 3734, 3671, 3756, 3679, 3696, 3596, 3726, 3750, 3707 },
};

// Randomized assignment for LIDAR_COMP_MODE 2: bit set = this cell gets the
// correction. 37x18 bits packed MSB-first into 84 bytes, the same packing as the
// display bitmaps. Generated offline with a fixed seed and STRATIFIED by
// |correction| so both arms carry the same mix of correction magnitudes -- an
// unstratified coin flip could hand one arm most of the large-correction cells,
// which are the only ones carrying signal. 332 of 666 cells are compensated;
// mean |correction| 0.647 mm compensated vs 0.654 mm control (re-stratified
// on the pass-2 corrections, since those are what the run actually applies).
// Byte checksum 0x29F9 -- printed at boot so the log records which mask ran.
const uint8_t COMP_MASK[84] = {
  0x74, 0x1D, 0x24, 0x2D, 0xAB, 0x21, 0x5F, 0xDA, 0xCE, 0xEC, 0xC4, 0xB3,
  0x24, 0x91, 0xD0, 0xBB, 0x47, 0x61, 0x67, 0xE3, 0x35, 0x31, 0x95, 0x01,
  0xC3, 0x7E, 0xCE, 0x0C, 0x99, 0x0B, 0x4D, 0x38, 0x4C, 0xB6, 0x9F, 0x54,
  0xCF, 0x7C, 0x1E, 0x63, 0x31, 0x7A, 0x8F, 0x8F, 0x8F, 0x63, 0x78, 0x03,
  0xCA, 0x93, 0x72, 0xF9, 0x80, 0x11, 0xCE, 0xFD, 0x60, 0xAC, 0xF5, 0x92,
  0xB9, 0xC5, 0x42, 0x64, 0xAD, 0x81, 0x9B, 0x75, 0x79, 0x83, 0xD7, 0xF4,
  0x2D, 0xA2, 0xE2, 0x29, 0xB5, 0xD6, 0x9D, 0x11, 0x64, 0x95, 0x8C, 0x00
};

static inline bool cellCompensated(int gx, int gy) {
  int i = gy * GRID_W + gx;
  return (COMP_MASK[i >> 3] >> (7 - (i & 7))) & 1;
}

// Compensated pulse width for a contact position at cell (gx,gy).
int compensatedUs(int baseUs, int gx, int gy) {
  // Pin-length trim first, and always — see RELEASE_EXTRA_REACH_MM.
  float delta = RELEASE_EXTRA_REACH_MM + edgeRowReachTrim(gy);
#if LIDAR_COMP_MODE == 0
  (void)gx;   // gy is used by edgeRowReachTrim above
#else
#if LIDAR_COMP_MODE == 2
  if (cellCompensated(gx, gy))
#endif
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
// second-catch back-move. Mirrors PARMain.ino — keep both in sync.
//#define FLIP_SECOND_CATCH

// Hardware UART2 (Serial2) to the ServoNano (matches PARMain.ino). D9 is NOT a
// PWM servo line any more — the SG90 hangs off a dedicated 5V Nano, and D9
// carries a 9600-baud TX of the µs value into its RX. Driving D9 with Servo.h
// makes the ServoNano decode the PWM edges as garbage frames and throw the arm
// to random angles mid-traverse.
const int SERVO_TX_PIN = D9;

// PORT (Arduino Nano ESP32): the RP2040 bit-banged this 9600-baud frame on D9
// with interrupts disabled (servoTxByte + SERVO_TX_BIT_US, both deleted). The
// ESP32-S3 GPIO matrix routes a real UART to any pin, so the link is now
// hardware Serial2 TX on the SAME physical D9 wire -- same 9600 8N1 framing, no
// ISR blackout, no bit-period tuning. RX is unused (-1): the link is still
// one-way; the ack is the separate D2 level line.

// Leading newline as well as trailing — see PARMain.ino. It terminates any
// partial line stranded in the ServoNano by a dropped byte, so two commands
// can never merge into one number.
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
    // Block until the last stop bit is actually on the wire, so this call
    // stays synchronous like the old bit-bang did -- callers time their
    // settle delay from here.
    Serial2.flush();
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}


// ---------------------------------------------------------------- servo ack
// ServoNano D3 --[1.8k]--+--> this pin (D2);  3.3k from that junction to GND.
// The divider is MANDATORY: the ESP32-S3 is not 5V tolerant either (abs max
// VDD+0.3 = 3.6 V, same as the RP2040 it replaced) and the ServoNano drives 5 V. 5.0*3.3/(1.8+3.3) = 3.24 V. See
// ServoNano.ino. The other direction (D9 -> ServoNano) needs nothing, since
// 3.3 V clears the AVR's V_IH of 0.6*Vcc = 3.0 V.
//
// The line is a LEVEL, not a UART: idle HIGH, held LOW for ~40 ms on every
// command the ServoNano accepts. We already know what we sent, so all we need
// is "it landed". INPUT_PULLUP so a broken or unfitted wire reads HIGH, i.e.
// "no ack" -- it fails loud rather than silently reporting success.
//
// SERVO_ACK_MODE  0 = off      (no wire fitted; original open-loop behaviour)
//                 1 = observe  (log every missing ack, keep running)
//                 2 = enforce  (retry FOREVER; never move without the ack)
// Mode 2 is live: the divider + ack wire are fitted.
#define SERVO_ACK_MODE 2
const int SERVO_ACK_PIN = D2;
const unsigned long SERVO_ACK_TIMEOUT_MS = 80;   // must exceed the ~6 ms frame
unsigned long servoAckMisses = 0;

#if SERVO_ACK_MODE > 0
// Let the previous command's 40 ms hold expire so it cannot be mistaken for ours.
static void servoAckWaitIdle() {
  unsigned long t0 = millis();
  while (digitalRead(SERVO_ACK_PIN) == LOW && millis() - t0 < 100) {}
}
static bool servoAckSeen() {
  unsigned long t0 = millis();
  while (millis() - t0 < SERVO_ACK_TIMEOUT_MS)
    if (digitalRead(SERVO_ACK_PIN) == LOW) return true;
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
  Serial.print("writeServoUs("); Serial.print(us); Serial.println(")");
#if SERVO_ACK_MODE == 0
  servoTxLine(us);
  delay(settle_ms);
#else
  servoAckWaitIdle();
  unsigned long t0 = millis();
  unsigned long attempt = 0;
  bool acked = false;
  do {
    attempt++;
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
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      // 25 mm starting X offset — matches P.A.R.Main.
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
  }
}

#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32

int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

void enqueue(int len) {
  cmdLengths[qTail] = len;
  qTail = (qTail + 1) % QUEUE_SIZE;
}

int dequeue() {
  int len = cmdLengths[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return len;
}

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;

    Serial.print("GRBL: ");
    Serial.println(resp);

    if (resp == "ok") {
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      Serial.print("!!! GRBL halted: ");
      Serial.println(resp);
      plog::logf("GRBL HALT: %.40s", resp.c_str());
      while (true)
        ;
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;

  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
  }

  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmdLen);

  Serial.print("Sent [buf:");
  Serial.print(bufferFill);
  Serial.print("]: ");
  Serial.println(cmd);
}

void waitForIdle() {
  while (bufferFill > 0) drainResponses();
}

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Travel to (targetX, targetY) such that any vertical (Y) component happens
// with X pinned to the nearest absolute machine limit (X = 0 or X = -X_TRAVEL).
// Emits pure-X → pure-Y → pure-X. Use for entry into a phase or any cross-row
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

void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Mirrors PARMain.ino flipDisc, including the second error-reduction catch.
// `inverted`: mirror the whole X excursion (dx = -FLIP_OFFSET_X, flip target
// shifted right by FLIP_INVERT_OFFSET_X) — pass it on LEFT-TO-RIGHT rows, so
// the return stroke ends the way the sweep is already heading.
// `catchByNextMove`: the second catch always runs OPPOSITE the return stroke,
// which under the mirror is against the sweep on both row directions — so it
// can never be folded into the caller's next move any more. Callers pass false;
// flipDisc emits its own +dx stroke and re-parks at REST.
void flipDisc(int gx, int gy, bool catchByNextMove, bool inverted) {
  // Mirrored-flip shift on inverted rows; the excursions below are relative
  // (G91), so only this absolute X target moves.
  float fx = grid[gy][gx].x + FLIP_TARGET_OFFSET_X + flipSkewX(gy)
            + (inverted ? FLIP_INVERT_OFFSET_X : FLIP_NONINVERT_OFFSET_X);
  moveTo(fx, grid[gy][gx].y);
  waitForMotion();

  // Release-stroke displacement, hoisted above the ENGAGE call because the
  // unload move needs its sign. Capped against BOTH soft limits —
  // inverted (LTR) rows slide toward -X_TRAVEL, plain (RTL) rows toward 0.
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
  // Second catch pass — drop the arm ~10° below RELEASE; emit our own +X
  // stroke only when the next move won't already provide it.
  writeServoUs(compensatedUs(SERVO_US_RELEASE2, gx, gy), SERVO_10_DEG_SETTLE_MS);
  if (!catchByNextMove) {
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
  // park it at REST regardless of catchByNextMove.
  (void)catchByNextMove;
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
#endif
}

// End-of-job park, servo already at REST.
//
// This used to serpentine all 18 rows top-to-bottom -- ~13 m of X traverse for a
// pass that never touches a disc, since the arm is parked the whole time. Its
// stated purpose was letting half-rotated discs settle on the gantry's vibration
// before the snapshot. That is a weak mechanism for a lot of travel and wear, so
// the default is now a direct park: X to the soft limit, then straight down Y.
// The X-limit-first order keeps the Y leg a pure-Y move at a soft limit, per the
// motion convention.
//
// Define FLIP_SETTLE_SWEEP to restore the old serpentine -- worth doing if the
// 90-degree count climbs after this change, since that would be evidence the
// settling pass was actually doing something.
//#define FLIP_SETTLE_SWEEP

void parkHead() {
#ifdef FLIP_SETTLE_SWEEP
  Serial.println("Release sweep (serpentine)...");
  bool ltr = true;
  moveToYSafe(grid[0][0].x, grid[0][0].y);
  for (int y = 0; y < GRID_H; y++) {
    int endCol = ltr ? GRID_W - 1 : 0;
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y + 1 < GRID_H) {
      moveToYSafe(grid[y + 1][endCol].x, grid[y + 1][endCol].y);
      ltr = !ltr;
    }
  }
#else
  Serial.println("Parking...");
  char cmd[40];
  // X to the negative soft limit first, so the Y leg is a pure-Y move there.
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", -X_TRAVEL);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", grid[GRID_H - 1][0].y);
  sendGcode(cmd);
  waitForMotion();
#endif
}


void setup() {
  Serial.begin(115200);
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);

  // Bring the servo TX UART up before anything else, then park the arm —
  // the ServoNano must see a clean line and a valid REST frame before the
  // gantry is allowed to move.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  pinMode(SERVO_ACK_PIN, INPUT_PULLUP);
  delay(100);
  servoTxLine(SERVO_US_REST);

  initGrid();

  // Mount the flash log and print whatever the PREVIOUS run left behind before
  // this run starts overwriting context. A run that ends in a GRBL halt or an
  // MCU reset leaves its tail here and nowhere else.
  plog::begin();
  Serial.println("--- previous run ---");
  plog::printBootDump(Serial);
  Serial.println("--- end previous run ---");
  plog::logf("BOOT eng%d rel%d tx%d ack%d comp%d unl%d/%d",
             SERVO_US_ENGAGE, SERVO_US_RELEASE, SERVO_TX_REPEATS,
             SERVO_ACK_MODE, LIDAR_COMP_MODE,
             (int)(FLIP_UNLOAD_X * 10.0f), (int)(FLIP_UNLOAD_X_EDGE * 10.0f));

  // Reset cause, ported from PARMain. A run that dies mid-sweep leaves NO reason
  // line in the log unless the reset was one of ours (GRBL stall etc.), so a
  // silent millis() rollback is otherwise unattributable — which is exactly what
  // happened on 2026-08-05: 83 cells in at trim 7.0, deepest release angle ever
  // run (rel up to 990 us), servo quit and then the whole rig did, with no ALARM,
  // no error, and miss=0. Brownout and firmware panic look identical in the log
  // without this. The ESP32-S3 records the cause in hardware:
  //   BROWNOUT -> supply sagged past the BOD threshold. Electrical, not firmware.
  //               Prime suspect here: a deep arm draws more servo stall current.
  //   POWERON  -> genuine cold start (plug-in, power cycle, RST button).
  //   PANIC    -> firmware crash. On the RP2040 a hard fault just locked up, so a
  //               reboot could never be firmware; on the ESP32 it can be.
  //   SW       -> one of our own esp_restart() paths.
  //   *_WDT    -> a watchdog fired; something blocked too long.
  // Keep the "reset cause: <NAME>" line shape — plog dumps are grepped for it.
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

  delay(2000);
  while (Serial1.available()) Serial1.read();

  // Record which compensation condition this run used -- the analysis has to
  // know, and a boot line in the log is the only thing that survives a reflash.
  {
    long sum = 0;
    for (int i = 0; i < 84; i++) sum += COMP_MASK[i];
    // Count what this build ACTUALLY compensates, not what the mask says — the
    // mask only gates cells in mode 2.
    int nComp = 0;
    for (int y = 0; y < GRID_H; y++)
      for (int x = 0; x < GRID_W; x++)
        // Count LIDAR-compensated cells specifically. Do NOT test
        // "compensatedUs != SERVO_US_RELEASE" — RELEASE_EXTRA_REACH_MM now
        // shifts every cell, so that test would report all 666 in mode 2 and
        // destroy the paired-trial bookkeeping.
      {
        // BRACED deliberately: the two for-loops above have no braces of their
        // own, so a multi-statement body would attach only its first statement
        // and leave the rest outside the loop (that is exactly how the mode-0
        // build broke — `(void)y;` ended up out of scope).
#if LIDAR_COMP_MODE == 1
        nComp++;
#elif LIDAR_COMP_MODE == 2
        if (cellCompensated(x, y)) nComp++;
#else
        (void)x; (void)y;   // no compensation in this build; nComp stays 0
#endif
      }
    Serial.print("LIDAR_COMP_MODE="); Serial.print(LIDAR_COMP_MODE);
    Serial.print(" ref="); Serial.print(LIDAR_REF_MM);
    Serial.print("mm maskChecksum=0x"); Serial.print(sum, HEX);
    Serial.print(" compensatedCells="); Serial.println(nComp);
    // Servo-link build variables, logged for the same reason as the block above:
    // a boot line is the only record of which firmware a run actually used.
    Serial.print("SERVO engage="); Serial.print(SERVO_US_ENGAGE);
    Serial.print(" release="); Serial.print(SERVO_US_RELEASE);
    Serial.print(" txRepeats="); Serial.print(SERVO_TX_REPEATS);
    Serial.print(" ackMode="); Serial.println(SERVO_ACK_MODE);
  }

  Serial.println("Homing...");
  sendGcode("$H");
  waitForIdle();
  Serial.println("Homed.");
  unsigned long startTime = millis();

  // $1=255 keeps steppers energized between motions; released at end via $1=0
  // + a tiny jog so the disable actually takes effect on the next idle.
  sendGcode("$1=255");
  waitForIdle();  // sync past the $1 EEPROM write before pipelining more — grbl
                  // disables interrupts during the commit and drops Serial1 RX
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();

  writeServoUs(SERVO_US_REST, 1000);

  // Bottom-to-top (bitmap y = GRID_H-1 → 0), every cell in every row. Rows
  // alternate sweep direction (serpentine) so the only Y travel between rows
  // happens at an X soft-limit via moveToYSafe.
  bool ltr = true;
  for (int y = GRID_H - 1; y >= 0; y--) {
    // Single-column sweep. xStart == xEnd, so the loop below runs exactly one
    // cell per row. `ltr` is still toggled per row exactly as in FlipAllTest,
    // so each cell gets the SAME mirrored/plain flip sense it would get in a
    // full-board run — that is what makes these results transferable.
    int xStart = TEST_COLUMN;
    int xEnd   = TEST_COLUMN;
    int xStep  = ltr ? +1 : -1;

    for (int x = xStart; (xStep > 0) ? (x <= xEnd) : (x >= xEnd); x += xStep) {
      Serial.print("Flipping (");
      Serial.print(x);
      Serial.print(",");
      Serial.print(y);
      Serial.println(")");
      // The mirrored (LTR) flip ends its return stroke in the sweep direction,
      // so the second catch — which always runs OPPOSITE that return — now runs
      // AGAINST the sweep on both row directions. It can never be folded into
      // the caller's next move any more, so this is always false. That's the
      // trade the mirror buys: the fold-in saved a stroke only on the rows that
      // were paying a backtrack stroke to begin with.
      const bool catchByNextMove = false;
      flipDisc(x, y, catchByNextMove, ltr);  // mirrored flip on LTR rows
      waitForIdle();
    }

    if (y > 0) {
      moveToYSafe(grid[y - 1][xEnd].x, grid[y - 1][xEnd].y);
      ltr = !ltr;
    }
  }

  unsigned long elapsed = millis() - startTime;
  Serial.print("Total time: ");
  Serial.print(elapsed / 1000UL);
  Serial.print(".");
  Serial.print((elapsed % 1000) / 100);
  Serial.println("s");

  parkHead();

  // Release steppers. $1=0 only takes effect on the next idle transition, so
  // the tiny X-0.1 jog (safe — work area is entirely negative X) gives GRBL
  // the motion→idle edge it needs to disable the drivers.
  sendGcode("$1=0");
  sendGcode("G91");
  sendGcode("G0 X-0.1");
  sendGcode("G90");
  waitForIdle();

  Serial.println("Done.");
}

void loop() {}
