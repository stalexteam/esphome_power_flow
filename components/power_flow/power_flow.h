#pragma once
//
// power_flow — the ESPHome component.
//
// This header is the seam between three separately written pieces:
//
//   pf_math.h      the platform-free core (solver, averaging, staleness, fit)
//   __init__.py    the config schema and codegen, which call the builder below
//   the renderer   which reads the views below and draws them with raw LVGL
//
// The builder is deliberately flat and index-addressed. Codegen walks the YAML
// in declaration order, calls add_device() / add_terminal() / add_consumer(),
// and attaches everything else by the returned index. That keeps the object
// model in C++ where it can be validated, and keeps the Python side a
// straightforward transcription with no mirror data structures.
//
// Indices are stable for the life of the component and are assigned in
// declaration order, which is also the render order (§4, consumers attach to
// the bus "ordered by declaration").
//
#include "pf_math.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"

#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace power_flow {

class FlowRenderer;
class BatteryScreen;
class LoadScreen;

static const uint8_t INVALID_INDEX = 0xFF;

enum class DeviceKind : uint8_t { GRID, INVERTER, BATTERY, BUS, PV };

/// Terminals are named from the device's point of view (§4, decision log #1).
/// GENERIC covers a consumer's single terminal and a plain device meter.
enum class TerminalRole : uint8_t { GENERIC, INPUT, OUTPUT, BATTERY, SELF, PV, TAP };

/// How several meters on one terminal combine. Separate keys on purpose:
/// merging them would silently double-count a line measured twice, which is the
/// worst class of bug here (decision log #2).
enum class MeterCombine : uint8_t {
  SUM,     ///< parallel branches on one terminal — add them
  PREFER,  ///< one line measured redundantly — first available wins, top down
};

enum class Side : uint8_t { LEFT, RIGHT };

// ---------------------------------------------------------------------------
// Style — every visual constant comes from YAML. The owner must never edit C++
// to change a colour (§8).
// ---------------------------------------------------------------------------
/// Only the faces. The palette lives in power_flow_render.cpp as constants
/// transcribed from DEV/UI/POWER_FLOW_UI_SPEC.md §2 — there is a document that
/// specifies every colour, so a YAML key for each would be twenty-six ways to
/// contradict it. Fonts cannot follow: ESPHome builds them at codegen time from
/// the `font:` block, and LVGL's built-in Montserrat carries no Cyrillic, so the
/// owner's own node names would vanish.
/// Only the faces. Every colour is a constant in power_flow_render.cpp,
/// transcribed from DEV/UI/POWER_FLOW_UI_SPEC.md §2 — there is a document that
/// specifies each one, so a YAML key per colour would be twenty-six ways to
/// contradict it. Fonts cannot follow: ESPHome builds them at codegen time.
///
/// Named by family and size rather than by role, and deliberately so. §8 states
/// one rule — every numeral is monospaced, every word is Montserrat — which
/// makes the family a property of the glyphs and not of the context. Slots that
/// mirror the spec's two tables turn transcription into a line-by-line job with
/// nothing left to interpret.
struct PowerFlowStyle {
#ifdef USE_LVGL
  // Montserrat: words, captions, node names.
  const lv_font_t *s10{nullptr};  ///< letter-spaced captions
  const lv_font_t *s14{nullptr};  ///< sub-lines, OFF, Balancing
  const lv_font_t *s16{nullptr};  ///< consumer names, table labels, Back
  const lv_font_t *s18{nullptr};  ///< Other, OnGrid, BULK
  const lv_font_t *s20{nullptr};  ///< node names in the narrow cards
  const lv_font_t *s22{nullptr};  ///< node names in the wide cards

  // JetBrains Mono: every numeral, and the units that ride along with one.
  const lv_font_t *m12{nullptr};  ///< units next to a value
  const lv_font_t *m14{nullptr};  ///< % under the flow SOC, source sub-values
  const lv_font_t *m16{nullptr};  ///< badge values, table values, cell index
  const lv_font_t *m22{nullptr};  ///< triple values, bus total, cell voltages
  const lv_font_t *m28{nullptr};  ///< LOSS / EFF, SOC number on the flow page
  const lv_font_t *m72{nullptr};  ///< SOC number on the battery screen

