#include "load_render.h"

#ifdef USE_LVGL

#include "pf_palette.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace esphome {
namespace power_flow {

static const char *const TAG = "power_flow.load";

// ---------------------------------------------------------------------------
// Geometry, DEV/UI/LOAD_UI_SPEC.md §2. Absolute pixels, origin top-left. Only
// the block heights and the gap are constants: the y of every block is computed
// by layout_(), because which blocks apply depends on the subject and §2 is
// explicit that the cursor is calculated rather than tabulated per consumer.
// ---------------------------------------------------------------------------
static const int16_t SCREEN_W = 480, SCREEN_H = 800;
static const int16_t MARGIN = 16;
static const int16_t CONTENT_W = SCREEN_W - 2 * MARGIN;  // 448
static const int16_t GAP = 16;

static const int16_t BACK_X = 16, BACK_Y = 12, BACK_W = 96, BACK_H = 40;
static const int16_t HEADER_Y = 66, HEADER_H = 48;
static const int16_t STATE_H1 = 104, STATE_H2 = 128;  // one reason line, or two
static const int16_t SWITCH_H = 72;
static const int16_t POWER_H = 112;
static const int16_t EXTRA_W = 144, EXTRA_H = 76, EXTRA_R = 13;
static const int16_t EXTRA_X[3] = {16, 168, 320};
static const int16_t ROWS_CAP_H = 16;
static const int16_t ROW_H = 52, ROW_PITCH = 60, ROW_R = 12;
static const int16_t CARD_R = 14;
static const int16_t PAD_X = 20, PAD_Y = 16;

static const uint32_t REASON_TEXT = 0xA8C0D2;
static const uint32_t STATE_BORDER_NODATA = 0x3D2E24;
static const uint32_t SW_TRACK_ON = 0x2E6B47;
static const uint32_t SW_TRACK_OFF = 0x1B2733;
static const uint32_t SW_BORDER_OFF = 0x2E3E4C;

static const uint32_t MDI_CHEVRON_LEFT = 0x0F0141;
static const uint32_t MDI_PLUG_OUTLINE = 0x0F1425;
/// The state marker. A glyph rather than U+25CF, which is in neither face's
/// character set — and rather than a drawn circle, so the dot and the ✕ stay
/// one label whose text is swapped.
static const uint32_t MDI_CIRCLE = 0x0F09DE;
static const uint32_t MDI_CLOSE_THICK = 0x0F1398;

/// The most cards §7 can produce, so the tree is built once and hidden per
/// subject rather than rebuilt on every tap.
static const size_t MAX_EXTRAS = 4;
/// power, switch, voltage, temperature, link.
static const size_t MAX_ROWS = 6;

static const char *const DASH = "\xE2\x80\x94";

// ---------------------------------------------------------------------------

static std::string fmt_power(float w) {
  if (!is_valid(w))
    return DASH;
  char buf[24];
  if (std::fabs(w) >= 500.0f)
    snprintf(buf, sizeof(buf), "%.2f", w / 1000.0f);
  else
    snprintf(buf, sizeof(buf), "%.0f", w);
  return buf;
}

static const char *power_unit(float w) {
  return !is_valid(w) ? "" : (std::fabs(w) >= 500.0f ? "kW" : "W");
}

static std::string fmt_num(float v, int decimals) {
  if (!is_valid(v))
    return DASH;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
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

lv_obj_t *LoadScreen::plain_(lv_obj_t *parent) const {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  return o;
}

lv_obj_t *LoadScreen::card_(lv_obj_t *parent, int x, int y, int w, int h, int radius) const {
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

lv_obj_t *LoadScreen::label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                             lv_text_align_t align, uint32_t colour) const {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w > 0 ? w : LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  if (font != nullptr)
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(l, "");
  return l;
}

void LoadScreen::make_value_(Value &v, lv_obj_t *parent, const lv_font_t *num_font,
                             const lv_font_t *unit_font, int anchor, int y, bool centred) {
  v.anchor = (int16_t) anchor;
  v.centred = centred;
  v.num = this->label_(parent, num_font, 0, y, -1, LV_TEXT_ALIGN_LEFT, pal::text);
  const int dy = baseline_of(num_font, 20) - baseline_of(unit_font, 12);
  v.unit = this->label_(parent, unit_font, 0, y + dy, -1, LV_TEXT_ALIGN_LEFT, pal::text_dim);
}

void LoadScreen::set_value_(Value &v, const std::string &num, const char *unit, uint32_t colour) {
  if (v.num == nullptr)
    return;
  const bool changed = v.cache != num;
  set_text(v.num, v.cache, num);
  set_colour(v.num, v.colour, colour);
  if (v.unit != nullptr) {
    // A dash stands where a number would; a unit beside it would be a claim
    // about a value we do not have.
    lv_label_set_text(v.unit, num == DASH || unit == nullptr ? "" : unit);
  }
  if (!changed)
    return;
  lv_obj_update_layout(v.num);
  const int wn = lv_obj_get_width(v.num);
  const int wu = v.unit != nullptr ? lv_obj_get_width(v.unit) : 0;
  const int gap = wu > 0 ? 5 : 0;
  const int left = v.centred ? v.anchor - (wn + gap + wu) / 2 : v.anchor;
  lv_obj_set_x(v.num, left);
  if (v.unit != nullptr)
    lv_obj_set_x(v.unit, left + wn + gap);
}

void LoadScreen::build_back_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  lv_obj_t *b = this->card_(root, BACK_X, BACK_Y, BACK_W, BACK_H, 12);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(b, this);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        auto *self = static_cast<LoadScreen *>(lv_event_get_user_data(e));
        if (self != nullptr && self->pf_ != nullptr && self->pf_->on_back() != nullptr)
          self->pf_->on_back()->trigger();
      },
      LV_EVENT_CLICKED, this);

  const std::string chev = utf8(MDI_CHEVRON_LEFT);
  const int iw = text_w(s.icon, chev.c_str()), tw = text_w(s.s16, "Back");
  const int x = (BACK_W - 2 - (iw + 6 + tw)) / 2;
  lv_obj_t *ic = this->label_(b, s.icon, x, (BACK_H - 2 - line_h(s.icon, 26)) / 2, iw + 2,
                              LV_TEXT_ALIGN_LEFT, pal::text_dim);
  lv_label_set_text(ic, chev.c_str());
  lv_obj_t *tx = this->label_(b, s.s16, x + iw + 6, (BACK_H - 2 - line_h(s.s16, 20)) / 2, tw + 2,
                              LV_TEXT_ALIGN_LEFT, pal::text);
  lv_label_set_text(tx, "Back");
}

