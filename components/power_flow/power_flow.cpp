#include "power_flow.h"
#include "power_flow_render.h"
#include "battery_render.h"
#include "load_render.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace esphome {
namespace power_flow {

static const char *const TAG = "power_flow";

// ---------------------------------------------------------------------------
// Graph construction. Codegen calls these in YAML declaration order and refers
// to everything afterwards by the returned index, so the indices must stay
// exactly as handed out.
// ---------------------------------------------------------------------------

uint8_t PowerFlow::add_device(DeviceKind kind, const std::string &id, const std::string &name) {
  Device d;
  d.kind = kind;
  d.id = id;
  d.name = name;
  // Device owns a unique_ptr now, so it is move-only; the implicit move is
  // noexcept, which is what lets the vector grow.
  this->devices_.push_back(std::move(d));
  return static_cast<uint8_t>(this->devices_.size() - 1);
}

/// Orientation as seen by the terminal's own node: what flows in, what flows
/// out. The battery is `-1` and carries the BMS sign convention unchanged, so
/// charging (positive) correctly reads as an outflow from the inverter.
static int8_t default_sign(TerminalRole role) {
  switch (role) {
    case TerminalRole::INPUT:
    case TerminalRole::PV:
      return 1;
    default:
      return -1;
  }
}

/// Fallback caption for a terminal the YAML did not name. `terminals.input:`
/// carries no `name:` in the reference config, and an unnamed edge is useless
/// in a log line and worse on a diagram.
static const char *role_name(TerminalRole role) {
  switch (role) {
    case TerminalRole::INPUT:   return "Input";
    case TerminalRole::OUTPUT:  return "Output";
    case TerminalRole::BATTERY: return "Battery";
    case TerminalRole::SELF:    return "Self";
    case TerminalRole::PV:      return "PV";
    case TerminalRole::TAP:     return "Tap";
    default:                    return "Edge";
  }
}

uint8_t PowerFlow::add_terminal(uint8_t device, TerminalRole role, const std::string &name) {
  Terminal t;
  t.device = device;
  t.role = role;
  t.name = name.empty() ? role_name(role) : name;
  t.sign = default_sign(role);
  t.bidirectional = role == TerminalRole::BATTERY;
  this->terminals_.push_back(std::move(t));
  uint8_t idx = static_cast<uint8_t>(this->terminals_.size() - 1);
  if (device < this->devices_.size())
    this->devices_[device].terminals.push_back(idx);
  return idx;
}

uint8_t PowerFlow::add_consumer(const std::string &name, Side side) {
  Terminal t;
  t.name = name;
  t.side = side;
  t.role = TerminalRole::GENERIC;
  t.device = INVALID_INDEX;  // a consumer hangs off the bus, not off a device
  t.sign = -1;               // always out of the bus
  this->terminals_.push_back(std::move(t));
  return static_cast<uint8_t>(this->terminals_.size() - 1);
}

void PowerFlow::add_terminal_meter(uint8_t terminal, sensor::Sensor *meter) {
  Terminal &t = this->terminals_[terminal];
  t.meters.push_back(meter);
  t.average.emplace_back();
  t.staleness.emplace_back();
}

void PowerFlow::set_terminal_combine(uint8_t t, MeterCombine c) { this->terminals_[t].combine = c; }
void PowerFlow::set_terminal_switch(uint8_t t, text_sensor::TextSensor *s) {
  this->terminals_[t].switch_source = s;
}
void PowerFlow::set_terminal_energy(uint8_t t, sensor::Sensor *e) { this->terminals_[t].energy = e; }
void PowerFlow::set_terminal_auto(uint8_t t, float ceiling) {
  this->terminals_[t].is_auto = true;
  this->terminals_[t].ceiling = ceiling;
}
void PowerFlow::set_terminal_unavailable(uint8_t t, UnavailablePolicy p) {
  this->terminals_[t].on_unavailable = p;
}
void PowerFlow::set_terminal_stale_after(uint8_t t, uint32_t ms) {
  for (auto &s : this->terminals_[t].staleness)
    s.set_stale_after(ms);
  this->terminals_[t].stale_after = ms;
}
void PowerFlow::set_terminal_detect_stale(uint8_t t, bool d) { this->terminals_[t].detect_stale = d; }
void PowerFlow::set_terminal_bidirectional(uint8_t t, bool b) { this->terminals_[t].bidirectional = b; }
void PowerFlow::set_terminal_enabled(uint8_t t, bool e) { this->terminals_[t].enabled = e; }
void PowerFlow::set_terminal_baseline_learn(uint8_t t, bool l) { this->terminals_[t].learn_baseline = l; }
void PowerFlow::set_terminal_sign(uint8_t t, int8_t s) { this->terminals_[t].sign = s; }
void PowerFlow::set_terminal_icon(uint8_t t, const std::string &i) { this->terminals_[t].icon = i; }
void PowerFlow::set_terminal_id(uint8_t t, const std::string &i) { this->terminals_[t].id = i; }
void PowerFlow::set_device_key(uint8_t d, const std::string &k) { this->devices_[d].key = k; }

NodeDetails &PowerFlow::device_extra(uint8_t d) {
  Device &dev = this->devices_[d];
  if (dev.extra == nullptr)
    dev.extra = make_unique<NodeDetails>();
  return *dev.extra;
}

NodeDetails &PowerFlow::extra(uint8_t t) {
  Terminal &term = this->terminals_[t];
  if (term.extra == nullptr)
    term.extra = make_unique<NodeDetails>();
  return *term.extra;
}
void PowerFlow::set_device_icon(uint8_t d, const std::string &i) { this->devices_[d].icon = i; }

void PowerFlow::set_device_switch(uint8_t d, text_sensor::TextSensor *s) { this->devices_[d].switch_source = s; }
void PowerFlow::set_device_voltage(uint8_t d, sensor::Sensor *v) { this->devices_[d].voltage = v; }
void PowerFlow::set_device_soc(uint8_t d, sensor::Sensor *s) { this->devices_[d].soc = s; }
void PowerFlow::set_device_capacity(uint8_t d, sensor::Sensor *c) { this->devices_[d].capacity_remaining = c; }
void PowerFlow::set_device_source(uint8_t d, uint8_t terminal) { this->devices_[d].source_terminal = terminal; }
void PowerFlow::set_device_on_click(uint8_t d, Trigger<> *t) { this->devices_[d].on_click = t; }

BatteryDetails &PowerFlow::details(uint8_t d) {
  Device &dev = this->devices_[d];
  if (dev.details == nullptr)
    dev.details = make_unique<BatteryDetails>();
  return *dev.details;
}

void PowerFlow::add_device_temperature(uint8_t d, const std::string &label, sensor::Sensor *s) {
  this->details(d).temperatures.emplace_back(label, s);
}

void PowerFlow::add_device_cell(uint8_t d, sensor::Sensor *s) { this->details(d).cells.push_back(s); }

void PowerFlow::configure_inference(bool enabled, InferenceMode mode, float min_discharge, uint32_t hold_ms,
                                    bool require_stale_grid) {
  this->inference_.set_enabled(enabled);
  this->inference_.set_mode(mode);
  this->inference_.set_min_discharge(min_discharge);
  this->inference_.set_hold(hold_ms);
  this->inference_.set_require_stale_grid(require_stale_grid);
}

uint32_t PowerFlow::last_update(const Terminal &t) const {
  uint32_t newest = 0;
  for (const TimeWeightedAverage &a : t.average) {
    const uint32_t u = a.last_update();
    if (u > newest)
      newest = u;
  }
  return newest;
}

uint8_t PowerFlow::find_device(const std::string &id) const {
  for (size_t i = 0; i < this->devices_.size(); i++)
    if (this->devices_[i].id == id)
      return static_cast<uint8_t>(i);
  return INVALID_INDEX;
}

// ---------------------------------------------------------------------------

static SwitchState parse_switch(const std::string &s) {
  if (s == "on" || s == "ON" || s == "true")
    return SwitchState::ON;
  if (s == "off" || s == "OFF" || s == "false")
    return SwitchState::OFF;
  // `unknown`, `unavailable`, `null`, anything else: the entity exists but is
  // telling us nothing. Decision log #6 — that is not the same as `off`.
  return SwitchState::UNKNOWN;
}

void PowerFlow::setup() {
  for (size_t ti = 0; ti < this->terminals_.size(); ti++) {
    Terminal &t = this->terminals_[ti];

    for (size_t mi = 0; mi < t.meters.size(); mi++) {
      t.average[mi].set_window(this->average_window_);
      if (t.stale_after != 0)
        t.staleness[mi].set_stale_after(t.stale_after);
      // Indices, never pointers: terminals_ reallocates as codegen builds it.
      t.meters[mi]->add_on_state_callback([this, ti, mi](float v) {
        uint32_t now = millis();
        this->terminals_[ti].average[mi].add(v, now);
        // An `unavailable` is still contact with the source, so it counts as an
        // arrival for cadence learning. Only silence is staleness (§6.4).
        this->terminals_[ti].staleness[mi].on_sample(now);
      });
    }

    if (t.switch_source != nullptr) {
      t.switch_source->add_on_state_callback(
          [this, ti](const std::string &s) { this->terminals_[ti].sw = parse_switch(s); });
    }
  }

  for (size_t di = 0; di < this->devices_.size(); di++) {
    Device &d = this->devices_[di];
    if (d.switch_source != nullptr) {
      d.switch_source->add_on_state_callback(
          [this, di](const std::string &s) { this->devices_[di].sw = parse_switch(s); });
    }
  }

  ESP_LOGCONFIG(TAG, "power_flow: %u devices, %u terminals", (unsigned) this->devices_.size(),
                (unsigned) this->terminals_.size());

#ifdef USE_LVGL
  if (this->parent_ != nullptr) {
    this->renderer_ = make_unique<FlowRenderer>();
    this->renderer_->setup(this);
  }
  if (this->battery_parent_ != nullptr) {
    this->battery_ = make_unique<BatteryScreen>();
    this->battery_->setup(this);
  }
  if (this->load_parent_ != nullptr) {
    this->load_ = make_unique<LoadScreen>();
    this->load_->setup(this);
  }
#endif
}

// ---------------------------------------------------------------------------
// Fast path (§6.5): newest raw value per terminal. Availability, switch states
// and every cross marker come from here, never from the 60 s average — a minute
// of smoothing would eat exactly the seconds the inference exists to win.
// ---------------------------------------------------------------------------

void PowerFlow::read_raw_() {
  for (Terminal &t : this->terminals_) {
    if (t.meters.empty()) {
      t.raw = NAN;
      continue;
    }
    if (t.combine == MeterCombine::PREFER) {
      // First available wins, top to bottom. This is a line measured twice, so
      // adding would double-count (decision log #2).
      t.raw = NAN;
      for (auto &a : t.average) {
        if (is_valid(a.last())) {
          t.raw = a.last();
          break;
        }
      }
    } else {
      // Parallel branches. A single unreadable branch makes the sum unknown —
      // treating it as zero would understate the terminal.
      float sum = 0.0f;
      bool any = false;
      for (auto &a : t.average) {
        float v = a.last();
        if (!is_valid(v)) {
          sum = NAN;
          any = true;
          break;
        }
        sum += v;
        any = true;
      }
      t.raw = any ? sum : NAN;
    }
  }
}

// ---------------------------------------------------------------------------
// Slow path: the averaged numbers, staleness, and the resulting edge state.
// ---------------------------------------------------------------------------

void PowerFlow::resolve_() {
  const uint32_t now = millis();

  for (Terminal &t : this->terminals_) {
    if (t.is_auto)
      continue;  // filled in by solve_()

    t.stale = false;
    if (t.detect_stale) {
      for (size_t mi = 0; mi < t.meters.size(); mi++) {
        if (t.staleness[mi].stale(now) && is_valid(t.average[mi].last()))
          t.stale = true;
      }
    }

    // Both spans come off the same buffer: the window is only an integration
    // limit, so the short one costs nothing but a second pass.
    const uint32_t span = this->display_window_;
    if (t.meters.empty()) {
      t.value = NAN;
      t.display = NAN;
    } else if (t.combine == MeterCombine::PREFER) {
      t.value = NAN;
      t.display = NAN;
      for (size_t mi = 0; mi < t.meters.size(); mi++) {
        if (is_valid(t.average[mi].last())) {
          t.value = t.average[mi].average(now);
          t.display = t.average[mi].average(now, span);
          break;
        }
      }
    } else {
      float sum = 0.0f, fast = 0.0f;
      for (auto &a : t.average) {
        float v = a.average(now);
        if (!is_valid(v)) {
          sum = NAN;
          fast = NAN;
          break;
        }
        sum += v;
        const float f = a.average(now, span);
        fast = is_valid(fast) && is_valid(f) ? fast + f : NAN;
      }
      t.value = sum;
      t.display = fast;
    }

    EdgeInput in;
    in.power = t.value;
    in.meter_available = is_valid(t.raw);
    in.has_meter = !t.meters.empty();
    in.sw = t.sw;
    in.idle_below = this->idle_below_;
    in.on_unavailable = t.on_unavailable;
    t.state = evaluate_edge(in);
  }
}

// ---------------------------------------------------------------------------
// §4 balance. One auto per node, and the three-way distinction below is the
// whole point: an absent edge contributes zero, an unreadable one makes the
// node unsolvable. Collapsing them invents load that does not exist.
// ---------------------------------------------------------------------------

/// How a resolved edge enters its node's balance.
///
/// A stale value still enters, and carries its doubt onward instead of voiding
/// the node. §6.4 asks for a stale reading to be dimmed and flagged, not
/// treated as zero — it says nothing about excluding it, and the first live
/// firmware showed why that distinction matters: one Zigbee report arriving
/// fifteen seconds late blanked every number on the page. The held value is
/// almost certainly still about right; the honest response is to mark it, and
/// everything solved from it, as suspect.
static BalanceTerm term_for(const Terminal &t) {
  BalanceTerm b;
  b.sign = t.sign;
  b.is_auto = t.is_auto;

  switch (t.state) {
    case EdgeState::OPEN:
    case EdgeState::DE_ENERGIZED:
      // Physically not there. Contributes exactly 0 and the node still solves.
      b.included = false;
      b.power = 0.0f;
      break;
    case EdgeState::NO_DATA:
      // There, carrying something we cannot read. The node must NOT solve.
      b.included = true;
      b.power = NAN;
      break;
    default:
      b.included = true;
      b.power = t.value;
      break;
  }
  return b;
}

/// True while the edge joining the inverter to the bus carries a number.
///
/// When it does not — the output meter has dropped out, or its breaker is open
/// while the apartment is fed around the inverter through the ATS — the two
/// nodes stop being separable. Their only link is unmeasured, so the inverter's
/// own draw and the apartment's unmetered remainder become one unknown seen
/// through one equation, and no arithmetic separates them.
bool PowerFlow::link_measured_() const {
  for (const Device &d : this->devices_) {
    if (d.kind != DeviceKind::BUS || d.source_terminal == INVALID_INDEX)
      continue;
    const Terminal &t = this->terminals_[d.source_terminal];
    return contributes_to_balance(t.state) && !t.stale;
  }
  return true;  // no bus, nothing to merge
}

void PowerFlow::solve_() {
  this->diag_.reliable = true;

  // Two nodes or one? The link decides, and it can change from one cycle to the
  // next, so it is asked every cycle rather than at setup.
  const bool split = this->link_measured_();

  for (size_t di = 0; di < this->devices_.size(); di++) {
    Device &dev = this->devices_[di];

    // Merged: the inverter is solved as part of the bus, so it has no node of
    // its own this cycle. Its own `auto` cannot be solved and says so.
    if (!split && dev.kind == DeviceKind::INVERTER) {
      for (uint8_t ti : dev.terminals) {
        Terminal &t = this->terminals_[ti];
        if (t.is_auto) {
          t.value = NAN;
          t.display = NAN;
          t.state = EdgeState::NO_DATA;
        }
      }
      continue;
    }

    std::vector<uint8_t> edges;
    for (uint8_t ti : dev.terminals) {
      if (this->terminals_[ti].enabled)
        edges.push_back(ti);
    }
    if (!split && dev.kind == DeviceKind::BUS) {
      // ...and the bus takes the inverter's edges instead, minus the dead link
      // between them. What its `auto` then solves for is the inverter's draw
      // plus the apartment's remainder together — which is honest: the power
      // that went past the inverter really is apartment load we cannot itemise,
      // and the part that stayed really is the inverter's. One figure, because
      // one equation.
      for (const Device &inv : this->devices_) {
        if (inv.kind != DeviceKind::INVERTER)
          continue;
        for (uint8_t ti : inv.terminals) {
          const Terminal &t = this->terminals_[ti];
          if (t.enabled && !t.is_auto && ti != dev.source_terminal)
            edges.push_back(ti);
        }
      }
    }
    if (dev.kind == DeviceKind::BUS) {
      // The bus is fed by whichever terminal `source:` named, and drained by
      // every consumer. Those live in two different YAML blocks; the node does
      // not care.
      if (split && dev.source_terminal != INVALID_INDEX)
        edges.push_back(dev.source_terminal);
      for (size_t ti = 0; ti < this->terminals_.size(); ti++) {
        if (this->terminals_[ti].device == INVALID_INDEX && this->terminals_[ti].enabled)
          edges.push_back(static_cast<uint8_t>(ti));
      }
    }

    std::vector<BalanceTerm> terms;
    int8_t auto_idx = -1;
    float ceiling = NAN;
    for (uint8_t ti : edges) {
      const Terminal &t = this->terminals_[ti];
      BalanceTerm b = term_for(t);
      // The source terminal feeds the bus, so at *this* node it is an inflow —
      // the opposite of its orientation at the device that owns it.
      if (dev.kind == DeviceKind::BUS && ti == dev.source_terminal)
        b.sign = 1;
      if (b.is_auto) {
        auto_idx = static_cast<int8_t>(terms.size());
        ceiling = t.ceiling;
      }
      terms.push_back(b);
    }
    if (auto_idx < 0)
      continue;

    BalanceResult r = solve_balance(terms.data(), terms.size(), ceiling);

    // A solved value is only as trustworthy as the readings it came from.
    bool any_stale = false;
    for (uint8_t ti : edges)
      any_stale = any_stale || (this->terminals_[ti].stale && !this->terminals_[ti].is_auto);

    for (uint8_t ti : edges) {
      Terminal &t = this->terminals_[ti];
      if (!t.is_auto)
        continue;
      t.value = r.solved ? r.value : NAN;
      // A solved value *is* a difference, so it keeps the long window; showing
      // it fast would make the kettle land on `Other` for the ten seconds
      // between one meter reporting and the next.
      t.display = t.value;
      t.stale = any_stale;

      EdgeInput in;
      in.power = t.value;
      in.meter_available = r.solved;
      in.has_meter = false;
      in.sw = t.sw;
      in.idle_below = this->idle_below_;
      in.on_unavailable = t.on_unavailable;
      t.state = evaluate_edge(in);
    }
    if (r.fault == BalanceFault::NEGATIVE || r.fault == BalanceFault::ABOVE_CEILING)
      this->diag_.reliable = false;
  }
}

// ---------------------------------------------------------------------------

const Terminal *PowerFlow::find_terminal_(DeviceKind kind, TerminalRole role) const {
  for (const Terminal &t : this->terminals_) {
    if (t.device == INVALID_INDEX || t.role != role)
      continue;
    if (this->devices_[t.device].kind == kind)
      return &t;
  }
  return nullptr;
}

const Device *PowerFlow::find_kind_(DeviceKind kind) const {
  for (const Device &d : this->devices_)
    if (d.kind == kind)
      return &d;
  return nullptr;
}

/// The value an edge contributes, in the balance's convention: exactly 0 when
/// the edge is physically absent, NaN when it is present but unreadable. The
/// headline figure needs the same distinction the solver does — a real blackout
/// takes the grid meter to NaN, while an open breaker upstream of a still-alive
/// meter reads a true 0.
static float balance_value(const Terminal *t) {
  if (t == nullptr)
    return NAN;
  switch (t->state) {
    case EdgeState::OPEN:
    case EdgeState::DE_ENERGIZED:
      return 0.0f;
    case EdgeState::NO_DATA:
      return NAN;
    default:
      return t->stale ? NAN : t->value;
  }
}

void PowerFlow::derive_() {
  const uint32_t now = millis();

  const Terminal *in = this->find_terminal_(DeviceKind::INVERTER, TerminalRole::INPUT);
  const Terminal *out = this->find_terminal_(DeviceKind::INVERTER, TerminalRole::OUTPUT);
  const Terminal *bat = this->find_terminal_(DeviceKind::INVERTER, TerminalRole::BATTERY);
  const Terminal *self = this->find_terminal_(DeviceKind::INVERTER, TerminalRole::SELF);

  const float p_in = in != nullptr ? in->value : NAN;
  const float p_out = out != nullptr ? out->value : NAN;
  const float p_bat = bat != nullptr ? bat->value : NAN;

  // §6.2: every meter dark at once *and* no API connection means we lost Home
  // Assistant, not the mains. Draw no crosses in that case.
  bool any_live = false;
  for (const Terminal &t : this->terminals_)
    any_live = any_live || (!t.meters.empty() && is_valid(t.raw));
  const bool api_up = this->status_ == nullptr || this->status_->state;
  this->diag_.ha_contact = any_live || api_up;

  // §6.6. The sampling conditions the fit cannot check for itself: the grid has
  // to be feeding through a closed bypass, and no panels may be generating.
  // Without PV in this installation the second is free.
  if (self != nullptr && self->learn_baseline && in != nullptr && out != nullptr) {
    const bool grid_alive = in->state == EdgeState::ACTIVE && !in->stale;
    if (grid_alive && !out->stale && (bat == nullptr || !bat->stale)) {
      this->baseline_.set_deadband(this->battery_deadband_);
      this->baseline_.add_sample(p_in, p_out, is_valid(p_bat) ? p_bat : NAN);
    }
  }
  this->diag_.baseline_a = this->baseline_.a();
  this->diag_.baseline_b = this->baseline_.b();

  // §6.7. The fast path feeds this on purpose: the whole point is to beat Home
  // Assistant's availability timeout by minutes.
  const float raw_bat = bat != nullptr ? bat->raw : NAN;
  const bool grid_claims_import = in != nullptr && is_valid(in->raw) && in->raw > this->idle_below_;
  const bool grid_stale = in != nullptr && in->stale;
  this->diag_.grid = this->inference_.update(raw_bat, grid_claims_import, grid_stale, now);

  const bool grid_alive = in != nullptr && in->state == EdgeState::ACTIVE &&
                          this->diag_.grid != GridVerdict::OFFLINE;
  this->diag_.supply = classify_supply(p_in, p_bat, grid_alive, this->idle_below_);

  DerivedGates gates;

  // The headline figure. Deliberately the system form with nothing subtracted:
  // measurement showed the fitted `a` conflates the inverter's real draw with a
  // calibration offset between two meters, and subtracting it yields 121 %.
  //
  // Panels enter `supplied` as a full term when they are metered. When their
  // edge is `power: auto` the figure is not computable at all: the solver would
  // have derived that generation from this node's own residual, so the loss
  // would come out as zero by construction. NaN, and both figures go to dashes.
  const Terminal *pv = this->find_terminal_(DeviceKind::INVERTER, TerminalRole::PV);
  float p_pv = 0.0f;
  if (pv != nullptr && pv->enabled)
    p_pv = pv->is_auto ? NAN : balance_value(pv);

  //
  // With the link to the bus unmeasured there is no separating the inverter's
  // own draw from what went past it, so neither figure is computable — and an
  // absent output reads as a genuine zero to `balance_value`, which would make
  // LOSS quietly absorb the bypassed load and call itself trustworthy. NaN is
  // the honest input; both figures become dashes and the merged total shows up
  // on `Other` instead.
  const float p_out_fig = this->link_measured_() ? balance_value(out) : NAN;
  this->diag_.energy = energy_figure(balance_value(in), p_out_fig, balance_value(bat), p_pv,
                                     this->battery_deadband_, this->figure_mode_, gates);

  // Kept for the details page (§6.8). Both depend on `a` and will therefore
  // read as dashes until the cross-mode discrepancy is resolved.
  const float a = this->diag_.baseline_a;
  this->diag_.charger_eff = charger_efficiency(p_in, p_out, p_bat, a, gates);
  this->diag_.inverter_eff =
      inverter_efficiency(p_out, is_valid(p_bat) && p_bat < 0 ? -p_bat : NAN, a, gates);
}

// ---------------------------------------------------------------------------

static const char *state_name(EdgeState s) {
  switch (s) {
    case EdgeState::DE_ENERGIZED: return "dead";
    case EdgeState::OPEN:         return "open";
    case EdgeState::NO_DATA:      return "nodata";
    case EdgeState::ACTIVE:       return "active";
    default:                      return "idle";
  }
}

void PowerFlow::loop() {
  const uint32_t now = millis();

  // Animation runs every pass; the arithmetic does not. Dots need ~25 fps to
  // read as motion, while the numbers behind them change on a 60 s average.
  if (this->renderer_ != nullptr)
    this->renderer_->frame(now);

  if ((uint32_t) (now - this->last_update_) < this->update_interval_)
    return;
  this->last_update_ = now;

  this->read_raw_();
  this->resolve_();
  this->solve_();
  this->derive_();

  if (this->renderer_ != nullptr)
    this->renderer_->update();
  if (this->battery_ != nullptr)
    this->battery_->update();
  if (this->load_ != nullptr)
    this->load_->update();

  // Stage 2 has no rendering on purpose: §8 wants the numbers trusted before
  // anything is drawn, so they go to the log where they can be checked against
  // Home Assistant by hand. One compact line per cycle rather than one per
  // terminal — the panel logs at INFO, and a dozen lines every ten seconds
  // would bury everything else.
  if ((uint32_t) (now - this->last_log_) < 10000)
    return;
  this->last_log_ = now;

  std::string line;
  for (const Terminal &t : this->terminals_) {
    if (!t.enabled)
      continue;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s=%.0f/%s%s ", t.name.c_str(), t.value, state_name(t.state),
             t.stale ? "!" : "");
    line += buf;
  }
  ESP_LOGI(TAG, "%s", line.c_str());

