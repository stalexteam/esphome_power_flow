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

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include <cmath>
#include <string>
#include <vector>

namespace esphome {
namespace power_flow {

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
struct PowerFlowStyle {
  uint16_t node_width{150};
  uint16_t node_height{62};
  uint16_t radius{14};
  uint32_t idle_color{0x3A4450};
  uint32_t active_color{0x4CAF50};
  uint32_t warn_color{0xE0A030};
  uint32_t dead_color{0x555F6B};
  uint32_t text_color{0xFFFFFF};
#ifdef USE_LVGL
  const lv_font_t *value_font{nullptr};
  const lv_font_t *label_font{nullptr};
#endif
};

// ---------------------------------------------------------------------------
// One measured (or solved) terminal. This is both the runtime state and what
// the renderer reads; there is no second copy of it.
// ---------------------------------------------------------------------------
struct Terminal {
  std::string name;
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

  // --- runtime, one entry per meter so `prefer` can fail over
  std::vector<TimeWeightedAverage> average;
  std::vector<StalenessDetector> staleness;
  SwitchState sw{SwitchState::ABSENT};

  // --- resolved each loop
  float raw{NAN};      ///< fast path: newest combined value
  float value{NAN};    ///< slow path: averaged, or the solved value if is_auto
  bool stale{false};
  EdgeState state{EdgeState::NO_DATA};
};

struct Device {
  std::string id;
  std::string name;
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

  float baseline_a{NAN};   ///< inverter standing draw, W
  float baseline_b{NAN};   ///< inter-meter calibration mismatch
  float runtime_hours{NAN};
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
#endif
  void set_average_window(uint32_t window_ms) { this->average_window_ = window_ms; }
  void set_idle_below(float watts) { this->idle_below_ = watts; }
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ = interval_ms; }
  /// `binary_sensor: platform: status`. The only sound "HA is connected" flag —
  /// api on_client_connected latches false and never recovers (§2).
  void set_status_sensor(binary_sensor::BinarySensor *s) { this->status_ = s; }
  void set_soc_cutoff(float pct) { this->soc_cutoff_ = pct; }
  PowerFlowStyle &style() { return this->style_; }

  // --- graph construction, in YAML declaration order
  uint8_t add_device(DeviceKind kind, const std::string &id, const std::string &name);
  uint8_t add_terminal(uint8_t device, TerminalRole role, const std::string &name);
  /// A consumer is one terminal hanging off the bus; returns the terminal index.
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
  void set_terminal_bidirectional(uint8_t terminal, bool bidirectional);
  void set_terminal_enabled(uint8_t terminal, bool enabled);
  void set_terminal_baseline_learn(uint8_t terminal, bool learn);
  void set_terminal_sign(uint8_t terminal, int8_t sign);

  // --- device-level entities
  void set_device_switch(uint8_t device, text_sensor::TextSensor *sw);
  void set_device_voltage(uint8_t device, sensor::Sensor *voltage);
  void set_device_soc(uint8_t device, sensor::Sensor *soc);
  void set_device_capacity(uint8_t device, sensor::Sensor *capacity_ah);
  /// bus: `source: inv.output` — resolved to a terminal index by codegen.
  void set_device_source(uint8_t device, uint8_t terminal);

  // --- §6.7 inference
  void configure_inference(bool enabled, InferenceMode mode, float min_discharge, uint32_t hold_ms,
                           bool require_stale_grid);

  // --- read side, for the renderer
  const std::vector<Device> &devices() const { return this->devices_; }
  const std::vector<Terminal> &terminals() const { return this->terminals_; }
  const Diagnostics &diagnostics() const { return this->diag_; }
  const PowerFlowStyle &style() const { return this->style_; }
  uint8_t find_device(const std::string &id) const;

 protected:
  /// Fast path: newest raw value and switch states (§6.5).
  void read_raw_();
  /// Slow path: time-weighted averages, staleness, edge states.
  void resolve_();
  /// Per-node balance, filling in every auto terminal.
  void solve_();
  /// Baseline fit, inference, efficiencies, runtime estimate.
  void derive_();

  std::vector<Device> devices_;
  std::vector<Terminal> terminals_;
  Diagnostics diag_;
  PowerFlowStyle style_;

  BaselineFit baseline_;
  GridLossInference inference_;

  binary_sensor::BinarySensor *status_{nullptr};
#ifdef USE_LVGL
  lv_obj_t *parent_{nullptr};
#endif
  uint32_t average_window_{60000};
  uint32_t update_interval_{250};
  uint32_t last_update_{0};
  float idle_below_{3.0f};
  float soc_cutoff_{NAN};
};

}  // namespace power_flow
}  // namespace esphome
