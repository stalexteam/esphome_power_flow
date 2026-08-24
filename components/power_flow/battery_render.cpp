#include "battery_render.h"

#ifdef USE_LVGL

#include "pf_palette.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace power_flow {

static const char *const TAG = "power_flow.battery";

// ---------------------------------------------------------------------------
// Geometry, transcribed from DEV/UI/BATTERY_UI_SPEC.md §1. Absolute pixels
// inside the scrolling content, origin top-left.
//
// The spec also mentions a `pad_top` of 52 on the scrolling child so the first
// block clears the `Back` button. It is not applied here: the y values in §1
// already clear it — the button occupies 12..52 and the ring starts at 68 — and
// padding on top of them would push everything down twice.
// ---------------------------------------------------------------------------
static const int16_t SCREEN_W = 480;
static const int16_t SCREEN_H = 800;
static const int16_t MARGIN = 16;
static const int16_t CONTENT_W = SCREEN_W - 2 * MARGIN;  // 448

static const int16_t BACK_X = 16, BACK_Y = 12, BACK_W = 96, BACK_H = 40;

static const int16_t RING_X = 140, RING_Y = 68, RING_D = 200;
static const int16_t RING_STROKE = 15;
static const int16_t SOC_NUM_NUDGE = 5;

/// Three across, twice: voltage/current/power, then the temperatures.
static const int16_t TRIPLE_W = 144, TRIPLE_H = 76, TRIPLE_R = 13;
static const int16_t TRIPLE_X[3] = {16, 168, 320};
static const int16_t TRIPLE_Y0 = 296;
static const int16_t TRIPLE_PITCH = 92;  // 76 + 16, §8's rule for a fourth row

static const int16_t TABLE_X = 16, TABLE_Y = 480, TABLE_W = CONTENT_W, TABLE_H = 274;
static const int16_t TABLE_R = 14;
static const int16_t TABLE_ROW_H = 36, TABLE_PAD_V = 8, TABLE_PAD_H = 20;

static const int16_t ERR_Y = 770, ERR_H = 60, ERR_R = 14;

static const int16_t CELLS_CAP_Y = 846;
static const int16_t CELL_Y0 = 870;
static const int16_t CELL_W = 216, CELL_H = 52, CELL_R = 12, CELL_PITCH = 60;
static const int16_t CELL_X[2] = {16, 248};
static const int16_t CELL_PAD = 16;

static const uint32_t RING_TRACK = 0x1C2A36;
static const uint32_t TABLE_LABEL = 0xA8C0D2;

static const uint32_t MDI_CHEVRON_LEFT = 0x0F0141;
static const uint32_t MDI_CHECK = 0x0F012C;
static const uint32_t MDI_ALERT_OUTLINE = 0x0F002A;

// ---------------------------------------------------------------------------
// Formatting. §9 of the flow spec plus the units this screen adds. Every one of
// them yields a dash when its input is missing — the dash is the only thing on
// either screen that is allowed to stand where a number should be.
// ---------------------------------------------------------------------------
static const char *const DASH = "\xE2\x80\x94";  // U+2014