void LoadScreen::build_header_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  const int nh = line_h(s.s22, 27), kh = line_h(s.m14, 18);
  const int top = HEADER_Y + (HEADER_H - (nh + kh)) / 2;
  this->h_icon_ = this->label_(root, s.icon, MARGIN, HEADER_Y + (HEADER_H - 30) / 2, 32,
                               LV_TEXT_ALIGN_LEFT, pal::load);
  const int tx = MARGIN + 30 + 13;
  this->h_name_ = this->label_(root, s.s22, tx, top, CONTENT_W - 30 - 13, LV_TEXT_ALIGN_LEFT,
                               pal::text);
  this->h_key_ = this->label_(root, s.m14, tx, top + nh, CONTENT_W - 30 - 13, LV_TEXT_ALIGN_LEFT,
                              pal::text_off);
}

void LoadScreen::build_state_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  this->st_card_ = this->card_(root, MARGIN, 0, CONTENT_W, STATE_H2, CARD_R);
  lv_obj_t *cap = this->label_(this->st_card_, s.s10, PAD_X, PAD_Y - 2, CONTENT_W - 2 * PAD_X,
                               LV_TEXT_ALIGN_LEFT, pal::text_dim);
  lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
  lv_label_set_text(cap, "STATE");

  // The marker is a label rather than a shape: a dot and a ✕ are the same slot
  // in two states, and swapping the glyph is cheaper than swapping objects.
  this->st_marker_ = this->label_(this->st_card_, s.icon, PAD_X, PAD_Y + 16, 26,
                                  LV_TEXT_ALIGN_LEFT, pal::load);
  this->st_word_ = this->label_(this->st_card_, s.s22, PAD_X + 26, PAD_Y + 16,
                                CONTENT_W - 2 * PAD_X - 26, LV_TEXT_ALIGN_LEFT, pal::text);
  this->st_reason_ = this->label_(this->st_card_, s.s14, PAD_X, PAD_Y + 48,
                                  CONTENT_W - 2 * PAD_X, LV_TEXT_ALIGN_LEFT, REASON_TEXT);
  lv_label_set_long_mode(this->st_reason_, LV_LABEL_LONG_MODE_WRAP);
}