  const lv_font_t *icon{nullptr};  ///< MDI, pinned tag v7.4.47
#endif
};




// ---------------------------------------------------------------------------
// One measured (or solved) terminal. This is both the runtime state and what
// the renderer reads; there is no second copy of it.
// ---------------------------------------------------------------------------
/// What the detail screen needs beyond the diagram (DEV/UI/LOAD_UI_SPEC.md).
///
/// It hangs off either a consumer terminal or a device, because the screen is
/// about a *card* on the diagram and a card is one or the other. For the grid
/// the two are split on purpose: its state and its power come from the
/// inverter's input terminal — that is the edge the card sits on — while its
/// voltage, temperature and link belong to the breaker itself.
/// Every field is optional; a load that reports none of it still gets a screen,
/// with the blocks it cannot fill simply absent (§2).
struct NodeDetails {
  sensor::Sensor *voltage{nullptr};
  sensor::Sensor *temperature{nullptr};
  sensor::Sensor *link{nullptr};
  /// A second subscription to the switch entity, and deliberately so: the state
  /// comes from a text sensor because only that can tell `off` from `unknown`
  /// (decision log #6), while writing needs a Switch. They do different jobs on
  /// the same entity.
  switch_::Switch *control{nullptr};

  /// The entity ids, for §4's reason line and §8's list. Pointers to the same
  /// string literals codegen already emits for the subscriptions, so the text
  /// costs nothing twice — the linker merges identical literals.
  const char *power_id{nullptr};
  const char *switch_id{nullptr};
  const char *voltage_id{nullptr};
  const char *temperature_id{nullptr};
  const char *link_id{nullptr};
};

struct Terminal {
  std::string name;
  /// The key from the config (`pc`, `boiler`), shown under the display name and
  /// used in logs — the thing to grep for when something is wrong (§3).
  std::string id;
  std::string icon;  ///< UTF-8 MDI glyph; empty falls back to the role's
  uint8_t device{INVALID_INDEX};
  TerminalRole role{TerminalRole::GENERIC};

  // --- configuration
  std::vector<sensor::Sensor *> meters;
  MeterCombine combine{MeterCombine::SUM};
  text_sensor::TextSensor *switch_source{nullptr};
  sensor::Sensor *energy{nullptr};
  UnavailablePolicy on_unavailable{UnavailablePolicy::DE_ENERGIZED};
  bool is_auto{false};
  float ceiling{NAN};       ///< plausibility bound for an auto terminal
  bool bidirectional{false};  ///< the battery terminal: sign carries direction
  bool enabled{true};         ///< `enabled: false` hides the node entirely
  bool learn_baseline{false}; ///< `baseline: learn` on the inverter self edge
  int8_t sign{1};             ///< orientation at its node: +1 in, -1 out
  Side side{Side::RIGHT};     ///< consumers only
  uint32_t stale_after{0};    ///< manual override; 0 = learn the cadence
  /// Off by default, and that is a measured decision rather than caution.
  /// Zigbee power meters publish on *change past a threshold*, so the interval
  /// between reports measures how volatile the load is, not whether the device
  /// is alive: a steady load goes quiet, and the detector then flags exactly the
  /// readings that deserve the most trust. Enable it only on a source with a
  /// genuinely fixed cadence.
  bool detect_stale{false};

  // --- runtime, one entry per meter so `prefer` can fail over
  std::vector<TimeWeightedAverage> average;
  std::vector<StalenessDetector> staleness;
  SwitchState sw{SwitchState::ABSENT};

  // --- resolved each loop
  float raw{NAN};      ///< fast path: newest combined value
  float value{NAN};    ///< slow path: averaged, or the solved value if is_auto
  /// What the screen shows for a *measured* terminal: the same samples over a
  /// much shorter span. The long window exists so that a difference of two
  /// meters sampled at different instants means something; a number that is
  /// measured and displayed directly is never subtracted, and sixty seconds of
  /// smoothing only makes the panel look slow when a real load steps. Solved
  /// terminals keep `value` here, because a difference is exactly what they are.
  float display{NAN};
  bool stale{false};
  EdgeState state{EdgeState::NO_DATA};

  /// Present only where the owner configured a detail screen for this load.
  std::unique_ptr<NodeDetails> extra;
};

