#pragma once
//
// power_flow — the battery screen.
//
// A transcription of DEV/UI/BATTERY_UI_SPEC.md. It replaces the two hand-built
// LVGL pages the panel used to carry: everything that was behind the `DETAILS`
// button now lives below the fold of one scrolling page.
//
// Separate from FlowRenderer on purpose. The two screens share their palette
// (pf_palette.h) and nothing else: this one has no edges, no dots and no state
// machine, it is a readout, and the only thing it recomputes is which cell is
// highest and which is lowest.
//
// The component owns one and calls:
//
//   setup(pf)   once, from PowerFlow::setup(), if a battery_parent_id was given
//   update()    whenever the values have been re-resolved (~4 Hz)
//
#include "power_flow.h"

#ifdef USE_LVGL

#include <string>
#include <vector>

namespace esphome {
namespace power_flow {

class BatteryScreen {
 public:
  void setup(PowerFlow *pf);
  void update();

 public:
  PowerFlow *pf_{nullptr};

 protected:
  /// A number and its unit, laid out as one group. The unit is a smaller face
  /// and sits on the number's baseline, so the pair reads as a single value
  /// rather than as two labels that happen to be adjacent.
  struct Value {
    lv_obj_t *num{nullptr};
    lv_obj_t *unit{nullptr};
    std::string cache;
    uint32_t colour{0xFFFFFFFFu};
    /// Where the group is anchored: its right edge for the table, its centre
    /// for the triples. Re-laid whenever the text changes width — which is why
    /// the numerals are monospaced, so it usually does not.
    int16_t anchor{0};
    bool centred{false};
  };

  struct Cell {
    lv_obj_t *box{nullptr};
    lv_obj_t *index{nullptr};
    lv_obj_t *value{nullptr};
    std::string cache;
    uint32_t border{0xFFFFFFFFu};
    uint32_t colour{0xFFFFFFFFu};
  };

  // --- construction
  lv_obj_t *plain_(lv_obj_t *parent) const;
  lv_obj_t *card_(lv_obj_t *parent, int x, int y, int w, int h, int radius) const;
  lv_obj_t *label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                   lv_text_align_t align, uint32_t colour) const;
  void make_value_(Value &v, lv_obj_t *parent, const lv_font_t *num_font,
                   const lv_font_t *unit_font, int anchor, int y, bool centred);

  void build_back_(lv_obj_t *root);
  void build_ring_(lv_obj_t *c);
  void build_triples_(lv_obj_t *c);
  void build_table_(lv_obj_t *c);
  void build_errors_(lv_obj_t *c);
  void build_cells_(lv_obj_t *c);

  // --- per-update painting
  void set_value_(Value &v, const std::string &num, const char *unit, uint32_t colour);
  void update_ring_();
  void update_triples_();
  void update_table_();
  void update_errors_();
  void update_cells_();

  const BatteryDetails *det_() const;
  const Device *dev_() const;

  lv_obj_t *scroll_{nullptr};

  // --- ring
  lv_obj_t *ring_{nullptr}, *soc_num_{nullptr}, *soc_pct_{nullptr}, *charge_state_{nullptr};
  std::string txt_soc_, txt_state_;
  int32_t ring_pct_{-1};
  uint32_t ring_colour_{0xFFFFFFFFu};
  uint32_t state_colour_{0xFFFFFFFFu};

  // --- the two triples: voltage/current/power, then the temperatures
  Value v_volt_, v_curr_, v_power_;
  std::vector<Value> v_temp_;

  // --- the pack table
  Value t_remaining_, t_capacity_, t_cycles_, t_spread_, t_charged_, t_discharged_;
  lv_obj_t *t_balancing_{nullptr};
  std::string txt_balancing_;

  // --- errors
  lv_obj_t *err_icon_{nullptr}, *err_text_{nullptr};
  std::string txt_err_;
  uint32_t err_colour_{0xFFFFFFFFu};

  std::vector<Cell> cells_;
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