void LoadScreen::build_switch_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  this->sw_card_ = this->card_(root, MARGIN, 0, CONTENT_W, SWITCH_H, CARD_R);
  lv_obj_t *cap = this->label_(this->sw_card_, s.s10, PAD_X, 14, 200, LV_TEXT_ALIGN_LEFT,
                               pal::text_dim);
  lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
  // §5: `SWITCH`, not `SOCKET` — half these loads are not sockets.
  lv_label_set_text(cap, "SWITCH");
  this->sw_pos_ = this->label_(this->sw_card_, s.s16, PAD_X, 34, 200, LV_TEXT_ALIGN_LEFT,
                               pal::text);

  this->sw_track_ = this->plain_(this->sw_card_);
  lv_obj_set_pos(this->sw_track_, CONTENT_W - 2 - PAD_X - 84, (SWITCH_H - 2 - 44) / 2);
  lv_obj_set_size(this->sw_track_, 84, 44);
  lv_obj_set_style_radius(this->sw_track_, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->sw_track_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(this->sw_track_, 1, LV_PART_MAIN);
  lv_obj_set_style_border_opa(this->sw_track_, LV_OPA_COVER, LV_PART_MAIN);
  // The only touch target on the screen, and the only thing that changes
  // anything. 84 x 44 clears the 44 px minimum on its short axis (§9).
  lv_obj_add_flag(this->sw_track_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(this->sw_track_, this);
  lv_obj_add_event_cb(this->sw_track_, LoadScreen::toggle_cb_, LV_EVENT_CLICKED, this);

  this->sw_knob_ = this->plain_(this->sw_track_);
  lv_obj_set_size(this->sw_knob_, 34, 34);
  lv_obj_set_style_radius(this->sw_knob_, 17, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->sw_knob_, LV_OPA_COVER, LV_PART_MAIN);
}

void LoadScreen::build_power_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  this->pw_card_ = this->card_(root, MARGIN, 0, CONTENT_W, POWER_H, CARD_R);
  lv_obj_t *cap = this->label_(this->pw_card_, s.s10, PAD_X, PAD_Y - 2, 120, LV_TEXT_ALIGN_LEFT,
                               pal::text_dim);
  lv_obj_set_style_text_letter_space(cap, 2, LV_PART_MAIN);
  lv_label_set_text(cap, "POWER");
  this->pw_age_ = this->label_(this->pw_card_, s.s14, CONTENT_W - 2 - PAD_X - 240, PAD_Y - 4, 240,
                               LV_TEXT_ALIGN_RIGHT, pal::text_dim);
  this->make_value_(this->pw_value_, this->pw_card_, s.m28, s.m14, PAD_X, PAD_Y + 30, false);
  // §6: the diagram's badge shows the averaged figure and this screen shows the
  // instantaneous one. Without both here, the two screens look like they
  // disagree with each other.
  this->pw_second_ = this->label_(this->pw_card_, s.s14, PAD_X + 210, PAD_Y + 42,
                                  CONTENT_W - 2 - PAD_X - (PAD_X + 210), LV_TEXT_ALIGN_RIGHT,
                                  pal::text_off);
}

