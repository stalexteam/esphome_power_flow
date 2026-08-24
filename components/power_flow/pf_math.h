#pragma once
//
// power_flow — dependency-free core math.
//
// Nothing in this header may include an ESPHome, LVGL or ESP-IDF header. It is
// compiled off-target by DEV/host/run.sh so the solver can be exercised against
// synthetic timelines in about a second. Keep it that way: the moment this file
// needs a platform header, the test harness dies.
//
// Section references (§) point at the specification, task.md.
// The signatures below are a frozen contract — power_flow.h and __init__.py are
// written against them. Adding declarations is fine; changing an existing one
// silently breaks another agent's work.
//
// Time is milliseconds from esphome::millis(), which wraps every ~49.7 days.
// Always compare with unsigned subtraction, (uint32_t)(now - then) >= span,
// never now >= then + span.
//
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace power_flow {

using ms_t = uint32_t;

/// NaN is how a Home Assistant `unavailable` reaches us (§2), so it is the one
/// and only "no value" marker in this component. Never substitute 0.
bool is_valid(float v);

// ---------------------------------------------------------------------------
// §6.5  Time-weighted averaging
// ---------------------------------------------------------------------------
//
// A sample mean is wrong here: ACIN delivers ~25 samples a minute and
// WallSocket_automation sometimes zero, so a mean hands the window to whichever
// device is chattier. Each value is weighted by how long it was held.
//
// A NaN sample opens a gap: the interval it covers carries no value and is
// excluded from both numerator and denominator. coverage() reports how much of
// the window was actually covered, so the caller can decide whether the average
// means anything (§6.9).
//
class TimeWeightedAverage {
 public:
  void set_window(ms_t window);
  ms_t window() const;

  /// Feed a sample. `value` may be NaN to mark the source unavailable from this
  /// instant. Samples must arrive in non-decreasing `now` order.
  void add(float value, ms_t now);

  /// Time-weighted mean over the valid part of the window ending at `now`.
  /// NaN when nothing valid is in the window.
  float average(ms_t now) const;

  /// The same over an arbitrary shorter span. One buffer answers both: the
  /// window is only an integration limit, not a property of the samples.
  ///
  /// This exists because §6.5 over-applied its own rule. A common 60 s window is
  /// what makes a *difference* meaningful — two meters sampled at different
  /// instants cannot be subtracted — but a value that is measured directly and
  /// displayed directly is never subtracted from anything, and needs only enough
  /// smoothing to stop it flickering between reports. Sixty seconds of it makes
  /// the panel visibly sluggish when a real load steps.
  ///
  /// A span longer than window() is clamped to it; there are no samples beyond.
  float average(ms_t now, ms_t span) const;

  /// Fraction [0..1] of the window covered by a valid held value.
  float coverage(ms_t now) const;

  /// Most recent raw sample — the fast path (§6.5). NaN if unavailable.
  float last() const;
  ms_t last_update() const;
  bool has_data() const;

  void reset();

 private:
  struct Sample {
    ms_t t;
    float v;
  };
  std::vector<Sample> samples_;
  ms_t window_{60000};
};

// ---------------------------------------------------------------------------
// §6.4  Learned staleness
// ---------------------------------------------------------------------------
//
// Home Assistant holds the last value until the device's own timeout expires
// (minutes). The value cannot reveal that it is old, but its arrival time can:
// track a rolling median of inter-arrival intervals and flag the source when
// nothing has arrived for `factor` times that median.
//
// The statistic is a high quantile, not the median. The median describes a
// typical *burst*; what this needs is a typical *silence*, and on a source that
// publishes several updates a second while a load changes and then nothing for
// minutes, those differ by a factor of sixty. Measured on six hours of real
// arrivals, a median-based rule flagged one socket as stale 94 % of the time.
//
// So the detector arms only on sources that are actually regular: if the 90th
// percentile of intervals exceeds `dispersion_limit` times the median, the
// source is event-driven, its silence carries no information, and the detector
// disarms rather than guessing. `stale_after` overrides all of this.
//
class StalenessDetector {
 public:
  /// Fixed timeout in ms; 0 (default) means learn from arrivals.
  void set_stale_after(ms_t stale_after);
  /// Multiplier applied to the learned quantile. Default 2.0 (§6.4).
  void set_factor(float factor);
  /// Arm only while p90 <= this many medians. Default 4.0; 0 disables the test
  /// and arms unconditionally.
  void set_dispersion_limit(float limit);
  /// Number of intervals kept for the median. Default 16.
  void set_history(size_t n);

  void on_sample(ms_t now);
  bool stale(ms_t now) const;

  /// Learned 90th-percentile interval, 0 until enough intervals have been seen.
  ms_t learned_interval() const;
  /// The timeout currently in force, learned or overridden. 0 means not armed —
  /// either too few samples yet, or a source too irregular to judge.
  ms_t effective_timeout() const;
  /// False when the dispersion test has disarmed this source. Worth surfacing:
  /// it is the difference between "this reading is fresh" and "freshness is not
  /// a question this source can answer".
  bool armed() const;

  void reset();

 private:
  std::vector<ms_t> intervals_;
  size_t history_{16};
  size_t next_{0};
  ms_t last_{0};
  ms_t stale_after_{0};
  float factor_{2.0f};
  float dispersion_limit_{4.0f};
  bool seen_{false};
};

// ---------------------------------------------------------------------------
// §6.1 / §6.3  Edge state
// ---------------------------------------------------------------------------

/// A Home Assistant switch as seen from here. `unknown` and `null` are distinct
/// from `off` and must stay that way: the two Moes plugs publish `null` forever
/// and an edge must not be drawn open because of it (§6.1, decision log #6).
enum class SwitchState : uint8_t {
  ABSENT,   ///< no switch entity configured
  ON,
  OFF,      ///< the only value that opens an edge
  UNKNOWN,  ///< entity exists but reports `unknown` / `null` / `unavailable`
};

/// What an unavailable meter means for this terminal (§6.3, decision log #5).
enum class UnavailablePolicy : uint8_t {
  DE_ENERGIZED,  ///< default: the meter is powered from the line it measures
  NO_DATA,       ///< telemetry bridge (BLE gateway): the line is still there
};

enum class EdgeState : uint8_t {
  DE_ENERGIZED,  ///< meter NaN, policy DE_ENERGIZED — grey, cross, out of balance
  OPEN,          ///< explicit switch off — grey, cross, out of balance
  NO_DATA,       ///< meter NaN, policy NO_DATA — dash, grey, node stays alive
  ACTIVE,        ///< |P| above the idle threshold — coloured, dots move
  IDLE,          ///< closed but below the threshold — thin grey, no cross
};

struct EdgeInput {
  /// Averaged power, slow path (§6.5). NaN if there is no usable average.
  float power{0.0f};
  /// Whether the newest raw sample is a real number — fast path. This, not
  /// `power`, decides availability: a 60 s average must never delay a cross.
  bool meter_available{false};
  /// False when the terminal declares no meter at all (e.g. a pure `auto` edge
  /// whose value comes from the balance). Then `power` is the solved value.
  bool has_meter{true};
  SwitchState sw{SwitchState::ABSENT};
  float idle_below{3.0f};
  UnavailablePolicy on_unavailable{UnavailablePolicy::DE_ENERGIZED};
};

EdgeState evaluate_edge(const EdgeInput &in);

/// True when the edge carries a number the balance may use. OPEN and
/// DE_ENERGIZED edges are excluded, which is not the same as contributing
/// zero — see solve_balance().
bool contributes_to_balance(EdgeState s);

// ---------------------------------------------------------------------------
// §4  Balance solver
// ---------------------------------------------------------------------------
//
// Every node obeys: sum of inflows = sum of outflows. One edge per node may be
// declared `power: auto` and is solved from the rest.
//
// The three-way distinction below is the heart of it and is easy to get wrong:
//
//   included = false         the edge is physically not there (open contact,
//                            de-energized line). It contributes exactly 0 and
//                            the node still solves.
//   included = true, NaN     the edge is there and carrying something we cannot
//                            read (a NO_DATA telemetry bridge, a stale meter).
//                            The node does NOT solve — the auto value becomes a
//                            dash. Treating this as zero would invent load that
//                            does not exist (§4, §6.9).
//   included = true, number  normal term.
//
struct BalanceTerm {
  float power{0.0f};   ///< signed value as reported; NaN = present but unknown
  int8_t sign{1};      ///< +1 if the term flows into the node, -1 if out of it
  bool included{true};
  bool is_auto{false};
};

enum class BalanceFault : uint8_t {
  NONE,
  NO_AUTO,        ///< nothing to solve for; caller wanted a value anyway
  MULTIPLE_AUTO,  ///< must be rejected at compile time; a runtime guard only
  MISSING_INPUT,  ///< an included term is NaN
  NEGATIVE,       ///< solved below zero — inputs are bad
  ABOVE_CEILING,  ///< solved above the plausible maximum
  AUTO_EXCLUDED,  ///< the auto edge is itself open or dead: it carries 0, and
                  ///< whatever the other terms fail to cancel is somebody
                  ///< else's error, not this edge's flow
};

struct BalanceResult {
  float value{0.0f};      ///< solved power for the auto edge, NaN if unsolved
  bool solved{false};
  bool plausible{false};  ///< false ⇒ flag the diagram unreliable, do not draw
  BalanceFault fault{BalanceFault::NONE};
  /// Signed sum of the included known terms — what the auto edge has to cancel
  /// for the node to close. Meaningful even when nothing was solved, which is
  /// the only way to tell a small inconsistency from a large one.
  float residual{NAN};
};

/// `ceiling` is the largest physically plausible magnitude for the auto edge.
/// NaN means no ceiling; so does a negative value, which is a config error the
/// schema rejects and this only tolerates as defence in depth. A result outside [0, ceiling] is still returned so
/// the caller can log it, but `plausible` is false.
BalanceResult solve_balance(const BalanceTerm *terms, size_t n, float ceiling);

// ---------------------------------------------------------------------------
// §6.6  Self-consumption baseline
// ---------------------------------------------------------------------------
//
//     P_in − P_out = a + b · P_out
//
// `a` is the inverter's genuine standing draw (fans, logic, the sine generator
// kept alive for instant transfer) — a load, not a loss (decision log #7).
// `b` is the relative calibration mismatch between two meters from different
// makers, which otherwise masquerades as consumption and grows with load.
//
// Samples are only valid while the grid is energized, the bypass is closed and
// the battery is inside a deadband — the moment the battery moves, the equation
// gains a term. With PV present and unmetered, sample only at night (§6.8).
// Those preconditions are the caller's to check; add_sample() enforces only the
// deadband and the presence of usable numbers.
//
// Weight is inverse to throughput: at 1 kW we are subtracting 1000 from 1050
// with meters good to ~1%, so high-load samples are near-worthless.
//
class BaselineFit {
 public:
  void set_deadband(float watts);      ///< |P_battery| below this = at rest
  void set_min_samples(size_t n);
  void set_weight_floor(float watts);  ///< clamp for the 1/P weight at P -> 0
  /// Weighted standard deviation of P_out, as a fraction of its weighted mean,
  /// below which the fit refuses to call itself valid. Default 0.25.
  ///
  /// Two coefficients cannot be separated from a narrow cluster: the line
  /// through it is right where the data is and nonsense everywhere else. On
  /// this installation a whole day of history spans 0.545, while the median
  /// rolling hour spans 0.108 — and an hour of steady load is exactly when the
  /// fit was seen reporting a 530 W standing draw with a slope of -0.96.
  ///
  /// The existing absolute floor stays: it catches the degenerate case where
  /// every sample is identical, which no relative test can see.
  void set_min_spread(float fraction);

  /// Returns true if the sample was accepted. Rejection is normal and silent.
  bool add_sample(float p_in, float p_out, float p_battery);

  bool valid() const;
  float a() const;  ///< watts, NaN until valid
  float b() const;  ///< dimensionless, NaN until valid
  size_t samples() const;

  /// Fitted difference at a given throughput; NaN until valid. Used both as the
  /// plausibility centre for the auto self edge and as the balance correction.
  float predict(float p_out) const;

  void reset();

 private:
  double sw_{0}, swx_{0}, swy_{0}, swxx_{0}, swxy_{0};
  size_t n_{0};
  size_t min_samples_{30};
  float deadband_{15.0f};
  float weight_floor_{50.0f};
  float min_spread_{0.25f};
};

// ---------------------------------------------------------------------------
// §6.7  Grid-loss inference
// ---------------------------------------------------------------------------
//
// The battery discharging while the grid meter still claims import is a strong
// hint that the grid reading is stale. Home Assistant needs minutes to call the
// meter unavailable; this fires in seconds.
//
enum class InferenceMode : uint8_t {
  SUSPECT,         ///< default: dash instead of a number, topology unchanged
  ASSUME_OFFLINE,  ///< declare the node dead — only once the field justifies it
};

enum class GridVerdict : uint8_t { TRUSTED, SUSPECT, OFFLINE };

class GridLossInference {
 public:
  void set_enabled(bool enabled);
  void set_mode(InferenceMode mode);
  void set_min_discharge(float watts);  ///< magnitude, positive
  void set_hold(ms_t hold);             ///< condition must persist this long
  void set_require_stale_grid(bool require_stale);

  /// `p_battery` keeps the BMS sign convention: positive charges the battery.
  GridVerdict update(float p_battery, bool grid_reports_import, bool grid_stale, ms_t now);
  GridVerdict verdict() const;
  void reset();

 private:
  ms_t since_{0};
  bool arming_{false};
  bool enabled_{true};
  bool require_stale_{false};
  float min_discharge_{50.0f};
  ms_t hold_{20000};
  InferenceMode mode_{InferenceMode::SUSPECT};
  GridVerdict verdict_{GridVerdict::TRUSTED};
};

/// What the installation is doing right now, for the caption. Strings live in
/// the rendering layer, not here.
enum class SupplyMode : uint8_t {
  UNKNOWN,
  GRID,     ///< grid supplying, battery not discharging — "bypass"
  BATTERY,  ///< grid absent, battery discharging — "on battery"
};

SupplyMode classify_supply(float p_grid, float p_battery, bool grid_alive, float idle_below);

// ---------------------------------------------------------------------------
// §6.8  Derived figures
// ---------------------------------------------------------------------------
//
// Every one of these returns NaN when its validity condition is unmet. That is
// the governing principle (§6.9): a dash, never a rough estimate. The `- a`
// terms remove the standing draw so the ratio measures the conversion path
// rather than the cost of readiness.
//
struct DerivedGates {
  float min_flow{100.0f};  ///< watts below which a ratio is meaningless
  float max_ratio{1.0f};   ///< anything above this is a measurement error
};

/// P_batt / (P_in - P_out - a)
float charger_efficiency(float p_in, float p_out, float p_battery, float a, const DerivedGates &g);

/// P_out / (P_discharge - a), where p_discharge is a positive magnitude.
float inverter_efficiency(float p_out, float p_discharge, float a, const DerivedGates &g);

/// Energy-counter form: robust by construction, no window and no gate beyond a
/// minimum delta. Prefer this over the instantaneous ratios (§6.8).
float efficiency_from_energy(float energy_out, float energy_in, float min_delta);

// ---------------------------------------------------------------------------
// §6.8, amended 2026-08-23 after measuring the installation
// ---------------------------------------------------------------------------
//
// One headline figure, defined over the inverter node as a whole:
//
//     supplied  = max(p_in, 0) + max(-p_battery, 0) + max(p_pv, 0)
//     delivered = max(p_out, 0) + max(p_battery, 0)
//     losses    = supplied - delivered
//     efficiency = delivered / supplied
//
// The two are exactly complementary, so the owner's choice between them is a
// display preference and not a different calculation. Measured on this
// installation: at rest 607 in / 509 out, discharging 532 / 478, charging
// 2643 / (467 + 1944) — 98 W, 54 W and 232 W of loss, or 83.9 %, 89.8 % and
// 91.2 %. All three are physical.
//
// Deliberately the *system* form of §6.8: the standing draw is not subtracted.
// Field measurement showed the fitted `a` conflates the inverter's real draw
// with a systematic offset between an EARU and a Tongou, and two AC meters
// cannot separate those. Subtracting it produced 121 % efficiency; leaving it
// in gives the figures above. These are indicator gauges, not instruments, and
// the number is honestly approximate — but it is no longer impossible.
//
// An efficiency outside (0, max_ratio] or a negative loss still yields NONE. An
// approximate number is acceptable; one that violates conservation of energy is
// not, and it would cost more trust than a dash does.
//
// `p_in` and `p_out` follow the balance's convention exactly: 0 for an edge
// that is physically absent, NaN for one that is present but unreadable. The
// distinction matters — a real blackout takes the grid meter to NaN, while this
// installation's deliberate outage left it reporting a true 0.
//
enum class FigureMode : uint8_t {
  AUTO,        ///< watts while the battery rests, efficiency while it moves
  LOSSES,      ///< always watts
  EFFICIENCY,  ///< always a ratio
  BOTH,        ///< both; they cost the same two numbers to compute
};

/// Both figures are always computed — they come from the same pair of sums, so
/// there is nothing to save by skipping one. The flags say what to display; a
/// value that is NaN shows a dash even when its flag is set.
struct EnergyReading {
  float losses{NAN};      ///< watts unaccounted for at the inverter node
  float efficiency{NAN};  ///< delivered / supplied, dimensionless
  bool show_losses{false};
  bool show_efficiency{false};
};

/// `deadband` is the |P_battery| below which the battery counts as at rest,
/// which is what FigureMode::AUTO switches on.
/// `p_pv` is 0 when there are no panels, their generation when they are
/// metered, and **NaN when their edge is `power: auto`** — a figure the solver
/// derived from this node's own residual is not an independent measurement of
/// it, and feeding it back would make the loss come out as zero by
/// construction. The caller owns that distinction; here NaN simply propagates
/// and both figures become dashes, which is the correct answer.
EnergyReading energy_figure(float p_in, float p_out, float p_battery, float p_pv, float deadband,
                            FigureMode mode, const DerivedGates &g);

/// hours = usable_capacity x voltage x eta / load
///
/// Capacity below the inverter's cutoff is unreachable, so it is removed first:
/// usable_ah = capacity_remaining_ah * (soc - soc_cutoff) / soc.
/// NaN when the load is negligible, the SOC is at or below the cutoff, or any
/// input is missing.
float runtime_hours(float capacity_remaining_ah, float voltage, float eta, float load_w, float soc,
                    float soc_cutoff);

}  // namespace power_flow
}  // namespace esphome