/// Everything the battery screen reads (DEV/UI/BATTERY_UI_SPEC.md). Kept apart
/// from `Device` because it is a second screen's worth of data that the diagram
/// never touches, and because most installations will configure none of it.
///
/// Three things the real BMS does that the spec's table does not say:
///   `delta` arrives in volts and is displayed in millivolts;
///   `errors` is an empty string when there are none, not the words "No errors";
///   `balancing` is a number, so OFF is our reading of zero rather than a state
///   the BMS reports.
struct BatteryDetails {
  text_sensor::TextSensor *charge_status{nullptr};  ///< "Bulk" -> BULK in the ring
  text_sensor::TextSensor *errors{nullptr};         ///< empty means no errors
  sensor::Sensor *current{nullptr};
  sensor::Sensor *power{nullptr};                   ///< signed, + charging
  sensor::Sensor *capacity_total{nullptr};
  sensor::Sensor *cycles{nullptr};
  sensor::Sensor *cell_delta{nullptr};              ///< volts in, millivolts out
  sensor::Sensor *balancing{nullptr};
  sensor::Sensor *energy_charged{nullptr};
  sensor::Sensor *energy_discharged{nullptr};

  /// Labelled because the pack decides what they measure, not the component:
  /// this BMS exposes five and the screen shows the three worth a glance.
  std::vector<std::pair<std::string, sensor::Sensor *>> temperatures;

  /// One per cell, in pack order. The count is configured and never assumed:
  /// the protocol takes up to 32, this pack has 8, and subscribing to all 32
  /// would leave 24 permanently unavailable — exactly the "dead versus no data"
  /// confusion §6.3 exists to prevent.
  std::vector<sensor::Sensor *> cells;
  /// Below this spread the min/max highlight is suppressed: at 1 mV a healthy
  /// pack would carry two coloured cells every day of its life, and a warning
  /// that is always on is not a warning.
  float highlight_above_mv{10.0f};
};

struct Device {
  std::string id;
  std::string name;
  std::string icon;
  DeviceKind kind{DeviceKind::GRID};
  std::vector<uint8_t> terminals;

  // --- device-level entities (§5)
  text_sensor::TextSensor *switch_source{nullptr};
  sensor::Sensor *voltage{nullptr};
  sensor::Sensor *soc{nullptr};
  sensor::Sensor *capacity_remaining{nullptr};
  uint8_t source_terminal{INVALID_INDEX};  ///< bus: `source: inv.output`

  SwitchState sw{SwitchState::ABSENT};
  bool alive{true};

  /// Fired when the owner taps this device's box. The component deliberately
  /// knows nothing about LVGL pages: the YAML binds an action, so the same
  /// component works on a panel laid out differently (§7).
  Trigger<> *on_click{nullptr};

  /// Present only on the battery, and only when the owner configured it.
  std::unique_ptr<BatteryDetails> details;
  /// Present when this device's card has a detail screen.
  std::unique_ptr<NodeDetails> extra;
  /// The key used in logs and shown under the display name (§3).
  std::string key;
};

// ---------------------------------------------------------------------------
// Whole-diagram state. Everything here obeys §6.9: NaN means "show a dash",
// never "show a rough estimate".
// ---------------------------------------------------------------------------
struct Diagnostics {
  bool ha_contact{true};   ///< false = we lost Home Assistant, not the mains
  bool reliable{true};     ///< false = a balance came out implausible
  SupplyMode supply{SupplyMode::UNKNOWN};
  GridVerdict grid{GridVerdict::TRUSTED};

  /// The single headline figure: losses in watts, or an efficiency, whichever
  /// the battery's current behaviour makes meaningful. See energy_figure().
  EnergyReading energy;

  float baseline_a{NAN};   ///< inverter standing draw, W
  float baseline_b{NAN};   ///< inter-meter calibration mismatch
  float charger_eff{NAN};
  float inverter_eff{NAN};
};

// ---------------------------------------------------------------------------
class PowerFlow : public Component {
 public:
  // --- Component
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // --- global configuration
#ifdef USE_LVGL
  void set_parent(lv_obj_t *parent) { this->parent_ = parent; }
  /// The second screen's root, an empty `obj` on its own LVGL page. Optional:
  /// without it the battery screen is simply not built.
  void set_battery_parent(lv_obj_t *parent) { this->battery_parent_ = parent; }
  lv_obj_t *battery_parent() const { return this->battery_parent_; }
  void set_load_parent(lv_obj_t *parent) { this->load_parent_ = parent; }
  lv_obj_t *load_parent() const { return this->load_parent_; }