void LoadScreen::build_extras_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  this->extras_.resize(MAX_EXTRAS);
  const int ch = line_h(s.s10, 12), vh = line_h(s.m22, 29);
  const int cap_y = (EXTRA_H - 2 - (ch + 9 + vh)) / 2;
  for (size_t i = 0; i < MAX_EXTRAS; i++) {
    Extra &e = this->extras_[i];
    e.card = this->card_(root, EXTRA_X[i % 3], 0, EXTRA_W, EXTRA_H, EXTRA_R);
    e.caption = this->label_(e.card, s.s10, 0, cap_y, EXTRA_W - 2, LV_TEXT_ALIGN_CENTER,
                             pal::text_dim);
    lv_obj_set_style_text_letter_space(e.caption, 1, LV_PART_MAIN);
    this->make_value_(e.value, e.card, s.m22, s.m12, (EXTRA_W - 2) / 2, cap_y + ch + 9, true);
  }
}

void LoadScreen::build_rows_(lv_obj_t *root) {
  const PowerFlowStyle &s = this->pf_->style();
  this->rows_caption_ = this->label_(root, s.s10, MARGIN, 0, CONTENT_W, LV_TEXT_ALIGN_LEFT,
                                     pal::text_dim);
  lv_obj_set_style_text_letter_space(this->rows_caption_, 2, LV_PART_MAIN);
  lv_label_set_text(this->rows_caption_, "ENTITIES");

  this->rows_.resize(MAX_ROWS);
  for (size_t i = 0; i < MAX_ROWS; i++) {
    Row &r = this->rows_[i];
    r.card = this->card_(root, MARGIN, 0, CONTENT_W, ROW_H, ROW_R);
    r.label = this->label_(r.card, s.s10, 16, 9, 200, LV_TEXT_ALIGN_LEFT, pal::text_dim);
    lv_obj_set_style_text_letter_space(r.label, 2, LV_PART_MAIN);
    r.flag = this->label_(r.card, s.s10, CONTENT_W - 2 - 16 - 180, 9, 180, LV_TEXT_ALIGN_RIGHT,
                          pal::alert);
    lv_obj_set_style_text_letter_space(r.flag, 2, LV_PART_MAIN);
    // Never truncated: a half-shown entity id is useless for the one job this
    // block has (§8).
    r.id = this->label_(r.card, s.m14, 16, 26, CONTENT_W - 2 - 32, LV_TEXT_ALIGN_LEFT,
                        REASON_TEXT);
  }
}

