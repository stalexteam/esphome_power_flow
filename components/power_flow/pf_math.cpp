#include "pf_math.h"

#include <algorithm>
#include <limits>

namespace esphome {
namespace power_flow {

namespace {

constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();

/// Elapsed milliseconds across the ~49.7-day millis() wrap. Plain unsigned
/// subtraction, deliberately.
///
/// The tempting refinement is to treat an age of half a counter or more as an
/// out-of-order call and clamp it to zero. That trade is the wrong way round:
/// it also reads a genuinely ancient sample - anything past 24.8 days - as
/// having just arrived, so a source silent for a month looks fresh and its
/// value goes on the wall. Without the clamp, a sample that really did arrive
/// backwards reads as nearly 49 days old instead, and every consumer of this
/// number treats "very old" as "do not trust", which is the safe direction.
inline ms_t elapsed(ms_t now, ms_t then) { return now - then; }

/// Walks the sample list once and returns the time-weighted numerator and the
/// duration that carried a valid value. Both average() and coverage() need it;
/// the header is frozen, so it cannot be a private member.
///
/// Sample i holds its value from its own timestamp until sample i+1 arrives,
/// and the newest sample holds until `now`. Everything is expressed as an age
/// relative to `now`, so the arithmetic stays wrap-safe.
template<typename S>
void window_sums(const std::vector<S> &samples, ms_t now, ms_t window, double &num, double &den) {
  num = 0.0;
  den = 0.0;
  if (window == 0)
    return;
  const size_t n = samples.size();
  for (size_t i = 0; i < n; i++) {
    ms_t from = elapsed(now, samples[i].t);
    ms_t to = (i + 1 < n) ? elapsed(now, samples[i + 1].t) : 0u;
    if (from > window)
      from = window;
    if (to > window)
      to = window;
    if (from <= to)
      continue;
    const double dt = static_cast<double>(from - to);
    if (std::isfinite(samples[i].v)) {
      num += static_cast<double>(samples[i].v) * dt;
      den += dt;
    }
  }
}

/// Upper bound on the interval history. Keeps the median off the heap and the
/// learning window at a couple of minutes for any realistic cadence.
constexpr size_t MAX_HISTORY = 64;

/// Terms that cancel exactly still leave a few ulps behind. Flagging the whole
/// diagram unreliable because a balance closed at -1e-13 W would be a bug, not
/// a safety net; 1 mW is three decades below what any of these meters resolve.
constexpr double BALANCE_EPSILON = 1e-3;

/// Weighted variance of the throughput, in W^2, below which the slope `b` is
/// not identifiable: with no spread in the load, a calibration mismatch and a
/// standing draw explain the data equally well. 10 W of spread.
constexpr double MIN_X_VARIANCE = 100.0;

/// Division guard for runtime_hours(). Not a physical threshold: below a watt
/// the load reading carries no information and the quotient explodes.
constexpr float MIN_RUNTIME_LOAD_W = 1.0f;

}  // namespace

bool is_valid(float v) {
  // Infinity cannot arrive from Home Assistant, but it can arrive from a bad
  // division upstream, and it poisons an average exactly the way NaN does.
  return std::isfinite(v);
}

// ---------------------------------------------------------------------------
// §6.5  Time-weighted averaging
// ---------------------------------------------------------------------------

void TimeWeightedAverage::set_window(ms_t window) { this->window_ = window; }

ms_t TimeWeightedAverage::window() const { return this->window_; }

void TimeWeightedAverage::add(float value, ms_t now) {
  this->samples_.push_back(Sample{now, value});

  // Keep the newest sample that is already older than the window: it is the
  // value being held at the left edge, and dropping it would report a slow
  // source as uncovered for most of the window it actually spans.
  size_t drop = 0;
  for (size_t i = 0; i + 1 < this->samples_.size(); i++) {
    if (elapsed(now, this->samples_[i].t) < this->window_)
      break;
    drop = i;
  }
  if (drop > 0) {
    this->samples_.erase(this->samples_.begin(),
                         this->samples_.begin() + static_cast<std::ptrdiff_t>(drop));
  }
}

float TimeWeightedAverage::average(ms_t now) const {
  double num = 0.0, den = 0.0;
  window_sums(this->samples_, now, this->window_, num, den);
  if (!(den > 0.0))
    return NOT_A_NUMBER;
  return static_cast<float>(num / den);
}

float TimeWeightedAverage::coverage(ms_t now) const {
  if (this->window_ == 0)
    return 0.0f;
  double num = 0.0, den = 0.0;
  window_sums(this->samples_, now, this->window_, num, den);
  const double c = den / static_cast<double>(this->window_);
  if (c <= 0.0)
    return 0.0f;
  return c >= 1.0 ? 1.0f : static_cast<float>(c);
}

float TimeWeightedAverage::last() const {
  return this->samples_.empty() ? NOT_A_NUMBER : this->samples_.back().v;
}

ms_t TimeWeightedAverage::last_update() const {
  return this->samples_.empty() ? 0u : this->samples_.back().t;
}

/// True once anything at all has arrived, including an `unavailable`: having
/// contact with a source that reports NaN is not the same as never having heard
/// from it, and only the caller knows which of the two matters.
bool TimeWeightedAverage::has_data() const { return !this->samples_.empty(); }

void TimeWeightedAverage::reset() { this->samples_.clear(); }

// ---------------------------------------------------------------------------
// §6.4  Learned staleness
// ---------------------------------------------------------------------------

void StalenessDetector::set_stale_after(ms_t stale_after) { this->stale_after_ = stale_after; }

void StalenessDetector::set_factor(float factor) { this->factor_ = factor; }

void StalenessDetector::set_dispersion_limit(float limit) {
  this->dispersion_limit_ = (std::isfinite(limit) && limit > 0.0f) ? limit : 0.0f;
}

void StalenessDetector::set_history(size_t n) {
  if (n == 0)
    n = 1;
  if (n > MAX_HISTORY)
    n = MAX_HISTORY;
  this->history_ = n;
  if (this->intervals_.size() > n)
    this->intervals_.resize(n);
  if (this->next_ >= n)
    this->next_ = 0;
}

void StalenessDetector::on_sample(ms_t now) {
  if (!this->seen_) {
    this->seen_ = true;
    this->last_ = now;
    return;
  }
  const ms_t dt = elapsed(now, this->last_);
  this->last_ = now;
  // Two publications in the same millisecond carry no cadence information, and
  // feeding zeros to the median would collapse the timeout to nothing.
  if (dt == 0)
    return;
  if (this->intervals_.size() < this->history_) {
    this->intervals_.push_back(dt);
    this->next_ = this->intervals_.size() % this->history_;
  } else {
    this->intervals_[this->next_] = dt;
    this->next_ = (this->next_ + 1) % this->history_;
  }
}

namespace {

/// Median and 90th percentile of the retained intervals, by nearest rank.
/// Returns false while the history is too short to describe anything.
///
/// Both statistics come out of one sort because the dispersion test needs them
/// together: the p90 is the timeout, the median is only there to say whether
/// the p90 means anything.
bool interval_quantiles(const std::vector<ms_t> &intervals, size_t history, ms_t &median, ms_t &p90) {
  const size_t n = intervals.size();
  // Below three intervals a quantile is just the latest gap wearing a hat.
  size_t need = history < 3 ? history : 3;
  if (need == 0)
    need = 1;
  if (n == 0 || n < need)
    return false;

  ms_t buf[MAX_HISTORY];
  const size_t count = n < MAX_HISTORY ? n : MAX_HISTORY;
  for (size_t i = 0; i < count; i++)
    buf[i] = intervals[i];
  std::sort(buf, buf + count);

  const size_t mid = count / 2;
  median = (count % 2 == 1)
               ? buf[mid]
               : static_cast<ms_t>((static_cast<uint64_t>(buf[mid - 1]) + buf[mid]) / 2u);

  // Nearest rank: ceil(0.9 * count), in integer arithmetic.
  const size_t rank = (9u * count + 9u) / 10u;
  p90 = buf[rank - 1];
  return true;
}

}  // namespace

ms_t StalenessDetector::learned_interval() const {
  ms_t median = 0, p90 = 0;
  if (!interval_quantiles(this->intervals_, this->history_, median, p90))
    return 0;
  return p90;
}

ms_t StalenessDetector::effective_timeout() const {
  if (this->stale_after_ != 0)
    return this->stale_after_;

  ms_t median = 0, p90 = 0;
  if (!interval_quantiles(this->intervals_, this->history_, median, p90))
    return 0;

  // An event-driven source - a Zigbee plug that publishes several times a
  // second while its load moves and then nothing for half an hour - has a
  // typical burst and a typical silence that differ by a factor of sixty. Its
  // silence carries no information about its health, so the honest answer is
  // to disarm rather than to guess at a threshold.
  if (this->dispersion_limit_ > 0.0f && median > 0) {
    const double spread = static_cast<double>(p90) / static_cast<double>(median);
    if (spread > static_cast<double>(this->dispersion_limit_))
      return 0;
  }

  const double f =
      (std::isfinite(this->factor_) && this->factor_ > 0.0f) ? static_cast<double>(this->factor_) : 2.0;
  const double t = static_cast<double>(p90) * f;
  if (t >= 4294967295.0)
    return 0xFFFFFFFFu;
  const ms_t v = static_cast<ms_t>(t);
  return v == 0 ? 1u : v;
}

/// A source with no timeout in force is not "fresh" - freshness is a question
/// it cannot answer - so the renderer needs this separately from stale().
bool StalenessDetector::armed() const { return this->effective_timeout() != 0; }

bool StalenessDetector::stale(ms_t now) const {
  if (!this->seen_)
    return false;  // never heard from is a different condition, and not this one
  const ms_t timeout = this->effective_timeout();
  if (timeout == 0)
    return false;  // still learning: a source cannot be late for an unknown appointment
  return elapsed(now, this->last_) >= timeout;
}

void StalenessDetector::reset() {
  this->intervals_.clear();
  this->next_ = 0;
  this->last_ = 0;
  this->seen_ = false;
}

// ---------------------------------------------------------------------------
// §6.1 / §6.3  Edge state
// ---------------------------------------------------------------------------

EdgeState evaluate_edge(const EdgeInput &in) {
  // Row order is the specification (§6.1) and first match wins. Availability is
  // decided on the raw sample, never on the average: a 60 s window must not
  // delay a cross by a minute.
  if (in.has_meter && !in.meter_available) {
    return in.on_unavailable == UnavailablePolicy::NO_DATA ? EdgeState::NO_DATA
                                                           : EdgeState::DE_ENERGIZED;
  }

  // Row 2 of the table, and it outranks every row below it: an edge whose
  // breaker is explicitly off is open whether or not anyone solved for it.
  // ABSENT / UNKNOWN still mean closed (decision log #6).
  if (in.sw == SwitchState::OFF)
    return EdgeState::OPEN;

  if (!in.has_meter && !is_valid(in.power)) {
    // A pure `auto` edge with no solution: a dash. It is emphatically not
    // de-energized — the balance failed, the wire did not.
    return EdgeState::NO_DATA;
  }

  // Flow above the threshold proves the contact is closed. Zero proves nothing,
  // which is why the fall-through is IDLE and carries no cross.
  if (is_valid(in.power) && std::fabs(in.power) > in.idle_below)
    return EdgeState::ACTIVE;

  return EdgeState::IDLE;
}

bool contributes_to_balance(EdgeState s) {
  // NO_DATA does contribute — as an unreadable term, which is exactly what
  // stops the node from solving. Only a physically absent edge is excluded.
  return s != EdgeState::OPEN && s != EdgeState::DE_ENERGIZED;
}

// ---------------------------------------------------------------------------
// §4  Balance solver
// ---------------------------------------------------------------------------

BalanceResult solve_balance(const BalanceTerm *terms, size_t n, float ceiling) {
  BalanceResult r;
  r.value = NOT_A_NUMBER;
  r.solved = false;
  r.plausible = false;
  r.residual = NOT_A_NUMBER;

  if (terms == nullptr)
    n = 0;

  size_t auto_count = 0;
  size_t auto_i = 0;
  for (size_t i = 0; i < n; i++) {
    if (terms[i].is_auto) {
      auto_count++;
      auto_i = i;
    }
  }

  // The residual is computed before any fault can return early: when nothing
  // solves, it is the only thing that tells the caller whether the node missed
  // by 2 W or by 900.
  double sum = 0.0;
  bool sum_known = true;
  for (size_t i = 0; i < n; i++) {
    const BalanceTerm &t = terms[i];
    if (t.is_auto || !t.included)
      continue;  // the unknown does not contribute, and an absent edge is exactly 0
    if (!is_valid(t.power)) {
      // Present but unreadable. Zeroing it here is what would invent load.
      sum_known = false;
      break;
    }
    sum += (t.sign < 0 ? -1.0 : 1.0) * static_cast<double>(t.power);
  }
  if (sum_known && std::isfinite(sum))
    r.residual = static_cast<float>(sum);

  if (auto_count == 0) {
    r.fault = BalanceFault::NO_AUTO;
    return r;
  }
  if (auto_count > 1) {
    r.fault = BalanceFault::MULTIPLE_AUTO;
    return r;
  }
  if (!terms[auto_i].included) {
    // The unknown edge is itself open or dead, so it carries exactly 0 - that
    // much is known. Solving anyway would push the whole imbalance of the node
    // through a breaker that is standing open: unmetered PV generating at
    // night. Whatever the rest fails to cancel is in `residual`, where it can
    // be attributed to the meter that is actually wrong.
    r.value = 0.0f;
    r.fault = BalanceFault::AUTO_EXCLUDED;
    return r;
  }
  if (!sum_known || !std::isfinite(sum)) {
    r.fault = BalanceFault::MISSING_INPUT;
    return r;
  }

  const double s_auto = terms[auto_i].sign < 0 ? -1.0 : 1.0;
  double v = -sum / s_auto;
  if (!std::isfinite(v)) {
    r.fault = BalanceFault::MISSING_INPUT;
    return r;
  }
  // Cancellation noise, not a negative remainder. The `<= 0` also normalises
  // the -0.0 that an exactly balanced node with an inflowing auto edge yields:
  // it is not less than zero, and it renders as "-0 W".
  if (v <= 0.0 && v > -BALANCE_EPSILON)
    v = 0.0;

  r.value = static_cast<float>(v);
  r.solved = true;

  if (v < 0.0) {
    r.fault = BalanceFault::NEGATIVE;
    return r;
  }
  // A negative ceiling is a configuration error the schema rejects; here it
  // means "no ceiling", explicitly. The other reading - that everything exceeds
  // it - would flag a healthy diagram unreliable forever.
  const bool has_ceiling = is_valid(ceiling) && ceiling >= 0.0f;
  if (has_ceiling && v > static_cast<double>(ceiling)) {
    r.fault = BalanceFault::ABOVE_CEILING;
    return r;
  }

  r.plausible = true;
  r.fault = BalanceFault::NONE;
  return r;
}

// ---------------------------------------------------------------------------
// §6.6  Self-consumption baseline
// ---------------------------------------------------------------------------

void BaselineFit::set_deadband(float watts) { this->deadband_ = watts; }

void BaselineFit::set_min_samples(size_t n) { this->min_samples_ = n; }

void BaselineFit::set_weight_floor(float watts) { this->weight_floor_ = watts; }

bool BaselineFit::add_sample(float p_in, float p_out, float p_battery) {
  if (!is_valid(p_in) || !is_valid(p_out) || !is_valid(p_battery))
    return false;
  // A battery outside the deadband adds a term to the equation, so the sample
  // stops being about the inverter. An unreadable battery cannot clear this
  // test either, which is why NaN is rejected above instead of assumed at rest.
  if (!(std::fabs(p_battery) < this->deadband_))
    return false;

  const double x = static_cast<double>(p_out);
  const double y = static_cast<double>(p_in) - x;

  // Inverse to throughput squared. Each meter is accurate to a percentage of
  // its own reading, so the absolute error on the difference grows linearly
  // with P_out and its variance grows as P_out^2; weighting by 1/variance is
  // what makes the fit minimum-variance. A linear 1/P under-punishes a 1.5 kW
  // sample by a factor of thirty against a 50 W one. The floor keeps the weight
  // finite at zero throughput and stands in for the constant noise term the
  // meters also have.
  const double floor_w = (is_valid(this->weight_floor_) && this->weight_floor_ > 0.0f)
                             ? static_cast<double>(this->weight_floor_)
                             : 1.0;
  const double mag = std::fabs(x);
  const double scale = mag > floor_w ? mag : floor_w;
  const double w = 1.0 / (scale * scale);

  this->sw_ += w;
  this->swx_ += w * x;
  this->swy_ += w * y;
  this->swxx_ += w * x * x;
  this->swxy_ += w * x * y;
  this->n_++;
  return true;
}

bool BaselineFit::valid() const {
  if (this->n_ < this->min_samples_ || this->n_ < 2)
    return false;
  if (!(this->sw_ > 0.0))
    return false;
  const double mean_x = this->swx_ / this->sw_;
  const double var_x = this->swxx_ / this->sw_ - mean_x * mean_x;
  return var_x > MIN_X_VARIANCE;
}

float BaselineFit::b() const {
  if (!this->valid())
    return NOT_A_NUMBER;
  const double d = this->sw_ * this->swxx_ - this->swx_ * this->swx_;
  if (!(d > 0.0))
    return NOT_A_NUMBER;
  const double v = (this->sw_ * this->swxy_ - this->swx_ * this->swy_) / d;
  return std::isfinite(v) ? static_cast<float>(v) : NOT_A_NUMBER;
}

float BaselineFit::a() const {
  const float slope = this->b();
  if (!is_valid(slope))
    return NOT_A_NUMBER;
  const double v = (this->swy_ - static_cast<double>(slope) * this->swx_) / this->sw_;
  return std::isfinite(v) ? static_cast<float>(v) : NOT_A_NUMBER;
}

size_t BaselineFit::samples() const { return this->n_; }

float BaselineFit::predict(float p_out) const {
  if (!is_valid(p_out))
    return NOT_A_NUMBER;
  const float intercept = this->a();
  const float slope = this->b();
  if (!is_valid(intercept) || !is_valid(slope))
    return NOT_A_NUMBER;
  return intercept + slope * p_out;
}

/// Clears the collected samples only. Deadband, minimum count and weight floor
/// come from the configuration and outlive a reset.
void BaselineFit::reset() {
  this->sw_ = 0.0;
  this->swx_ = 0.0;
  this->swy_ = 0.0;
  this->swxx_ = 0.0;
  this->swxy_ = 0.0;
  this->n_ = 0;
}

// ---------------------------------------------------------------------------
// §6.7  Grid-loss inference
// ---------------------------------------------------------------------------

void GridLossInference::set_enabled(bool enabled) { this->enabled_ = enabled; }

void GridLossInference::set_mode(InferenceMode mode) { this->mode_ = mode; }

void GridLossInference::set_min_discharge(float watts) { this->min_discharge_ = std::fabs(watts); }

void GridLossInference::set_hold(ms_t hold) { this->hold_ = hold; }

void GridLossInference::set_require_stale_grid(bool require_stale) { this->require_stale_ = require_stale; }

GridVerdict GridLossInference::update(float p_battery, bool grid_reports_import, bool grid_stale, ms_t now) {
  if (!this->enabled_) {
    this->arming_ = false;
    this->verdict_ = GridVerdict::TRUSTED;
    return this->verdict_;
  }

  // BMS convention: positive charges. Discharge lives on the negative side, and
  // an unreadable battery is not evidence of anything.
  const bool discharging = is_valid(p_battery) && (-p_battery) >= this->min_discharge_;
  const bool condition = discharging && grid_reports_import && (!this->require_stale_ || grid_stale);

  if (!condition) {
    // No hold on the way back: §6.4 wants the falling edge fast, not debounced.
    this->arming_ = false;
    this->verdict_ = GridVerdict::TRUSTED;
    return this->verdict_;
  }

  if (!this->arming_) {
    this->arming_ = true;
    this->since_ = now;
  }
  if (elapsed(now, this->since_) >= this->hold_) {
    this->verdict_ =
        this->mode_ == InferenceMode::ASSUME_OFFLINE ? GridVerdict::OFFLINE : GridVerdict::SUSPECT;
  }
  return this->verdict_;
}

GridVerdict GridLossInference::verdict() const { return this->verdict_; }

void GridLossInference::reset() {
  this->arming_ = false;
  this->since_ = 0;
  this->verdict_ = GridVerdict::TRUSTED;
}

SupplyMode classify_supply(float p_grid, float p_battery, bool grid_alive, float idle_below) {
  // The battery must be readable for either verdict: "grid supplying and the
  // battery not discharging" is a claim about the battery too (§6.9).
  if (!is_valid(p_battery))
    return SupplyMode::UNKNOWN;
  const float idle = is_valid(idle_below) ? std::fabs(idle_below) : 0.0f;

  const bool discharging = p_battery < -idle;
  const bool grid_supplying = grid_alive && is_valid(p_grid) && p_grid > idle;
  const bool grid_gone = !grid_alive || !is_valid(p_grid) || std::fabs(p_grid) <= idle;

  if (grid_supplying && !discharging)
    return SupplyMode::GRID;
  if (grid_gone && discharging)
    return SupplyMode::BATTERY;
  return SupplyMode::UNKNOWN;
}

// ---------------------------------------------------------------------------
// §6.8  Derived figures
// ---------------------------------------------------------------------------

namespace {

/// Shared gate for the two instantaneous efficiencies. Written so that a NaN
/// anywhere — including in the gate itself — fails closed.
float gated_ratio(double numerator, double denominator, const DerivedGates &g) {
  if (!std::isfinite(numerator) || !std::isfinite(denominator))
    return NOT_A_NUMBER;
  if (!(denominator > 0.0) || !(denominator >= static_cast<double>(g.min_flow)))
    return NOT_A_NUMBER;
  const double ratio = numerator / denominator;
  if (!std::isfinite(ratio) || !(ratio > 0.0) || !(ratio <= static_cast<double>(g.max_ratio)))
    return NOT_A_NUMBER;
  return static_cast<float>(ratio);
}

}  // namespace

float charger_efficiency(float p_in, float p_out, float p_battery, float a, const DerivedGates &g) {
  if (!is_valid(p_in) || !is_valid(p_out) || !is_valid(p_battery) || !is_valid(a))
    return NOT_A_NUMBER;
  // Subtracting the standing draw is what separates "quality of the charging
  // path" from "cost of readiness" (§6.8).
  const double into_charger =
      static_cast<double>(p_in) - static_cast<double>(p_out) - static_cast<double>(a);
  return gated_ratio(static_cast<double>(p_battery), into_charger, g);
}

float inverter_efficiency(float p_out, float p_discharge, float a, const DerivedGates &g) {
  if (!is_valid(p_out) || !is_valid(p_discharge) || !is_valid(a))
    return NOT_A_NUMBER;
  if (!(p_discharge > 0.0f))
    return NOT_A_NUMBER;  // documented as a positive magnitude; a sign slip is not data
  const double from_battery = static_cast<double>(p_discharge) - static_cast<double>(a);
  return gated_ratio(static_cast<double>(p_out), from_battery, g);
}

float efficiency_from_energy(float energy_out, float energy_in, float min_delta) {
  if (!is_valid(energy_out) || !is_valid(energy_in) || !is_valid(min_delta))
    return NOT_A_NUMBER;
  if (energy_out < 0.0f)
    return NOT_A_NUMBER;
  if (!(energy_in > 0.0f) || !(energy_in >= min_delta))
    return NOT_A_NUMBER;
  return energy_out / energy_in;
}

EnergyReading energy_figure(float p_in, float p_out, float p_battery, float p_pv, float deadband,
                            FigureMode mode, const DerivedGates &g) {
  EnergyReading r;  // both values NaN until they survive their gates

  // The flags describe the shape of the display and nothing else: they are set
  // from the mode and the battery state before a single gate is consulted, so a
  // gate firing for one cycle turns a number into a dash instead of making the
  // whole row disappear and come back.
  const bool battery_known = is_valid(p_battery) && is_valid(deadband);
  const bool at_rest = battery_known && std::fabs(p_battery) <= std::fabs(deadband);
  switch (mode) {
    case FigureMode::LOSSES:
      r.show_losses = true;
      break;
    case FigureMode::EFFICIENCY:
      r.show_efficiency = true;
      break;
    case FigureMode::BOTH:
      r.show_losses = true;
      r.show_efficiency = true;
      break;
    case FigureMode::AUTO:
      // With an unreadable battery the mode cannot be chosen. Keeping the
      // at-rest presentation leaves a labelled dash on the screen, which is the
      // lesser of the two evils the flags exist to avoid.
      r.show_losses = at_rest || !battery_known;
      r.show_efficiency = battery_known && !at_rest;
      break;
  }

  // p_in, p_out and p_pv carry the balance's convention: 0 is an edge that is
  // not there, NaN is an edge that is there and unreadable. A real blackout
  // gives NaN and forbids the figure; a deliberate outage with the meter still
  // alive gives a true 0 and the node is computed from the battery alone.
  //
  // For p_pv that convention does double duty. Unmetered panels are `power:
  // auto` and the solver derives their output from this node's own residual;
  // feeding that back would drive the loss to zero by construction and produce
  // a confident number carrying no information. The caller passes NaN, and the
  // dash below is the honest answer.
  if (!is_valid(p_in) || !is_valid(p_out) || !is_valid(p_pv) || !battery_known)
    return r;

  const double in = static_cast<double>(p_in);
  const double out = static_cast<double>(p_out);
  const double batt = static_cast<double>(p_battery);
  const double pv = static_cast<double>(p_pv);
  const double supplied = (in > 0.0 ? in : 0.0) + (batt < 0.0 ? -batt : 0.0) + (pv > 0.0 ? pv : 0.0);
  const double delivered = (out > 0.0 ? out : 0.0) + (batt > 0.0 ? batt : 0.0);

  // One gate for both figures: below a real flow the difference of two meters
  // is their disagreement, not the inverter.
  if (!(supplied >= static_cast<double>(g.min_flow)) || !(supplied > 0.0))
    return r;

  const double losses = supplied - delivered;
  if (std::isfinite(losses) && losses >= 0.0)
    r.losses = static_cast<float>(losses);  // a negative loss is beyond physics

  const double efficiency = delivered / supplied;
  if (std::isfinite(efficiency) && efficiency > 0.0 &&
      efficiency <= static_cast<double>(g.max_ratio))
    r.efficiency = static_cast<float>(efficiency);

  return r;
}

float runtime_hours(float capacity_remaining_ah, float voltage, float eta, float load_w, float soc,
                    float soc_cutoff) {
  if (!is_valid(capacity_remaining_ah) || !is_valid(voltage) || !is_valid(eta) || !is_valid(load_w) ||
      !is_valid(soc) || !is_valid(soc_cutoff))
    return NOT_A_NUMBER;
  if (capacity_remaining_ah < 0.0f || voltage <= 0.0f)
    return NOT_A_NUMBER;
  if (!(eta > 0.0f) || eta > 1.0f)
    return NOT_A_NUMBER;
  if (load_w < MIN_RUNTIME_LOAD_W)
    return NOT_A_NUMBER;
  // At or below the cutoff the remaining charge is unreachable, so there is no
  // number to show — not a small one (§6.9).
  // A SOC outside 0..100 is a broken sensor, not a very full battery: taking
  // 150% at face value hands back a runtime the pack cannot deliver.
  if (soc_cutoff < 0.0f || !(soc > 0.0f) || soc > 100.0f || !(soc > soc_cutoff))
    return NOT_A_NUMBER;

  const double usable_ah = static_cast<double>(capacity_remaining_ah) *
                           (static_cast<double>(soc) - static_cast<double>(soc_cutoff)) /
                           static_cast<double>(soc);
  const double hours = usable_ah * static_cast<double>(voltage) * static_cast<double>(eta) /
                       static_cast<double>(load_w);
  return std::isfinite(hours) ? static_cast<float>(hours) : NOT_A_NUMBER;
}

}  // namespace power_flow
}  // namespace esphome