  const EnergyReading &e = this->diag_.energy;
  ESP_LOGI(TAG, "losses=%.1fW%s eff=%.1f%%%s | supply=%d grid=%d %s%s | a=%.1f b=%+.4f",
           e.losses, e.show_losses ? "*" : "", e.efficiency * 100.0f, e.show_efficiency ? "*" : "",
           (int) this->diag_.supply, (int) this->diag_.grid,
           this->diag_.reliable ? "ok" : "UNRELIABLE", this->diag_.ha_contact ? "" : " NO-HA",
           this->diag_.baseline_a, this->diag_.baseline_b);
}

void PowerFlow::dump_config() {
  ESP_LOGCONFIG(TAG, "power_flow:");
  ESP_LOGCONFIG(TAG, "  average window: %u ms, idle below: %.1f W", (unsigned) this->average_window_,
                this->idle_below_);
  for (const Device &d : this->devices_) {
    ESP_LOGCONFIG(TAG, "  device %s (%s), %u terminals", d.id.c_str(), d.name.c_str(),
                  (unsigned) d.terminals.size());
  }
  for (const Terminal &t : this->terminals_) {
    ESP_LOGCONFIG(TAG, "  terminal %-14s meters=%u sign=%+d%s%s", t.name.c_str(),
                  (unsigned) t.meters.size(), t.sign, t.is_auto ? " AUTO" : "",
                  t.enabled ? "" : " DISABLED");
  }
}

}  // namespace power_flow
}  // namespace esphome
