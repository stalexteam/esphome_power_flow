#include "pf_math.h"

#include <algorithm>
#include <limits>

namespace esphome {
namespace power_flow {

namespace {

constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();

/// Elapsed milliseconds across the ~49.7-day millis() wrap. A timestamp that
/// looks more than half a counter ahead of `now` is an out-of-order call, not a
/// wrap, and is clamped to zero rather than turned into 24 days of history.
inline ms_t elapsed(ms_t now, ms_t then) {
  const ms_t d = now - then;
  return (d & 0x80000000u) != 0u ? 0u : d;
}

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

ms_t StalenessDetector::learned_interval() const {
  const size_t n = this->intervals_.size();
  // Below three intervals a "median" is just the latest gap wearing a hat.
  size_t need = this->history_ < 3 ? this->history_ : 3;
  if (need == 0)
    need = 1;
  if (n == 0 || n < need)
    return 0;

  ms_t buf[MAX_HISTORY];
  const size_t count = n < MAX_HISTORY ? n : MAX_HISTORY;
  for (size_t i = 0; i < count; i++)
    buf[i] = this->intervals_[i];

  const size_t mid = count / 2;
  std::nth_element(buf, buf + mid, buf + count);
  const ms_t hi = buf[mid];
  if ((count % 2) == 1)
    return hi;
  const ms_t lo = *std::max_element(buf, buf + mid);
  return static_cast<ms_t>((static_cast<uint64_t>(lo) + static_cast<uint64_t>(hi)) / 2u);
}

ms_t StalenessDetector::effective_timeout() const {
  if (this->stale_after_ != 0)
    return this->stale_after_;
  const ms_t median = this->learned_interval();
  if (median == 0)
    return 0;
  const double f =
      (std::isfinite(this->factor_) && this->factor_ > 0.0f) ? static_cast<double>(this->factor_) : 3.0;
  const double t = static_cast<double>(median) * f;
  if (t >= 4294967295.0)
    return 0xFFFFFFFFu;
  const ms_t v = static_cast<ms_t>(t);
  return v == 0 ? 1u : v;
}

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
  if (in.has_meter) {
    if (!in.meter_available) {
      return in.on_unavailable == UnavailablePolicy::NO_DATA ? EdgeState::NO_DATA
                                                             : EdgeState::DE_ENERGIZED;
    }
  } else if (!is_valid(in.power)) {
    // A pure `auto` edge with no solution: a dash. It is emphatically not
    // de-energized — the balance failed, the wire did not.
    return EdgeState::NO_DATA;
  }

  if (in.sw == SwitchState::OFF)
    return EdgeState::OPEN;  // ABSENT / UNKNOWN mean closed (decision log #6)

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
  if (auto_count == 0) {
    r.fault = BalanceFault::NO_AUTO;
    return r;
  }
  if (auto_count > 1) {
    r.fault = BalanceFault::MULTIPLE_AUTO;
    return r;
  }

  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    // `included` describes a contribution, and the unknown does not contribute;
    // it is solved for whatever the flag says.
    if (i == auto_i)
      continue;
    const BalanceTerm &t = terms[i];
    if (!t.included)
      continue;  // physically absent: exactly 0, and the node still solves
    if (!is_valid(t.power)) {
      // Present but unreadable. Zeroing it here is what would invent load.
      r.fault = BalanceFault::MISSING_INPUT;
      return r;
    }
    sum += (t.sign < 0 ? -1.0 : 1.0) * static_cast<double>(t.power);
  }

  const double s_auto = terms[auto_i].sign < 0 ? -1.0 : 1.0;
  double v = -sum / s_auto;
  if (!std::isfinite(v)) {
    r.fault = BalanceFault::MISSING_INPUT;
    return r;
  }
  if (v < 0.0 && v > -BALANCE_EPSILON)
    v = 0.0;  // cancellation noise, not a negative remainder

  r.value = static_cast<float>(v);
  r.solved = true;

  if (v < 0.0) {
    r.fault = BalanceFault::NEGATIVE;
    return r;
  }
  if (is_valid(ceiling) && ceiling >= 0.0f && v > static_cast<double>(ceiling)) {
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

  // Inverse to throughput: at 1 kW this is 1050 minus 1000 with meters good to
  // ~1%, so a high-load sample is nearly worthless next to a low-load one.
  const double floor_w = (is_valid(this->weight_floor_) && this->weight_floor_ > 0.0f)
                             ? static_cast<double>(this->weight_floor_)
                             : 1.0;
  const double mag = std::fabs(x);
  const double w = 1.0 / (mag > floor_w ? mag : floor_w);

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
  if (soc_cutoff < 0.0f || !(soc > 0.0f) || !(soc > soc_cutoff))
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