  /// Wall-clock, for "off since 09:12". Durations come from millis(); the hour
  /// does not, and a reason that says how long ago without saying when is half
  /// a reason — the time is what lets you match it against everything else that
  /// happened (DEV/UI/LOAD_UI_SPEC.md §4).
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  time::RealTimeClock *rtc() const { return this->time_; }

  /// Which card the detail screen is showing. Set by the tap, read by the
  /// screen; INVALID_INDEX means nothing is selected yet.
  void select_node(uint8_t device, uint8_t terminal) {
    this->sel_device_ = device;
    this->sel_terminal_ = terminal;
  }
  uint8_t selected_device() const { return this->sel_device_; }
  uint8_t selected_terminal() const { return this->sel_terminal_; }
  /// Fired by the battery screen's `Back` button. The component does not know
  /// what a page is; the YAML binds the action.
  void set_on_back(Trigger<> *t) { this->on_back_ = t; }
  Trigger<> *on_back() const { return this->on_back_; }
  /// Fired by a tap on any node that does not bind its own action. The battery
  /// binds one because it opens a different screen; everything else shares the
  /// detail screen, so one trigger serves them all.
  void set_on_node_click(Trigger<> *t) { this->on_node_click_ = t; }
  Trigger<> *on_node_click() const { return this->on_node_click_; }
#endif
  void set_average_window(uint32_t window_ms) { this->average_window_ = window_ms; }
  /// Span used for the numbers on screen. Shorter than the averaging window on
  /// purpose — see Terminal::display.
  void set_display_window(uint32_t window_ms) { this->display_window_ = window_ms; }
  void set_idle_below(float watts) { this->idle_below_ = watts; }
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ = interval_ms; }
  /// `binary_sensor: platform: status`. The only sound "HA is connected" flag —
  /// api on_client_connected latches false and never recovers (§2).
  void set_status_sensor(binary_sensor::BinarySensor *s) { this->status_ = s; }
  /// |P_battery| below which the battery counts as at rest, for both the
  /// headline figure and the baseline fit's sampling condition.
  void set_battery_deadband(float watts) { this->battery_deadband_ = watts; }
  void set_figure_mode(FigureMode mode) { this->figure_mode_ = mode; }
  PowerFlowStyle &style() { return this->style_; }

  // --- graph construction, in YAML declaration order
  uint8_t add_device(DeviceKind kind, const std::string &id, const std::string &name);
  /// Orientation defaults from the role and is only overridden when the YAML
  /// says so explicitly: INPUT and PV flow into their node, OUTPUT, SELF,
  /// BATTERY and TAP flow out of it. Codegen emits set_terminal_sign() only for
  /// an explicit `sign:` key, so getting this default right is this class's job.
  uint8_t add_terminal(uint8_t device, TerminalRole role, const std::string &name);
  /// A consumer is one terminal hanging off the bus; returns the terminal index.
  /// Its orientation is always out of the bus.
  uint8_t add_consumer(const std::string &name, Side side);

  // --- attaching data to a terminal
  void add_terminal_meter(uint8_t terminal, sensor::Sensor *meter);
  void set_terminal_combine(uint8_t terminal, MeterCombine combine);
  void set_terminal_switch(uint8_t terminal, text_sensor::TextSensor *sw);
  void set_terminal_energy(uint8_t terminal, sensor::Sensor *energy);
  /// `power: auto`. Exactly one per node — two is rejected in the schema, and
  /// the runtime guard here only exists to fail loudly if that check regresses.
  void set_terminal_auto(uint8_t terminal, float ceiling);
  void set_terminal_unavailable(uint8_t terminal, UnavailablePolicy policy);
  void set_terminal_stale_after(uint8_t terminal, uint32_t stale_after_ms);
  void set_terminal_detect_stale(uint8_t terminal, bool detect);
  void set_terminal_bidirectional(uint8_t terminal, bool bidirectional);
  void set_terminal_enabled(uint8_t terminal, bool enabled);
  void set_terminal_baseline_learn(uint8_t terminal, bool learn);
  void set_terminal_sign(uint8_t terminal, int8_t sign);
  void set_terminal_icon(uint8_t terminal, const std::string &icon);
  void set_terminal_id(uint8_t terminal, const std::string &id);
  void set_device_key(uint8_t device, const std::string &key);

  // --- the load screen's data (DEV/UI/LOAD_UI_SPEC.md)
  NodeDetails &extra(uint8_t terminal);
  NodeDetails &device_extra(uint8_t device);
  void set_device_icon(uint8_t device, const std::string &icon);

  // --- device-level entities
  void set_device_switch(uint8_t device, text_sensor::TextSensor *sw);
  void set_device_voltage(uint8_t device, sensor::Sensor *voltage);
  void set_device_soc(uint8_t device, sensor::Sensor *soc);
  void set_device_capacity(uint8_t device, sensor::Sensor *capacity_ah);
  /// bus: `source: inv.output` — resolved to a terminal index by codegen.
  void set_device_source(uint8_t device, uint8_t terminal);
  void set_device_on_click(uint8_t device, Trigger<> *trigger);

  // --- the battery screen's data (DEV/UI/BATTERY_UI_SPEC.md)
  BatteryDetails &details(uint8_t device);
  void add_device_temperature(uint8_t device, const std::string &label, sensor::Sensor *s);
  void add_device_cell(uint8_t device, sensor::Sensor *s);

  // --- §6.7 inference
  void configure_inference(bool enabled, InferenceMode mode, float min_discharge, uint32_t hold_ms,
                           bool require_stale_grid);

  // --- read side, for the renderer
  const std::vector<Device> &devices() const { return this->devices_; }
  const std::vector<Terminal> &terminals() const { return this->terminals_; }
  const Diagnostics &diagnostics() const { return this->diag_; }
  float idle_below() const { return this->idle_below_; }
  uint32_t display_window() const { return this->display_window_; }
  uint32_t average_window() const { return this->average_window_; }
  /// When this terminal's newest sample arrived, in millis(). 0 if never.
  uint32_t last_update(const Terminal &t) const;
  const PowerFlowStyle &style() const { return this->style_; }
  uint8_t find_device(const std::string &id) const;
