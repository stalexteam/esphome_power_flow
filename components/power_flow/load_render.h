#pragma once
//
// power_flow — the node detail screen.
//
// A transcription of DEV/UI/LOAD_UI_SPEC.md. One screen, reused for whichever
// card was tapped: the widget tree is built once and re-laid out per subject,
// because the blocks that apply depend on what that node happens to report.
//
// A "subject" is a card on the diagram, and a card is either a consumer or a
// device. For a consumer everything comes from its terminal. For a device — the
// grid is the case that matters — the readings split: state and power come from
// the terminal its card sits on (the inverter's input), while voltage,
// temperature and link belong to the device itself.
//
// The component owns one and calls:
//
//   setup(pf)   once, if a load_parent_id was given
//   update()    whenever values are re-resolved, and after a tap changes the
//               subject (~4 Hz either way)
//
#include "power_flow.h"

#ifdef USE_LVGL

#include <string>
#include <vector>

namespace esphome {
namespace power_flow {

class LoadScreen {
 public:
  void setup(PowerFlow *pf);
  void update();

  PowerFlow *pf_{nullptr};

 protected:
  /// A number and its unit as one group, the unit seated on the number's
  /// baseline. Same idea as the battery screen's, kept separate because the two
  /// anchor differently and sharing would mean a base class for four fields.
  struct Value {
    lv_obj_t *num{nullptr};
    lv_obj_t *unit{nullptr};
    std::string cache;
    uint32_t colour{0xFFFFFFFFu};
    int16_t anchor{0};
    bool centred{false};
  };

  /// One 144 x 76 card in the extra-readings row (§7). Built for the maximum
  /// and hidden when the subject does not report it: an absent entity produces
  /// no card, while a present one that has gone quiet produces a dash.
  struct Extra {
    lv_obj_t *card{nullptr};
    lv_obj_t *caption{nullptr};
    Value value;
  };

  /// One row of §8. Two lines, because an entity id can reach forty characters
  /// and truncating it defeats the only job this block has.
  struct Row {
    lv_obj_t *card{nullptr};
    lv_obj_t *label{nullptr};
    lv_obj_t *id{nullptr};
    lv_obj_t *flag{nullptr};
    std::string txt_id, txt_flag;
    uint32_t flag_colour{0xFFFFFFFFu};
  };

  // --- construction
  lv_obj_t *plain_(lv_obj_t *parent) const;
  lv_obj_t *card_(lv_obj_t *parent, int x, int y, int w, int h, int radius) const;
  lv_obj_t *label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                   lv_text_align_t align, uint32_t colour) const;
  void make_value_(Value &v, lv_obj_t *parent, const lv_font_t *num_font,
                   const lv_font_t *unit_font, int anchor, int y, bool centred);
  void set_value_(Value &v, const std::string &num, const char *unit, uint32_t colour);

  void build_back_(lv_obj_t *root);
  void build_header_(lv_obj_t *root);
  void build_state_(lv_obj_t *root);
  void build_switch_(lv_obj_t *root);
  void build_power_(lv_obj_t *root);
  void build_extras_(lv_obj_t *root);
  void build_rows_(lv_obj_t *root);

  // --- per-subject
  /// Recomputes which blocks apply and where they sit. Called when the subject
  /// changes and when a reason line changes height — §2 says the cursor is
  /// computed, never hard-coded for a particular consumer.
  void layout_();
  void update_header_();
  void update_state_();
  void update_switch_();
  void update_power_();
  void update_extras_();
  void update_rows_();

  const Terminal *term_() const;
  const NodeDetails *extra_() const;
  const Device *dev_() const;
  /// Wall-clock string for an event that happened at `when` millis, or an empty
  /// string when there is no clock or no event.
  std::string clock_at_(uint32_t when) const;
  static std::string ago_(uint32_t millis_since);

  static void toggle_cb_(lv_event_t *e);
  void on_toggle_();

  lv_obj_t *root_{nullptr};

  // --- header
  lv_obj_t *h_icon_{nullptr}, *h_name_{nullptr}, *h_key_{nullptr};
  std::string txt_name_, txt_key_, txt_icon_;

  // --- state and reason
  lv_obj_t *st_card_{nullptr}, *st_marker_{nullptr}, *st_word_{nullptr}, *st_reason_{nullptr};
  std::string txt_word_, txt_reason_;
  uint32_t st_colour_{0xFFFFFFFFu}, st_border_{0xFFFFFFFFu};

  // --- switch
  lv_obj_t *sw_card_{nullptr}, *sw_pos_{nullptr}, *sw_track_{nullptr}, *sw_knob_{nullptr};
  std::string txt_pos_;
  bool sw_shown_{false}, sw_on_{false};
  /// Optimistic feedback: the knob moves at once and the entity is given three
  /// seconds to agree (§5).
  uint32_t sw_pending_since_{0};
  bool sw_pending_{false}, sw_pending_target_{false};

  // --- power
  lv_obj_t *pw_card_{nullptr}, *pw_age_{nullptr}, *pw_second_{nullptr};
  Value pw_value_;
  std::string txt_age_, txt_second_;
  uint32_t age_colour_{0xFFFFFFFFu};

  std::vector<Extra> extras_;
  lv_obj_t *rows_caption_{nullptr};
  std::vector<Row> rows_;

  uint8_t shown_device_{INVALID_INDEX}, shown_terminal_{INVALID_INDEX};
  int16_t reason_h_{104};
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