static std::string fmt(float v, int decimals) {
  if (!is_valid(v))
    return DASH;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

/// The battery's own power carries a sign: it is the one figure on the screen
/// whose direction matters as much as its size (§9).
static std::string fmt_signed(float w) {
  if (!is_valid(w))
    return DASH;
  if (std::fabs(w) < 0.5f)
    return "0";
  char buf[24];
  snprintf(buf, sizeof(buf), "%+.0f", w);
  return buf;
}

static std::string utf8(uint32_t cp) {
  char b[5];
  b[0] = (char) (0xF0 | (cp >> 18));
  b[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
  b[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
  b[3] = (char) (0x80 | (cp & 0x3F));
  b[4] = 0;
  return b;
}

static int line_h(const lv_font_t *f, int fallback) {
  return f != nullptr ? (int) lv_font_get_line_height(f) : fallback;
}

static int text_w(const lv_font_t *f, const char *t) {
  if (f == nullptr || t == nullptr || t[0] == 0)
    return 0;
  lv_point_t sz;
  lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return (int) sz.x;
}

/// Distance from the top of a line box to the baseline, so a small unit can be
/// seated on a large number's baseline rather than on its box.
static int baseline_of(const lv_font_t *f, int fallback) {
  if (f == nullptr)
    return fallback;
  return (int) lv_font_get_line_height(f) - (int) f->base_line;
}

static void set_text(lv_obj_t *o, std::string &cache, const std::string &txt) {
  if (o == nullptr || cache == txt)
    return;
  cache = txt;
  lv_label_set_text(o, txt.c_str());
}

static void set_colour(lv_obj_t *o, uint32_t &cache, uint32_t c) {
  if (o == nullptr || cache == c)
    return;
  cache = c;
  lv_obj_set_style_text_color(o, lv_color_hex(c), LV_PART_MAIN);
}

static void set_hidden(lv_obj_t *o, bool hidden) {
  if (o == nullptr)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

lv_obj_t *BatteryScreen::plain_(lv_obj_t *parent) const {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  return o;
}

lv_obj_t *BatteryScreen::card_(lv_obj_t *parent, int x, int y, int w, int h, int radius) const {
  lv_obj_t *o = this->plain_(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lv_color_hex(pal::card), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(o, lv_color_hex(pal::border), LV_PART_MAIN);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  return o;
}

lv_obj_t *BatteryScreen::label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                                lv_text_align_t align, uint32_t colour) const {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_pos(l, x, y);
  if (w > 0)
    lv_obj_set_width(l, w);
  else
    lv_obj_set_width(l, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  if (font != nullptr)
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  // §8's last line: a label inside a card must set its colour or it inherits
  // the dark theme's and vanishes.
  lv_obj_set_style_text_color(l, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(l, "");
  return l;
}

void BatteryScreen::make_value_(Value &v, lv_obj_t *parent, const lv_font_t *num_font,
                                const lv_font_t *unit_font, int anchor, int y, bool centred) {
  v.anchor = (int16_t) anchor;
  v.centred = centred;
  v.num = this->label_(parent, num_font, 0, y, -1, LV_TEXT_ALIGN_LEFT, pal::text);
  const int dy = baseline_of(num_font, 20) - baseline_of(unit_font, 12);
  v.unit = this->label_(parent, unit_font, 0, y + dy, -1, LV_TEXT_ALIGN_LEFT, pal::text_dim);
}

void BatteryScreen::build_back_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  lv_obj_t *b = this->card_(root, BACK_X, BACK_Y, BACK_W, BACK_H, 12);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  // Same indirection as a node tap: the YAML decides what `Back` goes back to,
  // so the component carries no knowledge of pages (§11).
  lv_obj_set_user_data(b, this);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        auto *self = static_cast<BatteryScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
        if (self != nullptr && self->pf_ != nullptr && self->pf_->on_back() != nullptr)
          self->pf_->on_back()->trigger();
      },
      LV_EVENT_CLICKED, nullptr);

  // The chevron and the word are laid out as one group, centred in the button.
  const std::string chev = utf8(MDI_CHEVRON_LEFT);
  const int iw = text_w(s.icon, chev.c_str());
  const int tw = text_w(s.s16, "Back");
  const int gap = 6;
  const int total = iw + gap + tw;
  int x = (BACK_W - 2 - total) / 2;

  const int ih = line_h(s.icon, 26), th = line_h(s.s16, 20);
  lv_obj_t *ic = this->label_(b, s.icon, x, (BACK_H - 2 - ih) / 2, iw + 2, LV_TEXT_ALIGN_LEFT,
                              pal::text_dim);
  lv_label_set_text(ic, chev.c_str());
  lv_obj_t *tx = this->label_(b, s.s16, x + iw + gap, (BACK_H - 2 - th) / 2, tw + 2,
                              LV_TEXT_ALIGN_LEFT, pal::text);
  lv_label_set_text(tx, "Back");
}

void BatteryScreen::build_ring_(lv_obj_t *c) {
  const PowerFlowStyle &s = this->pf_->style();
  const int cx = RING_X + RING_D / 2, cy = RING_Y + RING_D / 2;

#if LV_USE_ARC
  // §4: 270 degrees, open at the bottom. LVGL measures angles clockwise from
  // three o'clock, so 135 -> 45 leaves the gap centred on six o'clock.
  this->ring_ = lv_arc_create(c);
  lv_obj_remove_style(this->ring_, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(this->ring_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(this->ring_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(this->ring_, RING_X, RING_Y);
  lv_obj_set_size(this->ring_, RING_D, RING_D);
  lv_obj_set_style_pad_all(this->ring_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->ring_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_arc_set_bg_angles(this->ring_, 135, 45);
  lv_arc_set_range(this->ring_, 0, 100);
  lv_arc_set_value(this->ring_, 0);
  lv_obj_set_style_arc_width(this->ring_, RING_STROKE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(this->ring_, RING_STROKE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(this->ring_, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(this->ring_, lv_color_hex(pal::batt), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(this->ring_, true, LV_PART_INDICATOR);
#endif

  // Three lines centred as one group: the number, the unit, the charge state.
  // §4 puts the state inside the ring's open bottom rather than in a band under
  // it, so the whole readout is one object to the eye.
  const int nh = line_h(s.m72, 90), ph = line_h(s.m28, 37), sh = line_h(s.s18, 22);
  const int gap = 2;
  const int total = nh + gap + ph + gap + 2 + sh;
  int y = cy - total / 2;

  // Optically 5 px lower than the arithmetic centre. At 72 px the digits carry a
  // lot of empty space below their baseline, so a group centred by line box sits
  // visibly high inside the ring. The `%` and the charge state do not move: the
  // nudge is applied to the label, not to the running y.
  this->soc_num_ =
      this->label_(c, s.m72, RING_X, y + SOC_NUM_NUDGE, RING_D, LV_TEXT_ALIGN_CENTER, pal::text);
  y += nh + gap;
  this->soc_pct_ = this->label_(c, s.m28, RING_X, y, RING_D, LV_TEXT_ALIGN_CENTER, pal::text_dim);
  lv_label_set_text(this->soc_pct_, "%");
  y += ph + gap + 2;
  this->charge_state_ = this->label_(c, s.s18, RING_X, y, RING_D, LV_TEXT_ALIGN_CENTER, pal::batt);
  lv_obj_set_style_text_letter_space(this->charge_state_, 1, LV_PART_MAIN);
}

void BatteryScreen::build_triples_(lv_obj_t *c) {
  const PowerFlowStyle &s = this->pf_->style();
  const BatteryDetails *d = this->det_();

  struct Slot {
    const char *caption;
    Value *v;
  };
  Slot first[3] = {{"VOLTAGE", &this->v_volt_}, {"CURRENT", &this->v_curr_},
                   {"POWER", &this->v_power_}};

  const int ch = line_h(s.s10, 12), vh = line_h(s.m22, 29);
  const int gap = 9;
  const int cap_y = (TRIPLE_H - 2 - (ch + gap + vh)) / 2;
  const int val_y = cap_y + ch + gap;

  for (int i = 0; i < 3; i++) {
    lv_obj_t *card = this->card_(c, TRIPLE_X[i], TRIPLE_Y0, TRIPLE_W, TRIPLE_H, TRIPLE_R);
    lv_obj_t *cap = this->label_(card, s.s10, 0, cap_y, TRIPLE_W - 2, LV_TEXT_ALIGN_CENTER,
                                 pal::text_dim);
    lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
    lv_label_set_text(cap, first[i].caption);
    this->make_value_(*first[i].v, card, s.m22, s.m12, (TRIPLE_W - 2) / 2, val_y, true);
  }

  // §8: the temperatures are however many are configured, three to a row, and
  // everything below moves down with them. This pack shows three, so the rest
  // of the screen sits where §1 puts it.
  const size_t n = d != nullptr ? d->temperatures.size() : 0;
  this->v_temp_.resize(n);
  for (size_t i = 0; i < n; i++) {
    const int col = (int) (i % 3), row = (int) (i / 3);
    const int y = TRIPLE_Y0 + TRIPLE_PITCH * (1 + row);
    lv_obj_t *card = this->card_(c, TRIPLE_X[col], y, TRIPLE_W, TRIPLE_H, TRIPLE_R);
    lv_obj_t *cap = this->label_(card, s.s10, 0, cap_y, TRIPLE_W - 2, LV_TEXT_ALIGN_CENTER,
                                 pal::text_dim);
    lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
    lv_label_set_text(cap, d->temperatures[i].first.c_str());
    this->make_value_(this->v_temp_[i], card, s.m22, s.m12, (TRIPLE_W - 2) / 2, val_y, true);
  }
}

void BatteryScreen::build_table_(lv_obj_t *c) {
  const PowerFlowStyle &s = this->pf_->style();
  lv_obj_t *card = this->card_(c, TABLE_X, TABLE_Y, TABLE_W, TABLE_H, TABLE_R);

  struct Row {
    const char *label;
    Value *v;
  };
  Row rows[7] = {{"Remaining", &this->t_remaining_},   {"Pack capacity", &this->t_capacity_},
                 {"Cycles", &this->t_cycles_},         {"Cell spread", &this->t_spread_},
                 {"Balancing", nullptr},               {"Charged today", &this->t_charged_},
                 {"Discharged today", &this->t_discharged_}};

  const int lh = line_h(s.s16, 20), vh = line_h(s.m16, 21);
  const int right = TABLE_W - 2 - TABLE_PAD_H;

  for (int i = 0; i < 7; i++) {
    const int top = TABLE_PAD_V + TABLE_ROW_H * i;
    lv_obj_t *lab = this->label_(card, s.s16, TABLE_PAD_H, top + (TABLE_ROW_H - lh) / 2, -1,
                                 LV_TEXT_ALIGN_LEFT, TABLE_LABEL);
    lv_label_set_text(lab, rows[i].label);

    if (rows[i].v != nullptr) {
      this->make_value_(*rows[i].v, card, s.m16, s.m12, right, top + (TABLE_ROW_H - vh) / 2,
                        false);
    } else {
      // Balancing is a word, not a value, so it is Montserrat and letter-spaced
      // like the other captions (§8).
      const int bh = line_h(s.s14, 18);
      this->t_balancing_ = this->label_(card, s.s14, TABLE_PAD_H, top + (TABLE_ROW_H - bh) / 2,
                                        right - TABLE_PAD_H, LV_TEXT_ALIGN_RIGHT, pal::text_dim);
      lv_obj_set_style_text_letter_space(this->t_balancing_, 1, LV_PART_MAIN);
    }

    // A rule between rows, inset, and none after the last.
    if (i < 6) {
      lv_obj_t *rule = this->plain_(card);
      lv_obj_set_pos(rule, TABLE_PAD_H, top + TABLE_ROW_H);
      lv_obj_set_size(rule, TABLE_W - 2 - 2 * TABLE_PAD_H, 1);
      lv_obj_set_style_bg_color(rule, lv_color_hex(pal::divider), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    }
  }
}

void BatteryScreen::build_errors_(lv_obj_t *c) {
  const PowerFlowStyle &s = this->pf_->style();
  lv_obj_t *card = this->card_(c, MARGIN, ERR_Y, CONTENT_W, ERR_H, ERR_R);
  const int ih = line_h(s.icon, 26), th = line_h(s.s16, 20);
  this->err_icon_ = this->label_(card, s.icon, 20, (ERR_H - 2 - ih) / 2, 24, LV_TEXT_ALIGN_LEFT,
                                 pal::batt);
  this->err_text_ = this->label_(card, s.s16, 20 + 24 + 11, (ERR_H - 2 - th) / 2,
                                 CONTENT_W - 2 - 20 - 24 - 11 - 20, LV_TEXT_ALIGN_LEFT,
                                 TABLE_LABEL);
  lv_label_set_long_mode(this->err_text_, LV_LABEL_LONG_MODE_WRAP);
}

void BatteryScreen::build_cells_(lv_obj_t *c) {
  const PowerFlowStyle &s = this->pf_->style();
  const BatteryDetails *d = this->det_();
  const size_t n = d != nullptr ? d->cells.size() : 0;

  lv_obj_t *cap = this->label_(c, s.s10, MARGIN, CELLS_CAP_Y, CONTENT_W, LV_TEXT_ALIGN_LEFT,
                               pal::text_dim);
  lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
  lv_label_set_text(cap, "CELLS, V");
  if (n == 0) {
    set_hidden(cap, true);
    return;
  }

  this->cells_.resize(n);
  const int ih = line_h(s.m16, 21), vh = line_h(s.m22, 29);
  for (size_t i = 0; i < n; i++) {
    // Filled left to right, then down (§8), so cell 1 is where the eye starts.
    const int col = (int) (i % 2), row = (int) (i / 2);
    Cell &cell = this->cells_[i];
    cell.box = this->card_(c, CELL_X[col], CELL_Y0 + CELL_PITCH * row, CELL_W, CELL_H, CELL_R);
    cell.index = this->label_(cell.box, s.m16, CELL_PAD, (CELL_H - 2 - ih) / 2, 40,
                              LV_TEXT_ALIGN_LEFT, pal::text_dim);
    // Wide enough for any 32-bit value: the schema caps the pack at 32 cells,
    // but nothing in this function says so and the compiler is right not to
    // assume it.
    char buf[12];
    snprintf(buf, sizeof(buf), "%u", (unsigned) (i + 1));
    lv_label_set_text(cell.index, buf);
    cell.value = this->label_(cell.box, s.m22, CELL_PAD, (CELL_H - 2 - vh) / 2,
                              CELL_W - 2 - 2 * CELL_PAD, LV_TEXT_ALIGN_RIGHT, pal::text);
  }
}

void BatteryScreen::setup(PowerFlow *pf) {
  this->pf_ = pf;
  lv_obj_t *root = pf->battery_parent();
  if (root == nullptr)
    return;

  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(root, lv_color_hex(pal::bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

  // §2: everything scrolls except `Back`. If the only way off the screen could
  // scroll away, a panel with no hardware key would strand the reader.
  this->scroll_ = this->plain_(root);
  lv_obj_set_pos(this->scroll_, 0, 0);
  lv_obj_set_size(this->scroll_, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scroll_dir(this->scroll_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(this->scroll_, LV_SCROLLBAR_MODE_AUTO);

  // A press has to land on something for LVGL to find a scrollable ancestor:
  // `lv_indev_search_obj` only considers clickable objects, and everything built
  // here is deliberately not clickable, so without this the finger hits nothing
  // and the page does not move. The container catches the press; LVGL then
  // decides between a click and a drag on its own.
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_CLICKABLE);

  // `lv_obj_remove_style_all` took the scrollbar's style with it, and §2 makes
  // the scrollbar the only affordance there is — no arrows, no caption. An
  // invisible one is the same as none.
  lv_obj_set_style_bg_color(this->scroll_, lv_color_hex(pal::divider), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(this->scroll_, LV_OPA_60, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(this->scroll_, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(this->scroll_, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(this->scroll_, 3, LV_PART_SCROLLBAR);

  this->build_ring_(this->scroll_);
  this->build_triples_(this->scroll_);
  this->build_table_(this->scroll_);
  this->build_errors_(this->scroll_);
  this->build_cells_(this->scroll_);
  this->build_back_(root);  // last, so it sits above the scrolling content

  const BatteryDetails *d = this->det_();
  const size_t cells = d != nullptr ? d->cells.size() : 0;
  const size_t temps = d != nullptr ? d->temperatures.size() : 0;
  ESP_LOGI(TAG, "battery screen: %u cells, %u temperatures, content %d px, highlight above %.0f mV",
           (unsigned) cells, (unsigned) temps,
           (int) (CELL_Y0 + CELL_PITCH * ((cells + 1) / 2) + 8),
           d != nullptr ? d->highlight_above_mv : 0.0f);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

const Device *BatteryScreen::dev_() const {
  for (const Device &d : this->pf_->devices())
    if (d.kind == DeviceKind::BATTERY)
      return &d;
  return nullptr;
}

const BatteryDetails *BatteryScreen::det_() const {
  const Device *d = this->dev_();
  return d != nullptr ? d->details.get() : nullptr;
}

void BatteryScreen::set_value_(Value &v, const std::string &num, const char *unit,
                               uint32_t colour) {
  if (v.num == nullptr)
    return;
  const bool changed = v.cache != num;
  set_text(v.num, v.cache, num);
  set_colour(v.num, v.colour, colour);
  if (v.unit != nullptr) {
    std::string u = unit == nullptr ? "" : unit;
    // A dash stands where a number would; its unit would be a claim about a
    // value we do not have.
    if (num == DASH)
      u.clear();
    lv_label_set_text(v.unit, u.c_str());
  }
  if (!changed)
    return;

  // Monospaced numerals mean this rarely moves, but "rarely" is not "never":
  // 99 becomes 100, and a sign appears and disappears.
  lv_obj_update_layout(v.num);
  const int wn = lv_obj_get_width(v.num);
  const int wu = v.unit != nullptr ? lv_obj_get_width(v.unit) : 0;
  const int gap = wu > 0 ? 4 : 0;
  const int total = wn + gap + wu;
  const int left = v.centred ? v.anchor - total / 2 : v.anchor - total;
  lv_obj_set_x(v.num, left);
  if (v.unit != nullptr)
    lv_obj_set_x(v.unit, left + wn + gap);
}

void BatteryScreen::update_ring_() {
  const Device *dev = this->dev_();
  const BatteryDetails *d = this->det_();
  const float soc = (dev != nullptr && dev->soc != nullptr) ? dev->soc->state : NAN;

  set_text(this->soc_num_, this->txt_soc_, fmt(soc, 0));
  set_hidden(this->soc_pct_, !is_valid(soc));

  // §4: the ring's colour follows the level, not the direction of flow. A pack
  // at 8 % is worth noticing whether it is charging or not.
  const uint32_t col = !is_valid(soc)   ? RING_TRACK
                       : soc < 10.0f    ? pal::alert
                       : soc < 20.0f    ? pal::discharge
                                        : pal::batt;
  const int32_t pct = is_valid(soc) ? (int32_t) lroundf(std::min(100.0f, std::max(0.0f, soc))) : 0;
#if LV_USE_ARC
  if (this->ring_ != nullptr) {
    if (pct != this->ring_pct_) {
      this->ring_pct_ = pct;
      lv_arc_set_value(this->ring_, pct);
    }
    if (col != this->ring_colour_) {
      this->ring_colour_ = col;
      lv_obj_set_style_arc_color(this->ring_, lv_color_hex(col), LV_PART_INDICATOR);
    }
  }
#endif
  if (this->charge_state_ != nullptr) {
    std::string st;
    if (d != nullptr && d->charge_status != nullptr && d->charge_status->has_state())
      st = d->charge_status->state;
    for (char &ch : st)
      ch = (char) toupper((unsigned char) ch);
    set_text(this->charge_state_, this->txt_state_, st);
    set_colour(this->charge_state_, this->state_colour_, col);
  }
}

void BatteryScreen::update_triples_() {
  const Device *dev = this->dev_();
  const BatteryDetails *d = this->det_();
  const float volt = (dev != nullptr && dev->voltage != nullptr) ? dev->voltage->state : NAN;
  const float curr = (d != nullptr && d->current != nullptr) ? d->current->state : NAN;
  const float pw = (d != nullptr && d->power != nullptr) ? d->power->state : NAN;

  this->set_value_(this->v_volt_, fmt(volt, 2), "V", pal::text);
  this->set_value_(this->v_curr_, fmt(curr, 1), "A", pal::text);

  const uint32_t pcol = !is_valid(pw) || std::fabs(pw) < 0.5f ? pal::text
                        : pw > 0                              ? pal::batt
                                                              : pal::discharge;
  this->set_value_(this->v_power_, fmt_signed(pw), "W", pcol);

  for (size_t i = 0; i < this->v_temp_.size(); i++) {
    sensor::Sensor *s = d->temperatures[i].second;
    this->set_value_(this->v_temp_[i], fmt(s != nullptr ? s->state : NAN, 1), "\xC2\xB0" "C",
                     pal::text);
  }
}

void BatteryScreen::update_table_() {
  const Device *dev = this->dev_();
  const BatteryDetails *d = this->det_();
  if (d == nullptr)
    return;
  auto val = [](sensor::Sensor *s) { return s != nullptr ? s->state : NAN; };

  this->set_value_(this->t_remaining_,
                   fmt(dev != nullptr ? val(dev->capacity_remaining) : NAN, 1), "Ah", pal::text);
  this->set_value_(this->t_capacity_, fmt(val(d->capacity_total), 0), "Ah", pal::text);
  this->set_value_(this->t_cycles_, fmt(val(d->cycles), 0), nullptr, pal::text);

  // The BMS reports the spread in volts; the screen has always shown it in
  // millivolts, which is the only scale at which the number says anything.
  const float dv = val(d->cell_delta);
  this->set_value_(this->t_spread_, fmt(is_valid(dv) ? dv * 1000.0f : NAN, 0), "mV", pal::text);

  this->set_value_(this->t_charged_, fmt(val(d->energy_charged), 2), "kWh", pal::batt);
  this->set_value_(this->t_discharged_, fmt(val(d->energy_discharged), 2), "kWh", pal::discharge);

  const float bal = val(d->balancing);
  // `balancing` is a number, so OFF is our reading of zero rather than a state
  // the BMS names.
  set_text(this->t_balancing_, this->txt_balancing_,
           !is_valid(bal) ? DASH : (bal > 0.5f ? "ON" : "OFF"));
}

void BatteryScreen::update_errors_() {
  const BatteryDetails *d = this->det_();
  std::string txt;
  uint32_t icol = pal::batt;
  std::string icon = utf8(MDI_CHECK);

  if (d == nullptr || d->errors == nullptr) {
    txt = DASH;
    icol = pal::alert;
    icon = "\xC3\x97";
  } else if (!d->errors->has_state()) {
    txt = DASH;
    icol = pal::alert;
    icon = "\xC3\x97";
  } else if (d->errors->state.empty()) {
    // The BMS says nothing when nothing is wrong; the words are ours. §7: the
    // card never disappears, because an empty errors panel is the statement
    // "nothing is wrong" and a missing one reads as a bug.
    txt = "No errors";
  } else {
    txt = d->errors->state;
    icol = pal::alert;
    icon = utf8(MDI_ALERT_OUTLINE);
  }

  set_text(this->err_text_, this->txt_err_, txt);
  set_colour(this->err_icon_, this->err_colour_, icol);
  if (this->err_icon_ != nullptr)
    lv_label_set_text(this->err_icon_, icon.c_str());
}

void BatteryScreen::update_cells_() {
  const BatteryDetails *d = this->det_();
  if (d == nullptr || this->cells_.empty())
    return;

  float lo = NAN, hi = NAN;
  size_t i_lo = 0, i_hi = 0;
  for (size_t i = 0; i < this->cells_.size(); i++) {
    const float v = d->cells[i] != nullptr ? d->cells[i]->state : NAN;
    if (!is_valid(v))
      continue;
    if (!is_valid(hi) || v > hi) {
      hi = v;
      i_hi = i;
    }
    if (!is_valid(lo) || v < lo) {
      lo = v;
      i_lo = i;
    }
  }
  // §8: below the threshold, highlight nothing. A 1 mV spread on a healthy pack
  // would otherwise paint two cells every day of its life, and a warning that is
  // always on is not a warning.
  const bool mark = is_valid(hi) && is_valid(lo) &&
                    (hi - lo) * 1000.0f > d->highlight_above_mv && i_hi != i_lo;

  for (size_t i = 0; i < this->cells_.size(); i++) {
    Cell &c = this->cells_[i];
    const float v = d->cells[i] != nullptr ? d->cells[i]->state : NAN;
    set_text(c.value, c.cache, fmt(v, 3));

    uint32_t vcol = pal::text, bcol = pal::border;
    if (mark && i == i_hi) {
      vcol = pal::batt;
      bcol = 0x2E5B47;
    } else if (mark && i == i_lo) {
      vcol = pal::discharge;
      bcol = 0x3D3A28;
    }
    set_colour(c.value, c.colour, vcol);
    if (bcol != c.border) {
      c.border = bcol;
      lv_obj_set_style_border_color(c.box, lv_color_hex(bcol), LV_PART_MAIN);
    }
  }
}

void BatteryScreen::update() {
  if (this->pf_ == nullptr || this->scroll_ == nullptr)
    return;
  this->update_ring_();
  this->update_triples_();
  this->update_table_();
  this->update_errors_();
  this->update_cells_();
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