#ifdef USE_LVGL
  lv_obj_t *parent() const { return this->parent_; }
#endif

 protected:
  /// Fast path: newest raw value and switch states (§6.5).
  void read_raw_();
  /// Slow path: time-weighted averages, staleness, edge states.
  void resolve_();
  /// Per-node balance, filling in every auto terminal.
  void solve_();
  bool link_measured_() const;
  /// Baseline fit, inference, efficiencies, runtime estimate.
  void derive_();

  const Terminal *find_terminal_(DeviceKind kind, TerminalRole role) const;
  const Device *find_kind_(DeviceKind kind) const;

  std::vector<Device> devices_;
  std::vector<Terminal> terminals_;
  Diagnostics diag_;
  PowerFlowStyle style_;

  BaselineFit baseline_;
  GridLossInference inference_;
  /// Owned by the component but written entirely in power_flow_render.cpp, so
  /// the drawing and the arithmetic stay in separate files. Null until setup()
  /// and on any build without LVGL.
  std::unique_ptr<FlowRenderer> renderer_;
  std::unique_ptr<BatteryScreen> battery_;
  std::unique_ptr<LoadScreen> load_;

  binary_sensor::BinarySensor *status_{nullptr};
#ifdef USE_LVGL
  lv_obj_t *parent_{nullptr};
  lv_obj_t *battery_parent_{nullptr};
  lv_obj_t *load_parent_{nullptr};
  time::RealTimeClock *time_{nullptr};
  uint8_t sel_device_{INVALID_INDEX};
  uint8_t sel_terminal_{INVALID_INDEX};
  Trigger<> *on_back_{nullptr};
  Trigger<> *on_node_click_{nullptr};
#endif
  uint32_t average_window_{60000};
  uint32_t display_window_{10000};
  uint32_t update_interval_{250};
  uint32_t last_update_{0};
  uint32_t last_log_{0};
  float idle_below_{3.0f};
  float battery_deadband_{15.0f};
  FigureMode figure_mode_{FigureMode::AUTO};
};

}  // namespace power_flow
}  // namespace esphome