void LoadScreen::setup(PowerFlow *pf) {
  this->pf_ = pf;
  this->root_ = pf->load_parent();
  if (this->root_ == nullptr)
    return;
  lv_obj_remove_flag(this->root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(this->root_, lv_color_hex(pal::bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->root_, LV_OPA_COVER, LV_PART_MAIN);

  // Everything scrolls except `Back` — the battery screen's rule, for the
  // same reason: if the only way off the screen could scroll away, a panel
  // with no hardware key would strand the reader. Needed since a card can
  // carry four extra tiles and six entity rows — taller than the screen.
  this->scroll_ = this->plain_(this->root_);
  lv_obj_set_pos(this->scroll_, 0, 0);
  lv_obj_set_size(this->scroll_, SCREEN_W, SCREEN_H);
  // plain_() strips CLICKABLE, but a scroll container must receive presses
  // or it never scrolls — the battery screen re-adds it the same way.
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scroll_dir(this->scroll_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(this->scroll_, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_bottom(this->scroll_, GAP, LV_PART_MAIN);

  this->build_header_(this->scroll_);
  this->build_state_(this->scroll_);
  this->build_switch_(this->scroll_);
  this->build_power_(this->scroll_);
  this->build_extras_(this->scroll_);
  this->build_rows_(this->scroll_);
  this->build_back_(this->root_);  // pinned above the scroll

  ESP_LOGI(TAG, "load screen built: up to %u extra cards, %u entity rows",
           (unsigned) MAX_EXTRAS, (unsigned) MAX_ROWS);
}

// ---------------------------------------------------------------------------
// Subject resolution
// ---------------------------------------------------------------------------

const Device *LoadScreen::dev_() const {
  const uint8_t d = this->pf_->selected_device();
  return d == INVALID_INDEX || d >= this->pf_->devices().size() ? nullptr
                                                                : &this->pf_->devices()[d];
}

const Terminal *LoadScreen::term_() const {
  const uint8_t t = this->pf_->selected_terminal();
  return t == INVALID_INDEX || t >= this->pf_->terminals().size() ? nullptr
                                                                  : &this->pf_->terminals()[t];
}

/// A device's card carries the device's extras; a consumer's carries its own.
const NodeDetails *LoadScreen::extra_() const {
  if (const Device *d = this->dev_())
    return d->extra.get();
  const Terminal *t = this->term_();
  return t != nullptr ? t->extra.get() : nullptr;
}

/// A device's `voltage:` is subscribed once, on the device, for the flow card;
/// the detail card reads the same sensor rather than a second subscription.
static sensor::Sensor *voltage_of(const NodeDetails *x, const Device *d) {
  if (x != nullptr && x->voltage != nullptr)
    return x->voltage;
  return d != nullptr ? d->voltage : nullptr;
}

std::string LoadScreen::clock_at_(uint32_t when) const {
  if (when == 0 || this->pf_->rtc() == nullptr)
    return "";
  auto now = this->pf_->rtc()->now();
  if (!now.is_valid())
    return "";
  const uint32_t ago = (uint32_t) (millis() - when) / 1000u;
  const time_t then = now.timestamp - (time_t) ago;
  auto t = ESPTime::from_epoch_local(then);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", t.hour, t.minute);
  return buf;
}

std::string LoadScreen::ago_(uint32_t since) {
  const uint32_t s = since / 1000u;
  char buf[24];
  if (s < 90)
    snprintf(buf, sizeof(buf), "%u s", (unsigned) s);
  else if (s < 5400)
    snprintf(buf, sizeof(buf), "%u min", (unsigned) (s / 60));
  else
    snprintf(buf, sizeof(buf), "%u h", (unsigned) (s / 3600));
  return buf;
}

// ---------------------------------------------------------------------------
// Layout — §2: the cursor is computed, never tabulated
// ---------------------------------------------------------------------------

void LoadScreen::layout_() {
  const NodeDetails *x = this->extra_();
  const bool has_switch = x != nullptr && x->control != nullptr;

  int y = HEADER_Y + HEADER_H + GAP;

  lv_obj_set_pos(this->st_card_, MARGIN, y);
  lv_obj_set_height(this->st_card_, this->reason_h_);
  y += this->reason_h_ + GAP;

  set_hidden(this->sw_card_, !has_switch);
  if (has_switch) {
    lv_obj_set_pos(this->sw_card_, MARGIN, y);
    y += SWITCH_H + GAP;
  }

  lv_obj_set_pos(this->pw_card_, MARGIN, y);
  y += POWER_H + GAP;

  // One card per entity the subject actually has: an absent entity produces no
  // card at all, and the row simply gets shorter (§7).
  size_t n = 0;
  if (x != nullptr) {
    for (sensor::Sensor *s : {voltage_of(x, this->dev_()), x->current, x->temperature, x->link}) {
      if (s == nullptr)
        continue;
      Extra &e = this->extras_[n];
      lv_obj_set_pos(e.card, EXTRA_X[n % 3], y + 92 * (int) (n / 3));
      set_hidden(e.card, false);
      n++;
    }
  }
  for (size_t i = n; i < MAX_EXTRAS; i++)
    set_hidden(this->extras_[i].card, true);
  if (n > 0)
    y += EXTRA_H * (int) ((n + 2) / 3) + 16 * (int) ((n + 2) / 3 - 1) + GAP;

  lv_obj_set_pos(this->rows_caption_, MARGIN, y);
  y += ROWS_CAP_H + 8;
  size_t r = 0;
  if (x != nullptr) {
    for (const char *id : {x->power_id, x->switch_id, x->voltage_id, x->temperature_id,
                           x->link_id}) {
      if (id == nullptr)
        continue;
      lv_obj_set_pos(this->rows_[r].card, MARGIN, y + ROW_PITCH * (int) r);
      set_hidden(this->rows_[r].card, false);
      r++;
    }
  }
  for (size_t i = r; i < MAX_ROWS; i++)
    set_hidden(this->rows_[i].card, true);
  set_hidden(this->rows_caption_, r == 0);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void LoadScreen::update_header_() {
  const Device *d = this->dev_();
  const Terminal *t = this->term_();
  const std::string name = d != nullptr ? d->name : (t != nullptr ? t->name : "");
  const std::string key = d != nullptr ? d->key : (t != nullptr ? t->id : "");
  std::string icon = d != nullptr ? d->icon : (t != nullptr ? t->icon : "");
  if (icon.empty())
    icon = utf8(MDI_PLUG_OUTLINE);

  const bool dead = t != nullptr && (t->state == EdgeState::DE_ENERGIZED ||
                                     t->state == EdgeState::OPEN);
  set_text(this->h_name_, this->txt_name_, name);
  set_text(this->h_key_, this->txt_key_, key);
  set_text(this->h_icon_, this->txt_icon_, icon);
  lv_obj_set_style_text_color(this->h_name_, lv_color_hex(dead ? pal::text_off : pal::text),
                              LV_PART_MAIN);
  lv_obj_set_style_text_color(this->h_icon_, lv_color_hex(dead ? pal::icon_off : pal::load),
                              LV_PART_MAIN);
}

void LoadScreen::update_state_() {
  const Terminal *t = this->term_();
  const NodeDetails *x = this->extra_();
  if (t == nullptr)
    return;

  const uint32_t last = this->pf_->last_update(*t);
  const uint32_t age = last == 0 ? 0 : (uint32_t) (millis() - last);
  const std::string at = this->clock_at_(last);
  char buf[192];
  std::string word, marker;
  uint32_t colour = pal::load, border = pal::border;

  switch (t->state) {
    case EdgeState::OPEN:
      word = "Switched off";
      marker = utf8(MDI_CIRCLE);
      colour = pal::text_dim;
      snprintf(buf, sizeof(buf), "%s reports off%s%s",
               x != nullptr && x->switch_id != nullptr ? x->switch_id : "the switch",
               at.empty() ? "" : " since ", at.c_str());
      break;
    case EdgeState::DE_ENERGIZED:
    case EdgeState::NO_DATA:
      word = "No data";
      marker = utf8(MDI_CLOSE_THICK);
      colour = pal::alert;
      border = STATE_BORDER_NODATA;
      // §4: name the entity, name the time, name the threshold. A reason that
      // says "unavailable" without saying since when is not worth the pixels.
      snprintf(buf, sizeof(buf), "Nothing from %s%s%s%s%s",
               x != nullptr && x->power_id != nullptr ? x->power_id : "its meter",
               at.empty() ? "" : " since ", at.c_str(),
               age == 0 ? "" : " — ", age == 0 ? "" : ago_(age).c_str());
      break;
    case EdgeState::IDLE:
      word = "Idle";
      marker = utf8(MDI_CIRCLE);
      colour = pal::text_off;
      snprintf(buf, sizeof(buf), "Below the idle threshold of %.0f W", this->pf_->idle_below());
      break;
    default:
      word = "Active";
      marker = utf8(MDI_CIRCLE);
      colour = pal::load;
      snprintf(buf, sizeof(buf), "Draw above the idle threshold of %.0f W",
               this->pf_->idle_below());
      break;
  }

  set_text(this->st_word_, this->txt_word_, word);
  lv_label_set_text(this->st_marker_, marker.c_str());
  lv_obj_set_style_text_color(this->st_marker_, lv_color_hex(colour), LV_PART_MAIN);
  set_colour(this->st_word_, this->st_colour_, colour);
  set_text(this->st_reason_, this->txt_reason_, buf);
  if (border != this->st_border_) {
    this->st_border_ = border;
    lv_obj_set_style_border_color(this->st_card_, lv_color_hex(border), LV_PART_MAIN);
  }
}

void LoadScreen::update_switch_() {
  const NodeDetails *x = this->extra_();
  if (x == nullptr || x->control == nullptr)
    return;
  const Terminal *t = this->term_();

  // The reported position, and decision log #6: absent, `unknown` and `null` all
  // mean closed, so a plug that has not spoken since a restart reads as on.
  bool on = x->control->state;
  if (t != nullptr && t->sw == SwitchState::OFF)
    on = false;
  else if (t != nullptr && t->sw != SwitchState::ON)
    on = true;

  // Optimistic: the knob moved on the tap, and the entity has three seconds to
  // agree before it snaps back (§5).
  if (this->sw_pending_) {
    if (on == this->sw_pending_target_) {
      this->sw_pending_ = false;
    } else if ((uint32_t) (millis() - this->sw_pending_since_) > 3000u) {
      this->sw_pending_ = false;
      set_text(this->st_reason_, this->txt_reason_, "switch did not respond");
    } else {
      on = this->sw_pending_target_;
    }
  }

  set_text(this->sw_pos_, this->txt_pos_, on ? "On" : "Off");
  if (on != this->sw_on_ || !this->sw_shown_) {
    this->sw_on_ = on;
    this->sw_shown_ = true;
    lv_obj_set_style_bg_color(this->sw_track_, lv_color_hex(on ? SW_TRACK_ON : SW_TRACK_OFF),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(this->sw_track_, lv_color_hex(on ? pal::batt : SW_BORDER_OFF),
                                  LV_PART_MAIN);
    lv_obj_set_style_bg_color(this->sw_knob_, lv_color_hex(on ? pal::batt : pal::text_off),
                              LV_PART_MAIN);
    lv_obj_set_pos(this->sw_knob_, on ? 84 - 34 - 5 : 5, (44 - 34) / 2);
  }
}

void LoadScreen::toggle_cb_(lv_event_t *e) {
  // `self` comes from the registration, not from the object: that is the form
  // the node taps already use and demonstrably works on this build.
  auto *self = static_cast<LoadScreen *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  ESP_LOGI(TAG, "toggle tapped");
  self->on_toggle_();
}

void LoadScreen::on_toggle_() {
  const NodeDetails *x = this->extra_();
  if (x == nullptr || x->control == nullptr)
    return;
  const bool target = !this->sw_on_;
  this->sw_pending_ = true;
  this->sw_pending_target_ = target;
  this->sw_pending_since_ = millis();
  if (target)
    x->control->turn_on();
  else
    x->control->turn_off();
  ESP_LOGI(TAG, "commanding %s -> %s", x->switch_id != nullptr ? x->switch_id : "switch",
           target ? "on" : "off");
  this->update_switch_();
}

void LoadScreen::update_power_() {
  const Terminal *t = this->term_();
  if (t == nullptr) {
    // A subject with no terminal has no reading of its own. The card is a
    // shared widget tree, so leaving it alone would show the previous
    // subject's number — a dash is the truthful display.
    this->set_value_(this->pw_value_, DASH, nullptr, pal::text_off);
    set_text(this->pw_age_, this->txt_age_, "");
    set_text(this->pw_second_, this->txt_second_, "");
    return;
  }
  const float now_v = t->display;
  const float avg = t->value;
  const uint32_t last = this->pf_->last_update(*t);
  const uint32_t age = last == 0 ? 0 : (uint32_t) (millis() - last);

  const bool nodata = t->state == EdgeState::DE_ENERGIZED || t->state == EdgeState::NO_DATA;
  this->set_value_(this->pw_value_, nodata ? DASH : fmt_power(now_v),
                   nodata ? nullptr : power_unit(now_v), nodata ? pal::text_off : pal::text);

  std::string age_txt;
  uint32_t age_col = pal::text_dim;
  const std::string seen = this->clock_at_(last);
  if (t->stale && !seen.empty()) {
    age_txt = "stale — last seen " + seen;
    age_col = pal::alert;
  } else if (last != 0) {
    age_txt = "updated " + ago_(age) + " ago";
  }
  set_text(this->pw_age_, this->txt_age_, age_txt);
  set_colour(this->pw_age_, this->age_colour_, age_col);

  std::string second;
  if (nodata)
    second = is_valid(avg) ? "last value " + fmt_power(avg) + " " + power_unit(avg) : "";
  else if (t->state == EdgeState::OPEN)
    second = "reporting, not drawing";
  else if (is_valid(avg))
    // The label states the configured window rather than assuming 60 s.
    second = std::to_string(this->pf_->average_window() / 1000u) + " s avg " + fmt_power(avg) +
             " " + power_unit(avg);
  set_text(this->pw_second_, this->txt_second_, second);
}

void LoadScreen::update_extras_() {
  const NodeDetails *x = this->extra_();
  if (x == nullptr)
    return;
  struct Def {
    sensor::Sensor *s;
    const char *cap;
    const char *unit;
    int decimals;
  };
  const Def defs[4] = {{voltage_of(x, this->dev_()), "VOLTAGE", "V", 0},
                       {x->current, "CURRENT", "A", 1},
                       {x->temperature, "TEMPERATURE", "\xC2\xB0" "C", 1},
                       {x->link, "LINK", "lqi", 0}};
  size_t n = 0;
  for (const Def &d : defs) {
    if (d.s == nullptr)
      continue;
    Extra &e = this->extras_[n++];
    lv_label_set_text(e.caption, d.cap);
    this->set_value_(e.value, fmt_num(d.s->state, d.decimals), d.unit, pal::text);
  }
}

void LoadScreen::update_rows_() {
  const NodeDetails *x = this->extra_();
  const Terminal *t = this->term_();
  if (x == nullptr)
    return;
  struct Def {
    const char *id;
    const char *label;
  };
  const Def defs[6] = {{x->power_id, "POWER"},
                       {x->switch_id, "SWITCH"},
                       {x->voltage_id, "VOLTAGE"},
                       {x->current_id, "CURRENT"},
                       {x->temperature_id, "TEMPERATURE"},
                       {x->link_id, "LINK"}};
  size_t n = 0;
  for (const Def &d : defs) {
    if (d.id == nullptr)
      continue;
    Row &r = this->rows_[n++];
    lv_label_set_text(r.label, d.label);
    set_text(r.id, r.txt_id, d.id);

    // The reason line says *what* is wrong; this says *which entity* to fix.
    std::string flag;
    uint32_t col = pal::alert;
    if (t != nullptr) {
      const bool is_power = d.id == x->power_id;
      const bool is_switch = d.id == x->switch_id;
      if (is_power && (t->state == EdgeState::DE_ENERGIZED || t->state == EdgeState::NO_DATA))
        flag = "UNAVAILABLE";
      else if (is_power && t->stale)
        flag = "STALE";
      else if (is_switch && t->sw == SwitchState::OFF) {
        flag = "OFF";
        col = pal::text_dim;
      }
    }
    set_text(r.flag, r.txt_flag, flag);
    set_colour(r.flag, r.flag_colour, col);
  }
}

void LoadScreen::update() {
  if (this->pf_ == nullptr || this->root_ == nullptr)
    return;
  const uint8_t d = this->pf_->selected_device(), t = this->pf_->selected_terminal();
  if (t == INVALID_INDEX && d == INVALID_INDEX)
    return;

  const bool changed = d != this->shown_device_ || t != this->shown_terminal_;
  if (changed) {
    this->shown_device_ = d;
    this->shown_terminal_ = t;
    this->sw_pending_ = false;
    this->sw_shown_ = false;
    // A new subject starts at the top, not wherever the last one was left.
    if (this->scroll_ != nullptr)
      lv_obj_scroll_to_y(this->scroll_, 0, LV_ANIM_OFF);
  }
  this->update_header_();
  this->update_state_();
  this->update_switch_();
  this->update_power_();
  this->update_extras_();
  this->update_rows_();
  if (changed)
    this->layout_();
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
