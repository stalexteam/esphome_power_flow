#include "power_flow_render.h"
#include "pf_palette.h"

#ifdef USE_LVGL

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace power_flow {

static const char *const TAG = "power_flow.render";

// ---------------------------------------------------------------------------
// §2 — the palette.
//
// Twenty-eight named colours, transcribed from DEV/UI/POWER_FLOW_UI_SPEC.md and
// named exactly as that document names them. Deliberately constants and not
// YAML keys: there is a document that specifies every colour, and a key per
// colour would be that many ways to contradict it.
//
// Two families carry meaning. **Role** — grid / pv / battery / load — says what
// a wire is. **State** — idle / off / no-data — says what it is doing. Green and
// amber belong to the battery-and-solar axis alone, and there they mean *good*
// and *worth a look*: generation and charging are green, discharge is amber.
// Blue and cyan are role colours with no verdict attached, because there is
// nothing to judge about the mains or a socket.


// ---------------------------------------------------------------------------
// §1, §3, §4 — the grid the whole drawing hangs on. Every one of these is the
// spec's own number: 1x, absolute, relative to the parent object, origin
// top-left. Nothing is derived from the parent's measured size, because the
// spec is written against a 480 x 800 panel and this is that panel.
// ---------------------------------------------------------------------------
static const int16_t SCREEN_W = 480;
static const int16_t SCREEN_H = 800;

static const int16_t COL_L_X = 16, COL_L_W = 108;   ///< OnGrid Load / Other Load
static const int16_t COL_C_X = 140, COL_C_W = 200;  ///< City grid / Invertor
static const int16_t COL_R_X = 356, COL_R_W = 108;  ///< PV / Battery
static const int16_t CONTENT_R = 464;               ///< 480 - 16

static const int16_t ROW1_Y = 16, ROW1_H = 96;
static const int16_t CORE_Y = 200, CORE_H = 150;
static const int16_t CORE_B = 350;  ///< every core edge leaves this abscissa

static const int16_t BUS_X = 239;

/// §4.3 / §4.4. Row *n* sits at 470 + 114n, and its connector geometry is the
/// first row's shifted by the same 114.
static const int16_t ROW_PITCH = 114;
static const int16_t ROW0_CARD_Y = 470;
static const int16_t CONS_W = 192, CONS_H = 52;
static const int16_t CONS_L_X = 16, CONS_R_X = 272;

/// §4.4. The consumer list and its length of bus live in their own object so a
/// configuration with more rows than fit can scroll. It exists unconditionally
/// — one geometry path, and scrolling simply never engages when the content
/// fits — and everything inside it is the absolute y minus SCROLL_Y.
static const int16_t SCROLL_Y = 446;
static const int16_t SCROLL_H = 354;

static const int16_t CARD_RADIUS = 14;
static const int16_t CONS_RADIUS = 12;
static const int16_t BADGE_RADIUS = 8;
static const int16_t BUS_BADGE_RADIUS = 10;
static const int16_t BADGE_PAD = 12;  ///< minimum clear space around a badge's text

static const int16_t HEAD_W = 10, HEAD_H = 8;
static const int16_t CROSS_D = 48;  ///< the disc that breaks the wire

/// Inside the inverter card, in *content* coordinates — the card has a 1 px
/// border, so a child at (18, 16) is at (159, 217) on the screen. The two metric
/// columns are pinned rather than laid out by content width: LOSS changes every
/// few seconds and a flex row would walk EFF across the card.
static const int16_t INV_PAD_X = 18, INV_PAD_Y = 16;
static const int16_t INV_RULE_Y = 52;   ///< 253 absolute
static const int16_t INV_CAP_Y = 66;    ///< 267
static const int16_t INV_VAL_Y = 80;    ///< 281
static const int16_t INV_EFF_X = 106;   ///< 247, i.e. 26 px past the LOSS column
static const int16_t INV_OFF_Y = 74;    ///< 275, where OFFLINE replaces both figures
static const int16_t INV_UNREL_Y = 116; ///< 317, the spare band under the metrics

/// §7. Dots run at ~25 fps along one straight run. Speed is proportional to the
/// 60 s time-weighted power, compressed with a square root: linear scaling puts
/// every domestic load in the bottom tenth of the range, so 50 W and 500 W would
/// look identical. These reproduce the mockups' own timings to within a tenth of
/// a second — the grid dot crosses its 66 px in about 1.5 s at half a kilowatt.
static const uint32_t FRAME_MS = 40;
static const float DOT_SPEED_MIN = 18.0f;
static const float DOT_SPEED_MAX = 80.0f;
static const float DOT_SPEED_REF = 2000.0f;

/// §10 forbids a status bar, so nothing on this screen announces that things
/// are fine. The banner, the UNRELIABLE caption and the grid verdict appear only
/// when something is not, and all three are INVENTED — the spec draws no mockup
/// for loss of Home Assistant, for an implausible balance or for a distrusted
/// grid. Each is marked at its definition so the owner knows what is not design.
static const lv_opa_t OPA_NO_HA = 110;

/// §9. One threshold, so the eye never compares `900 W` against `1.1 kW`.
static const float KW_FROM = 500.0f;

// ---------------------------------------------------------------------------
// Icons (§8). Every glyph the diagram uses comes from the config —
// `Device::icon` and `Terminal::icon` are UTF-8 MDI glyphs set in the YAML.
// These are only the fallbacks for an entry that left the key empty, and they
// are the same codepoints §8 names, read out of scss/_variables.scss at the
// pinned tag v7.4.47. Each is probed with the font's own glyph lookup before
// use: an unlisted codepoint renders as nothing, silently, and a silent blank
// is worse than an empty slot.
// ---------------------------------------------------------------------------
static const uint32_t MDI_FLASH = 0x0F0241;         ///< City grid
static const uint32_t MDI_SINE_WAVE = 0x0F095B;     ///< Invertor
static const uint32_t MDI_SUNNY = 0x0F05A8;         ///< PV
static const uint32_t MDI_PLUG_OUTLINE = 0x0F1425;  ///< OnGrid Load, and any consumer
static const uint32_t MDI_DOTS = 0x0F01D8;          ///< Other Load
static const uint32_t MDI_CLOSE_THICK = 0x0F1398;   ///< the lost-contact marker

/// UTF-8 for one codepoint. MDI lives in a private use plane, so these are all
/// four bytes; the shorter branches exist so the helper is not quietly wrong if
/// it is ever reused.
static std::string encode_utf8(uint32_t cp) {
  char out[5];
  int n = 0;
  if (cp < 0x80) {
    out[n++] = (char) cp;
  } else if (cp < 0x800) {
    out[n++] = (char) (0xC0 | (cp >> 6));
    out[n++] = (char) (0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[n++] = (char) (0xE0 | (cp >> 12));
    out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[n++] = (char) (0x80 | (cp & 0x3F));
  } else {
    out[n++] = (char) (0xF0 | (cp >> 18));
    out[n++] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[n++] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[n++] = (char) (0x80 | (cp & 0x3F));
  }
  out[n] = 0;
  return std::string(out);
}

/// The first codepoint of a UTF-8 string, so a configured glyph can be probed.
static uint32_t decode_utf8(const std::string &s) {
  if (s.empty())
    return 0;
  const uint8_t *p = (const uint8_t *) s.data();
  const size_t n = s.size();
  if (p[0] < 0x80)
    return p[0];
  if ((p[0] & 0xE0) == 0xC0 && n >= 2)
    return ((uint32_t) (p[0] & 0x1F) << 6) | (uint32_t) (p[1] & 0x3F);
  if ((p[0] & 0xF0) == 0xE0 && n >= 3)
    return ((uint32_t) (p[0] & 0x0F) << 12) | ((uint32_t) (p[1] & 0x3F) << 6) |
           (uint32_t) (p[2] & 0x3F);
  if ((p[0] & 0xF8) == 0xF0 && n >= 4)
    return ((uint32_t) (p[0] & 0x07) << 18) | ((uint32_t) (p[1] & 0x3F) << 12) |
           ((uint32_t) (p[2] & 0x3F) << 6) | (uint32_t) (p[3] & 0x3F);
  return 0;
}

// ---------------------------------------------------------------------------
// §9 — number formatting. Every NaN is a dash, never a rough number.
// U+2014 EM DASH is in GF_Latin_Core and is listed explicitly in f_pf_metric's
// glyph set, so it is safe in every face the diagram uses. U+00D7 MULTIPLICATION
// SIGN is in GF_Latin_Core, and it is what draws the ✕.
// ---------------------------------------------------------------------------
static const char *const DASH = "\xE2\x80\x94";  // U+2014
static const char *const CROSS = "\xC3\x97";     // U+00D7

static std::string fmt_power(float w) {
  if (!is_valid(w))
    return DASH;
  char buf[24];
  if (std::fabs(w) >= KW_FROM)
    snprintf(buf, sizeof(buf), "%.2f kW", w / 1000.0f);
  else
    snprintf(buf, sizeof(buf), "%.0f W", w);
  return buf;
}

/// The battery leg, where the sign is the direction (the BMS convention:
/// positive is charging).
static std::string fmt_signed_power(float w) {
  if (!is_valid(w))
    return DASH;
  char buf[24];
  if (std::fabs(w) >= KW_FROM)
    snprintf(buf, sizeof(buf), "%+.2f kW", w / 1000.0f);
  else
    snprintf(buf, sizeof(buf), "%+.0f W", w);
  return buf;
}

/// Splits a formatted power into the part `m28` can draw — digits, sign,
/// decimal point — and the part it cannot: `W`, `kW`. f_pf_metric is built with
/// a digits-only glyph list, so this split is load-bearing, not cosmetic.
static void split_number(const std::string &s, std::string &num, std::string &unit) {
  const size_t i = s.find(' ');
  if (i == std::string::npos) {
    num = s;
    unit.clear();
  } else {
    num = s.substr(0, i);
    unit = s.substr(i);
  }
}

// ---------------------------------------------------------------------------
// Small LVGL helpers
// ---------------------------------------------------------------------------

static void set_text(lv_obj_t *obj, std::string &cache, const std::string &txt) {
  if (obj == nullptr || cache == txt)
    return;
  cache = txt;
  lv_label_set_text(obj, txt.c_str());
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
  if (obj == nullptr)
    return;
  if (hidden)
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_color(lv_obj_t *obj, uint32_t &cache, uint32_t color) {
  if (obj == nullptr || cache == color)
    return;
  cache = color;
  lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
}

static int text_width(const lv_font_t *font, const char *txt) {
  if (font == nullptr || txt == nullptr || txt[0] == 0)
    return 0;
  lv_point_t sz;
  lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return (int) sz.x;
}

static int line_h(const lv_font_t *font, int fallback) {
  return font != nullptr ? (int) lv_font_get_line_height(font) : fallback;
}

/// Distance from the top of a line box down to the baseline. Two labels in
/// different sizes only read as one number when this is matched — which is what
/// the big figure and its small unit are.
static int baseline_of(const lv_font_t *font, int fallback) {
  if (font == nullptr)
    return fallback;
  return (int) lv_font_get_line_height(font) - (int) font->base_line;
}

// ---------------------------------------------------------------------------
// Construction primitives
// ---------------------------------------------------------------------------

bool FlowRenderer::has_glyph_(const lv_font_t *font, uint32_t cp) const {
  if (font == nullptr || font->get_glyph_dsc == nullptr || cp == 0)
    return false;
  lv_font_glyph_dsc_t dsc;
  // Deliberately the font's own callback and not lv_font_get_glyph_dsc(), which
  // falls back to a placeholder and would report success for a missing glyph.
  return font->get_glyph_dsc(font, &dsc, cp, 0);
}

std::string FlowRenderer::icon_text_(const std::string &configured, uint32_t fallback) const {
  const lv_font_t *f = this->pf_->style().icon;
  const uint32_t cp = decode_utf8(configured);
  if (this->has_glyph_(f, cp))
    return configured;
  if (cp != 0)
    ESP_LOGW(TAG, "configured icon U+%05X is not in f_icons — falling back", (unsigned) cp);
  if (this->has_glyph_(f, fallback))
    return encode_utf8(fallback);
  ESP_LOGW(TAG, "icon U+%05X missing from f_icons — that slot stays empty", (unsigned) fallback);
  return std::string();
}

/// A bare rectangle: no theme fill, no border, no padding, no scrolling. Every
/// object in this renderer starts life here, so nothing inherits a surprise from
/// the default theme.
lv_obj_t *FlowRenderer::plain_(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  return o;
}

/// One 2 px run of a connector, or the rule inside the inverter card. §5 gives
/// these as plain rectangles and they never move, so that is what they are.
lv_obj_t *FlowRenderer::rect_(lv_obj_t *parent, const Rect &r, uint32_t color) {
  lv_obj_t *o = this->plain_(parent);
  lv_obj_set_pos(o, r.x, r.y);
  lv_obj_set_size(o, r.w, r.h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  return o;
}

/// `w < 0` means size to the content, which is what the two-label metric pairs
/// need so their halves can be butted together on a shared baseline.
lv_obj_t *FlowRenderer::label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                               lv_text_align_t align, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
  if (font != nullptr)
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  // Labels inside a card must set text_color explicitly or they inherit the
  // dark theme colour and vanish (§8).
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_opa(l, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
  if (w < 0)
    lv_obj_set_width(l, LV_SIZE_CONTENT);
  else
    lv_obj_set_width(l, w);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, "");
  return l;
}

/// A node box: radius, a 1 px border, `card` on `border`, and clickable. §11 —
/// tapping a node fires the component's per-device trigger and the YAML decides
/// what opens; badges and connectors are not touch targets.
lv_obj_t *FlowRenderer::card_(int x, int y, int w, int h, int radius) {
  lv_obj_t *o = this->plain_(this->root_);
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

uint8_t FlowRenderer::add_node_(Node &&n) {
  lv_obj_add_flag(n.box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(n.box, (void *) (uintptr_t) this->nodes_.size());
  lv_obj_add_event_cb(n.box, FlowRenderer::node_event_cb_, LV_EVENT_CLICKED, this);
  this->nodes_.push_back(std::move(n));
  return (uint8_t) (this->nodes_.size() - 1);
}

// ---------------------------------------------------------------------------
// §4.1 — source cards. Three lines, centred: icon, gap 7, name, gap 7, sub.
// Child coordinates are relative to the card's content box, which the 1 px
// border insets by one pixel on every side.
// ---------------------------------------------------------------------------
void FlowRenderer::build_source_(Node &n, int x, int w, uint32_t icon_col,
                                 const lv_font_t *name_font, const lv_font_t *sub_font) {
  const PowerFlowStyle &s = this->pf_->style();
  n.box = this->card_(x, ROW1_Y, w, ROW1_H, CARD_RADIUS);
  if (n.slot == Slot::PV)
    lv_obj_set_style_border_color(n.box, lv_color_hex(pal::pv_border), LV_PART_MAIN);

  const Device *d = this->dev_(n.device);
  const Terminal *t = this->term_(n.terminal);
  uint32_t fallback = MDI_FLASH;
  if (n.slot == Slot::PV)
    fallback = MDI_SUNNY;
  else if (n.slot == Slot::TAP)
    fallback = MDI_PLUG_OUTLINE;
  std::string configured;
  if (n.slot == Slot::GRID)
    configured = (d != nullptr) ? d->icon : std::string();
  else if (t != nullptr && !t->icon.empty())
    configured = t->icon;
  else if (d != nullptr)
    configured = d->icon;
  const std::string glyph = this->icon_text_(configured, fallback);

  const int cw = w - 2, ch = ROW1_H - 2;
  const int ih = glyph.empty() ? 0 : line_h(s.icon, 26);
  const int gap = glyph.empty() ? 0 : 7;
  const int nh = line_h(name_font, 24);
  const int sh = line_h(sub_font, 18);
  int y = (ch - (ih + gap + nh + 7 + sh)) / 2;
  if (y < 1)
    y = 1;

  if (!glyph.empty()) {
    n.icon = this->label_(n.box, s.icon, 0, y, cw, LV_TEXT_ALIGN_CENTER, icon_col);
    lv_label_set_text(n.icon, glyph.c_str());
    y += ih + 7;
  }
  n.name = this->label_(n.box, name_font, 0, y, cw, LV_TEXT_ALIGN_CENTER, pal::text);
  y += nh + 7;
  n.sub = this->label_(n.box, sub_font, 0, y, cw, LV_TEXT_ALIGN_CENTER, pal::text_dim);
}

// ---------------------------------------------------------------------------
// §4.2 — Other Load. The bus remainder: everything in the apartment that no
// meter sees. Deliberately *not* the inverter's own draw, which is already on
// screen as LOSS — printing that twice would leave the unmetered load invisible.
// §10: there is no second "Others" row among the consumers.
// ---------------------------------------------------------------------------
void FlowRenderer::build_other_(Node &n) {
  const PowerFlowStyle &s = this->pf_->style();
  n.box = this->card_(COL_L_X, CORE_Y, COL_L_W, CORE_H, CARD_RADIUS);

  const Terminal *t = this->term_(n.terminal);
  const std::string glyph = this->icon_text_(t != nullptr ? t->icon : std::string(), MDI_DOTS);

  const int cw = COL_L_W - 2, ch = CORE_H - 2;
  const int ih = glyph.empty() ? 0 : line_h(s.icon, 26);
  const int gap = glyph.empty() ? 0 : 8;
  const int nh = line_h(s.s18, 22);
  const int sh = line_h(s.s14, 18);
  int y = (ch - (ih + gap + nh + 8 + sh)) / 2;

  if (!glyph.empty()) {
    n.icon = this->label_(n.box, s.icon, 0, y, cw, LV_TEXT_ALIGN_CENTER, pal::load);
    lv_label_set_text(n.icon, glyph.c_str());
    y += ih + 8;
  }
  n.name = this->label_(n.box, s.s18, 0, y, cw, LV_TEXT_ALIGN_CENTER, pal::text);
  y += nh + 8;
  // "Load" is the design's own second line, not a caption the config supplies:
  // §4.2 names this node `Other Load` and draws the word in `text_dim` under
  // whatever the owner called the remainder.
  n.sub = this->label_(n.box, s.s14, 0, y, cw, LV_TEXT_ALIGN_CENTER, pal::text_dim);
  lv_label_set_text(n.sub, "Load");
}

// ---------------------------------------------------------------------------
// §4.2 — the inverter. Name row, a 1 px rule, then two metric columns.
// ---------------------------------------------------------------------------
void FlowRenderer::build_inverter_(Node &n) {
  const PowerFlowStyle &s = this->pf_->style();
  n.box = this->card_(COL_C_X, CORE_Y, COL_C_W, CORE_H, CARD_RADIUS);

  const Device *d = this->dev_(n.device);
  const std::string glyph = this->icon_text_(d != nullptr ? d->icon : std::string(), MDI_SINE_WAVE);

  const int cw = COL_C_W - 2 - 2 * INV_PAD_X;

  // The rule is pinned at 253 absolute and the name row is centred in what is
  // left above it, so a 26 px icon face cannot push the metrics down the card.
  const int row_h = INV_RULE_Y - INV_PAD_Y - 15;
  int tx = INV_PAD_X;
  if (!glyph.empty()) {
    const int ih = line_h(s.icon, 26);
    const int iw = text_width(s.icon, glyph.c_str());
    n.icon = this->label_(n.box, s.icon, INV_PAD_X, INV_PAD_Y + (row_h - ih) / 2, iw + 2,
                          LV_TEXT_ALIGN_LEFT, pal::load);
    lv_label_set_text(n.icon, glyph.c_str());
    tx = INV_PAD_X + iw + 10;
  }
  const int nh = line_h(s.s22, 27);
  n.name = this->label_(n.box, s.s22, tx, INV_PAD_Y + (row_h - nh) / 2,
                        COL_C_W - 2 - INV_PAD_X - tx, LV_TEXT_ALIGN_LEFT, pal::text);
  lv_label_set_long_mode(n.name, LV_LABEL_LONG_MODE_DOTS);

  n.rule = this->rect_(n.box, {INV_PAD_X, INV_RULE_Y, (int16_t) cw, 1}, pal::divider);

  n.cap_loss = this->label_(n.box, s.s10, INV_PAD_X, INV_CAP_Y, -1, LV_TEXT_ALIGN_LEFT,
                            pal::text_dim);
  n.cap_eff = this->label_(n.box, s.s10, INV_EFF_X, INV_CAP_Y, -1, LV_TEXT_ALIGN_LEFT,
                           pal::text_dim);
  lv_obj_set_style_text_letter_space(n.cap_loss, 1, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(n.cap_eff, 1, LV_PART_MAIN);
  lv_label_set_text(n.cap_loss, "LOSS");
  lv_label_set_text(n.cap_eff, "EFF");

  n.val_loss = this->label_(n.box, s.m28, INV_PAD_X, INV_VAL_Y, -1, LV_TEXT_ALIGN_LEFT,
                            pal::text);
  n.unit_loss = this->label_(n.box, s.m12, INV_PAD_X, INV_VAL_Y, -1, LV_TEXT_ALIGN_LEFT,
                             pal::text_dim);
  n.val_eff = this->label_(n.box, s.m28, INV_EFF_X, INV_VAL_Y, -1, LV_TEXT_ALIGN_LEFT,
                           pal::text);
  n.unit_eff = this->label_(n.box, s.m12, INV_EFF_X, INV_VAL_Y, -1, LV_TEXT_ALIGN_LEFT,
                            pal::text_dim);

  // §6 — the spine prints OFFLINE in words rather than only going pale.
  n.offline = this->label_(n.box, s.s18, INV_PAD_X, INV_OFF_Y, cw, LV_TEXT_ALIGN_LEFT,
                           pal::text_off);
  lv_label_set_text(n.offline, "OFFLINE");
  set_hidden(n.offline, true);

  // INVENTED — there is no mockup for an implausible balance. A caption in the
  // same treatment as LOSS / EFF, in `alert`, in the band the card already has
  // spare, plus an `alert` border. It says nothing at all when the balance is
  // sound, which is the whole reason the old bottom strip was deleted.
  n.unreliable = this->label_(n.box, s.s10, INV_PAD_X, INV_UNREL_Y, cw,
                              LV_TEXT_ALIGN_LEFT, pal::alert);
  lv_obj_set_style_text_letter_space(n.unreliable, 1, LV_PART_MAIN);
  lv_label_set_text(n.unreliable, "UNRELIABLE");
  set_hidden(n.unreliable, true);
}

// ---------------------------------------------------------------------------
// §4.2 — the battery. SOC is a level, not a flow: never averaged, never dimmed
// by the flow logic, and the one ring on a diagram of lines.
// ---------------------------------------------------------------------------
void FlowRenderer::build_battery_(Node &n) {
  const PowerFlowStyle &s = this->pf_->style();
  n.box = this->card_(COL_R_X, CORE_Y, COL_R_W, CORE_H, CARD_RADIUS);

  // The spec's 86 x 86 ring box sits at (367, 244), so its centre is (410, 287).
  // In content coordinates, with the 1 px border, that is (53, 86).
  const int cw = COL_R_W - 2;
  const int rcx = 410 - (COL_R_X + 1), rcy = 287 - (CORE_Y + 1);
  const int nh = line_h(s.s16, 20);
  n.name = this->label_(n.box, s.s16, 0, (244 - (CORE_Y + 1)) - 8 - nh, cw,
                        LV_TEXT_ALIGN_CENTER, pal::text);

  // radius 37, stroke 6.5. LVGL centres its stroke on (size - width) / 2, so an
  // 82 px box with a 7 px arc gives r = 37.5; even numbers keep the centre on a
  // whole pixel. Declared out here because the SOC labels centre on it whether
  // or not this build has the arc widget.
  const int rd = 82, aw = 7;
#if LV_USE_ARC
  n.ring = lv_arc_create(n.box);
  lv_obj_remove_style(n.ring, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(n.ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(n.ring, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(n.ring, rcx - rd / 2, rcy - rd / 2);
  lv_obj_set_size(n.ring, rd, rd);
  lv_obj_set_style_pad_all(n.ring, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(n.ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_arc_set_mode(n.ring, LV_ARC_MODE_NORMAL);
  lv_arc_set_rotation(n.ring, 270);  // start angle -90 degrees, clockwise
  lv_arc_set_bg_angles(n.ring, 0, 360);
  lv_arc_set_range(n.ring, 0, 100);
  lv_arc_set_value(n.ring, 0);
  lv_obj_set_style_arc_width(n.ring, aw, LV_PART_MAIN);
  lv_obj_set_style_arc_width(n.ring, aw, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(n.ring, lv_color_hex(pal::divider), LV_PART_MAIN);
  lv_obj_set_style_arc_color(n.ring, lv_color_hex(pal::batt), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(n.ring, true, LV_PART_INDICATOR);
#endif

  // Two lines, not one: the number above, the unit smaller beneath it, the pair
  // centred in the ring both ways. A `%` set beside the digits pushes the whole
  // group off the ring's axis, and the ring is the one thing on this card the
  // eye centres on. Both labels span the ring and centre their own text, so the
  // group stays put as 9 % becomes 100 %.
  const int mh = line_h(s.m28, 34);
  const int uh = line_h(s.m14, 17);
  const int gap = -5;  // metric digits carry a descender's worth of empty space
  const int top = rcy - (mh + gap + uh) / 2;
  n.soc_num = this->label_(n.box, s.m28, rcx - rd / 2, top, rd, LV_TEXT_ALIGN_CENTER,
                           pal::text);
  n.soc_pct = this->label_(n.box, s.m14, rcx - rd / 2, top + mh + gap, rd,
                           LV_TEXT_ALIGN_CENTER, pal::text_dim);
  lv_label_set_text(n.soc_pct, "%");

  n.offline = this->label_(n.box, s.s16, 0, rcy - nh / 2, cw, LV_TEXT_ALIGN_CENTER,
                           pal::text_off);
  lv_label_set_text(n.offline, "OFFLINE");
  set_hidden(n.offline, true);
}

// ---------------------------------------------------------------------------
// §4.3 — a consumer. One line: icon, gap 9, name, padding-left 12. Built at
// row 0 and translated afterwards; `movable` carries everything whose y is a
// function of the row, so a sunk entry takes its whole connector with it.
// ---------------------------------------------------------------------------
void FlowRenderer::build_consumer_(Node &n, bool left, int row) {
  const PowerFlowStyle &s = this->pf_->style();
  const int x = left ? CONS_L_X : CONS_R_X;
  const int y0 = ROW0_CARD_Y - SCROLL_Y;  // row 0, in container coordinates

  n.box = this->plain_(this->scroll_);
  lv_obj_set_pos(n.box, x, y0);
  lv_obj_set_size(n.box, CONS_W, CONS_H);
  lv_obj_set_style_radius(n.box, CONS_RADIUS, LV_PART_MAIN);
  lv_obj_set_style_bg_color(n.box, lv_color_hex(pal::card), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(n.box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(n.box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(n.box, lv_color_hex(pal::border), LV_PART_MAIN);
  lv_obj_set_style_border_opa(n.box, LV_OPA_COVER, LV_PART_MAIN);
  n.movable.push_back({n.box, (int16_t) y0});

  const Terminal *t = this->term_(n.terminal);
  const std::string glyph =
      this->icon_text_(t != nullptr ? t->icon : std::string(), MDI_PLUG_OUTLINE);

  const int ch = CONS_H - 2;
  int tx = 12;
  if (!glyph.empty()) {
    const int ih = line_h(s.icon, 26);
    const int iw = text_width(s.icon, glyph.c_str());
    n.icon = this->label_(n.box, s.icon, 12, (ch - ih) / 2, iw + 2, LV_TEXT_ALIGN_LEFT,
                          pal::load);
    lv_label_set_text(n.icon, glyph.c_str());
    tx = 12 + iw + 9;
  }
  const int nh = line_h(s.s16, 20);
  n.name = this->label_(n.box, s.s16, tx, (ch - nh) / 2, CONS_W - 2 - tx - 12,
                        LV_TEXT_ALIGN_LEFT, pal::text);
  lv_label_set_long_mode(n.name, LV_LABEL_LONG_MODE_DOTS);
  // `row` is the row this consumer *wants*; its objects are nevertheless built
  // at row 0 and translated by resort_consumers_(). Leaving n.row at -1 is what
  // makes that first translation happen — claiming the row here would tell the
  // sorter the card is already in place while the pixels sat in a pile.
  (void) row;
}

// ---------------------------------------------------------------------------
// §5 — connectors.
// ---------------------------------------------------------------------------

/// A 10 x 8 triangle as four 2 px rows, widest at the base. A polygon would need
/// the canvas this build does not have.
void FlowRenderer::build_head_(Edge &e, lv_obj_t *parent) {
  for (int k = 0; k < HEAD_STEPS; k++) {
    e.head[k] = this->plain_(parent);
    lv_obj_set_style_bg_opa(e.head[k], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(e.head[k], LV_OBJ_FLAG_HIDDEN);
  }
}

/// Step widths run base-first. `down` puts the base at the top and the point at
/// the bottom; otherwise the point is at the top. A consumer's head carries its
/// row's 114 px offset like everything else that belongs to that row.
void FlowRenderer::place_head_(Edge &e, bool rev) {
  static const int16_t W[HEAD_STEPS] = {10, 7, 5, 2};
  const Rect &b = rev ? e.head_rev : e.head_fwd;
  const bool down = rev ? e.down_rev : e.down_fwd;
  const int off = ROW_PITCH * e.row;
  for (int k = 0; k < HEAD_STEPS; k++) {
    if (e.head[k] == nullptr)
      continue;
    const int w = W[k];
    const int y = down ? b.y + k * 2 : b.y + HEAD_H - 2 - k * 2;
    lv_obj_set_pos(e.head[k], b.x + (HEAD_W - w) / 2, y + off);
    lv_obj_set_size(e.head[k], w, 2);
  }
}

void FlowRenderer::build_badge_(Edge &e, lv_obj_t *parent, int radius, const lv_font_t *font) {
  e.badge_font = font;
  e.badge = this->plain_(parent);
  lv_obj_set_style_radius(e.badge, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(e.badge, lv_color_hex(pal::bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(e.badge, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(e.badge, 1, LV_PART_MAIN);
  lv_obj_set_style_border_opa(e.badge, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_pos(e.badge, e.bcx - e.bw / 2, e.bcy - e.bh / 2);
  lv_obj_set_size(e.badge, e.bw, e.bh);
  lv_obj_add_flag(e.badge, LV_OBJ_FLAG_HIDDEN);
  e.badge_lbl = this->label_(e.badge, font, 0, (e.bh - 2 - line_h(font, 20)) / 2, e.bw - 2,
                             LV_TEXT_ALIGN_CENTER, pal::text);
}

/// §6 — a 48 px disc in `bg` with an `alert` cross on it, centred on the middle
/// of the edge's longest run, which in every case in the spec is also where the
/// badge sits. The disc hides the line, so the mark reads as a break in the wire
/// rather than something drawn on top of one.
void FlowRenderer::build_cross_(Edge &e, lv_obj_t *parent, int cx, int cy) {
  const PowerFlowStyle &s = this->pf_->style();
  e.cross = this->plain_(parent);
  lv_obj_set_pos(e.cross, cx - CROSS_D / 2, cy - CROSS_D / 2);
  lv_obj_set_size(e.cross, CROSS_D, CROSS_D);
  lv_obj_set_style_radius(e.cross, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(e.cross, lv_color_hex(pal::bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(e.cross, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(e.cross, LV_OBJ_FLAG_HIDDEN);
  // MDI `close-thick` rather than the × of a text face: a typographic multiply
  // sign fills barely half its em, so at any size it draws a mark half as big as
  // the space it occupies. An icon glyph fills its cell, which is what makes the
  // marker read at arm's length from a wall.
  const lv_font_t *f = s.icon != nullptr ? s.icon : s.s22;
  const std::string mark = this->has_glyph_(s.icon, MDI_CLOSE_THICK)
                               ? encode_utf8(MDI_CLOSE_THICK)
                               : std::string(CROSS);
  if (mark == CROSS)
    f = s.s22;
  const int lh = line_h(f, 27);
  lv_obj_t *x = this->label_(e.cross, f, 0, (CROSS_D - lh) / 2, CROSS_D,
                             LV_TEXT_ALIGN_CENTER, pal::alert);
  lv_label_set_text(x, mark.c_str());
}

void FlowRenderer::add_edge_(Edge &&e) { this->edges_.push_back(std::move(e)); }

// ---------------------------------------------------------------------------

void FlowRenderer::setup(PowerFlow *pf) {
  this->pf_ = pf;
  lv_obj_t *parent = pf->parent();
  if (parent == nullptr) {
    ESP_LOGE(TAG, "no parent object — nothing drawn");
    return;
  }
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(parent);
  this->build_(parent);
  this->update();
}

void FlowRenderer::build_(lv_obj_t *parent) {
  const PowerFlowStyle &s = this->pf_->style();
  const auto &devs = this->pf_->devices();
  const auto &terms = this->pf_->terminals();

  this->root_ = this->plain_(parent);
  lv_obj_set_pos(this->root_, 0, 0);
  lv_obj_set_size(this->root_, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(this->root_, lv_color_hex(pal::bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->root_, LV_OPA_COVER, LV_PART_MAIN);

  // --- who exists ----------------------------------------------------------
  //
  // Presence is derived from the graph, never hardcoded, and §3's configuration
  // is a consequence of that rather than a variant to be matched by name.
  uint8_t d_grid = INVALID_INDEX, d_inv = INVALID_INDEX, d_bat = INVALID_INDEX;
  uint8_t d_pv = INVALID_INDEX;
  for (uint8_t i = 0; i < devs.size(); i++) {
    switch (devs[i].kind) {
      case DeviceKind::GRID: if (d_grid == INVALID_INDEX) d_grid = i; break;
      case DeviceKind::INVERTER: if (d_inv == INVALID_INDEX) d_inv = i; break;
      case DeviceKind::BATTERY: if (d_bat == INVALID_INDEX) d_bat = i; break;
      case DeviceKind::PV: if (d_pv == INVALID_INDEX) d_pv = i; break;
      default: break;
    }
  }
  this->d_grid_ = d_grid;
  this->d_inv_ = d_inv;
  this->d_bat_ = d_bat;

  uint8_t t_in = INVALID_INDEX, t_out = INVALID_INDEX, t_bat = INVALID_INDEX, t_pv = INVALID_INDEX;
  if (d_inv != INVALID_INDEX) {
    for (uint8_t ti : devs[d_inv].terminals) {
      if (!terms[ti].enabled)
        continue;  // `enabled: false` hides the node entirely — pv, today
      switch (terms[ti].role) {
        case TerminalRole::INPUT: t_in = ti; break;
        case TerminalRole::OUTPUT: t_out = ti; break;
        case TerminalRole::BATTERY: t_bat = ti; break;
        case TerminalRole::PV: t_pv = ti; break;
        default: break;
      }
    }
  }
  this->t_in_ = t_in;
  this->t_out_ = t_out;
  this->t_bat_ = t_bat;

  // OnGrid Load is a TAP terminal on the grid device. Not configured today — it
  // needs the ATS dry contact — so it is simply not drawn.
  uint8_t t_tap = INVALID_INDEX;
  for (uint8_t i = 0; i < terms.size() && t_tap == INVALID_INDEX; i++)
    if (terms[i].enabled && terms[i].role == TerminalRole::TAP)
      t_tap = i;

  // Whatever the bus is fed from is what the trunk carries.
  uint8_t t_bus = t_out;
  for (const Device &d : devs)
    if (d.kind == DeviceKind::BUS && d.source_terminal != INVALID_INDEX)
      t_bus = d.source_terminal;

  // Consumers are the terminals belonging to no device, in declaration order.
  // The one declared `power: auto` among them is the bus remainder — the
  // unmetered apartment load — and it goes in the core row, never in a consumer
  // row.
  std::vector<uint8_t> cons;
  uint8_t t_other = INVALID_INDEX;
  for (uint8_t i = 0; i < terms.size(); i++) {
    const Terminal &t = terms[i];
    if (t.device != INVALID_INDEX || !t.enabled)
      continue;
    if (t.is_auto && t_other == INVALID_INDEX) {
      t_other = i;
      continue;
    }
    cons.push_back(i);
  }

  const bool has_grid = d_grid != INVALID_INDEX;
  const bool has_tap = t_tap != INVALID_INDEX;
  const bool has_pv = t_pv != INVALID_INDEX || d_pv != INVALID_INDEX;

  // --- §3, row 1 -----------------------------------------------------------
  //
  // Four cases, and none of them is matched by name: the grid card takes
  // whatever its absent neighbours leave behind. A = 140..340, B = 140..464,
  // D = 16..340, C = 16..464. The exit abscissas 169 and 239 never move, so
  // nothing outside the card itself depends on its width.
  const int grid_x = has_tap ? COL_C_X : COL_L_X;
  const int grid_w = (has_pv ? (COL_R_X - 16) : CONTENT_R) - grid_x;

  if (has_tap) {
    Node n;
    n.slot = Slot::TAP;
    n.device = d_grid;
    n.terminal = t_tap;
    this->build_source_(n, COL_L_X, COL_L_W, pal::tap_val, s.s18, s.s14);
    lv_label_set_text(n.sub, "Load");
    this->add_node_(std::move(n));
  }
  if (has_grid) {
    Node n;
    n.slot = Slot::GRID;
    n.device = d_grid;
    n.terminal = t_in;
    this->build_source_(n, grid_x, grid_w, pal::grid, s.s22, s.m14);
    this->n_grid_ = this->add_node_(std::move(n));
  }
  if (has_pv) {
    Node n;
    n.slot = Slot::PV;
    n.device = d_pv;
    n.terminal = t_pv;
    this->build_source_(n, COL_R_X, COL_R_W, pal::pv, s.s20, s.m14);
    this->add_node_(std::move(n));
  }

  // --- §4.2, the core row --------------------------------------------------
  if (t_other != INVALID_INDEX) {
    Node n;
    n.slot = Slot::OTHER;
    n.terminal = t_other;
    this->build_other_(n);
    this->add_node_(std::move(n));
  }
  if (d_inv != INVALID_INDEX) {
    Node n;
    n.slot = Slot::INVERTER;
    n.device = d_inv;
    this->build_inverter_(n);
    this->n_inverter_ = this->add_node_(std::move(n));
  }
  if (d_bat != INVALID_INDEX) {
    Node n;
    n.slot = Slot::BATTERY;
    n.device = d_bat;
    n.terminal = t_bat;
    this->build_battery_(n);
    this->add_node_(std::move(n));
  }

  // --- §5.1, row 1 into the core -------------------------------------------
  if (has_grid && t_in != INVALID_INDEX) {
    Edge e;
    e.terminal = t_in;
    e.kind = Kind::GRID;
    e.line_col = pal::grid_line;
    e.val_col = pal::grid_val;
    e.seg_rect = {{BUS_X, 112, 2, 80}};
    e.head_fwd = {235, 190, HEAD_W, HEAD_H};
    e.down_fwd = true;
    e.bcx = 240;
    e.bcy = 152;
    // C — the card runs the full width — gets 96; A, B and D get 88.
    e.bw = (grid_x == COL_L_X && !has_pv) ? 96 : 88;
    e.bw_idle = e.bw;
    e.bh = 28;
    e.n_dots = 1;
    e.dot_size = 6;
    e.dot_x = 237;
    e.dot_y0 = 118;
    e.dot_len = 66;
    e.dot_dir = 1;
    this->add_edge_(std::move(e));
  }
  if (has_tap) {
    Edge e;
    e.terminal = t_tap;
    e.kind = Kind::TAP;
    e.line_col = pal::tap_line;
    e.val_col = pal::tap_val;
    e.seg_rect = {{169, 112, 2, 41}, {42, 151, 129, 2}, {42, 120, 2, 33}};
    e.head_fwd = {38, 112, HEAD_W, HEAD_H};
    e.down_fwd = false;
    e.bcx = 106;
    e.bcy = 152;
    e.bw = 84;
    e.bw_idle = 84;
    e.bh = 28;
    // The run that carries the head is the one that animates, which reproduces
    // both dots the mockups draw and gives this edge — which they do not draw
    // one for — the same treatment. Here the flow is upward, into OnGrid Load.
    e.n_dots = 1;
    e.dot_size = 6;
    e.dot_x = 40;
    e.dot_y0 = 126;
    e.dot_len = 19;
    e.dot_dir = -1;
    this->add_edge_(std::move(e));
  }
  if (t_pv != INVALID_INDEX) {
    // §2, amended: the PV edge is green, not amber. Generation is the good case
    // and reads like charging does; `pv` survives for the sun icon, which
    // identifies a device rather than reporting a state.
    Edge e;
    e.terminal = t_pv;
    e.kind = Kind::PV;
    e.line_col = pal::batt_line;
    e.val_col = pal::batt;
    e.seg_rect = {{436, 112, 2, 42}, {311, 152, 127, 2}, {311, 152, 2, 40}};
    e.head_fwd = {307, 190, HEAD_W, HEAD_H};
    e.down_fwd = true;
    e.bcx = 374;
    e.bcy = 152;
    e.bw = 68;
    e.bw_idle = 68;
    e.bh = 28;
    e.n_dots = 1;
    e.dot_size = 6;
    e.dot_x = 309;
    e.dot_y0 = 158;
    e.dot_len = 26;
    e.dot_dir = 1;
    this->add_edge_(std::move(e));
  }

  // --- §5.2, the core into row 3 -------------------------------------------
  if (t_other != INVALID_INDEX) {
    Edge e;
    e.terminal = t_other;
    e.kind = Kind::OTHER;
    e.line_col = pal::load_line;
    e.val_col = pal::load;
    e.seg_rect = {{169, CORE_B, 2, 40}, {42, 388, 129, 2}, {42, 358, 2, 32}};
    e.head_fwd = {38, CORE_B, HEAD_W, HEAD_H};
    e.down_fwd = false;
    e.bcx = 107;
    e.bcy = 389;
    e.bw = 78;
    e.bw_idle = 78;
    e.bh = 28;
    this->add_edge_(std::move(e));
  }
  if (d_bat != INVALID_INDEX && t_bat != INVALID_INDEX) {
    // The one edge on the diagram whose colour follows the sign of its value.
    // Kept as an obvious branch rather than a table: the battery terminal is
    // `bidirectional` and carries the BMS convention, positive is charging.
    Edge e;
    e.terminal = t_bat;
    e.kind = Kind::BATTERY;
    e.line_col = pal::batt_line;
    e.val_col = pal::batt;
    e.seg_rect = {{311, CORE_B, 2, 40}, {311, 388, 127, 2}, {436, 358, 2, 32}};
    e.head_fwd = {432, CORE_B, HEAD_W, HEAD_H};  // charging, up into the battery
    e.down_fwd = false;
    e.head_rev = {307, CORE_B, HEAD_W, HEAD_H};  // discharging, at the inverter end
    e.down_rev = true;                           // §5.2, verbatim: "down at (307, 350)"
    e.has_rev = true;
    e.bcx = 375;
    e.bcy = 389;
    e.bw = 78;
    e.bw_idle = 64;
    e.bh = 28;
    this->add_edge_(std::move(e));
  }

  // --- §4.4, the scrollable consumer block ---------------------------------
  //
  // Only the consumer list and its length of bus scroll. Everything above
  // y = 446 is pinned, the bus total badge included.
  this->scroll_ = this->plain_(this->root_);
  lv_obj_set_pos(this->scroll_, 0, SCROLL_Y);
  lv_obj_set_size(this->scroll_, SCREEN_W, SCROLL_H);

  std::vector<uint8_t> left, right;
  for (uint8_t ti : cons)
    (terms[ti].side == Side::LEFT ? left : right).push_back(ti);
  const int n_rows = (int) std::max(left.size(), right.size());
  const int n_last = n_rows > 0 ? n_rows - 1 : 0;

  for (int i = 0; i < (int) left.size(); i++) {
    Node n;
    n.slot = Slot::CONSUMER;
    n.terminal = left[i];
    this->build_consumer_(n, true, i);
    this->col_left_.push_back(this->add_node_(std::move(n)));
  }
  for (int i = 0; i < (int) right.size(); i++) {
    Node n;
    n.slot = Slot::CONSUMER;
    n.terminal = right[i];
    this->build_consumer_(n, false, i);
    this->col_right_.push_back(this->add_node_(std::move(n)));
  }

  // The bus, split at the scroll boundary. §5.2 gives it as one run
  // x239, y350 -> 776; §4.4 gives the same thing as 350..446 pinned plus
  // 102 + 114 * n_last inside the container. Draw only as much of it as there
  // are rows to feed: empty space at the bottom of the screen is correct, and a
  // redistributed layout would make the diagram jump whenever a socket is added.
  if (t_bus != INVALID_INDEX && n_rows > 0) {
    Edge e;
    e.terminal = t_bus;
    e.kind = Kind::BUS;
    e.line_col = pal::load_line;
    e.val_col = pal::load;
    e.seg_rect = {{BUS_X, CORE_B, 2, (int16_t) (SCROLL_Y - CORE_B)},
                  {BUS_X, 0, 2, (int16_t) (102 + ROW_PITCH * n_last)}};
    e.bcx = 240;
    e.bcy = 429;
    e.bw = has_pv ? 110 : 100;
    e.bw_idle = e.bw;
    e.bh = 34;
    e.n_dots = 2;
    e.dot_size = 8;
    e.dot_x = 236;
    e.dot_y0 = 356;
    e.dot_len = (int16_t) (198 + ROW_PITCH * n_last - 42);  // 384 for three rows
    e.dot_dir = 1;
    this->add_edge_(std::move(e));
  }

  // §5.3 — one connector per consumer, built at row 0 and translated by the row
  // that update() assigns it.
  auto consumer_edge = [&](uint8_t node_idx, bool is_left) {
    Node &n = this->nodes_[node_idx];
    Edge e;
    e.terminal = n.terminal;
    e.kind = Kind::CONSUMER;
    e.line_col = pal::load_line;
    e.val_col = pal::load;
    const int16_t vx = is_left ? 80 : 399;
    const int16_t hx = is_left ? 80 : 240;
    const int16_t hw = is_left ? 160 : 161;
    const int16_t card_b = ROW0_CARD_Y + CONS_H - SCROLL_Y;  // 76
    const int16_t hy = 546 - SCROLL_Y;                       // 100
    e.seg_rect = {{vx, (int16_t) (card_b + HEAD_H), 2, 18}, {hx, hy, hw, 2}};
    e.elastic = 0;
    e.elastic_head = {vx, (int16_t) (card_b + HEAD_H), 2, 18};
    e.elastic_bare = {vx, card_b, 2, 26};
    e.head_fwd = {(int16_t) (vx - 4), card_b, HEAD_W, HEAD_H};
    e.down_fwd = false;
    e.bcx = is_left ? 161 : 320;
    e.bcy = (int16_t) (hy + 1);
    e.bw = 78;
    e.bw_idle = 78;
    e.bh = 28;
    e.row = n.row;
    this->add_edge_(std::move(e));
    n.edge = (uint8_t) (this->edges_.size() - 1);
  };
  for (uint8_t idx : this->col_left_)
    consumer_edge(idx, true);
  for (uint8_t idx : this->col_right_)
    consumer_edge(idx, false);

  // --- realise every edge's objects ----------------------------------------
  for (Edge &e : this->edges_) {
    lv_obj_t *host = (e.kind == Kind::CONSUMER) ? this->scroll_ : this->root_;
    for (size_t i = 0; i < e.seg_rect.size(); i++) {
      // The bus is the one edge with a rectangle on each side of the boundary;
      // its lower length must be a child of the container so it scrolls with
      // the rows.
      lv_obj_t *p = (e.kind == Kind::BUS && i == 1) ? this->scroll_ : host;
      e.segs.push_back(this->rect_(p, e.seg_rect[i], e.line_col));
    }
    if (e.head_fwd.w != 0) {
      this->build_head_(e, host);
      this->place_head_(e, false);
    }
    size_t longest = 0;
    int best = -1;
    for (size_t i = 0; i < e.seg_rect.size(); i++) {
      const int len = std::max(e.seg_rect[i].w, e.seg_rect[i].h);
      if (len > best) {
        best = len;
        longest = i;
      }
    }
    const Rect &lr = e.seg_rect[longest];
    lv_obj_t *ch = (e.kind == Kind::BUS && longest == 1) ? this->scroll_ : host;
    this->build_cross_(e, ch, lr.x + lr.w / 2, lr.y + lr.h / 2);
  }

  // Badges after the lines and the heads, within the same host, so the pill's
  // fill hides the run it sits on (§5.4).
  for (Edge &e : this->edges_) {
    lv_obj_t *host = (e.kind == Kind::CONSUMER) ? this->scroll_ : this->root_;
    const bool bus = e.kind == Kind::BUS;
    const lv_font_t *f = bus ? s.m22 : s.m16;
    const int radius = bus ? BUS_BADGE_RADIUS : BADGE_RADIUS;
    this->build_badge_(e, host, radius, f);
    e.head_state = -2;  // unwritten: the first update places the head and the stub
  }

  // Dots last, on the root: the bus pair runs from 356 to past the scroll
  // boundary, so they cannot live inside the container.
  for (Edge &e : this->edges_) {
    for (uint8_t k = 0; k < e.n_dots && k < 2; k++) {
      lv_obj_t *d = this->plain_(this->root_);
      lv_obj_set_size(d, e.dot_size, e.dot_size);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_color(d, lv_color_hex(e.val_col), LV_PART_MAIN);
      lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
      e.dots[k] = d;
    }
  }

  // Everything a consumer takes with it when it sinks down the list.
  for (uint8_t idx : this->col_left_)
    this->register_consumer_movables_(idx);
  for (uint8_t idx : this->col_right_)
    this->register_consumer_movables_(idx);

  // §4.4 — scrolling engages only when the rows genuinely do not fit. The
  // scrollbar is the only affordance: no arrows, no "more" caption, and no snap
  // because rows are not pages.
  const int content_h = 102 + ROW_PITCH * n_last;
  const bool scrolls = content_h > SCROLL_H;
  if (scrolls) {
    lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(this->scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(this->scroll_, LV_SCROLLBAR_MODE_AUTO);
    // Without this the press lands on nothing: LVGL hit-tests clickable objects
    // only, and every card here is deliberately not one. The same omission made
    // the battery screen refuse to scroll.
    lv_obj_add_flag(this->scroll_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(this->scroll_, lv_color_hex(pal::divider), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(this->scroll_, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(this->scroll_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(this->scroll_, 2, LV_PART_SCROLLBAR);
  }

  // --- INVENTED: the "Home Assistant is gone" banner (§6.2) ----------------
  //
  // A sibling of root_, not a child, so dimming the diagram does not dim the one
  // thing that explains why it is dim. It sits across the middle of the screen,
  // over the bus total — the number that must not be trusted while this is up.
  // No ✕ is drawn anywhere in this state: with every meter NaN at once, a cross
  // on each one would say the mains are out when they are not.
  {
    const int bw = 448, bh = 72;
    this->banner_ = this->plain_(parent);
    lv_obj_set_pos(this->banner_, 16, 364);
    lv_obj_set_size(this->banner_, bw, bh);
    lv_obj_set_style_radius(this->banner_, CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(this->banner_, lv_color_hex(pal::card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(this->banner_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(this->banner_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(this->banner_, lv_color_hex(pal::alert), LV_PART_MAIN);
    lv_obj_set_style_border_opa(this->banner_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(this->banner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *b1 =
        this->label_(this->banner_, s.s16, 0, 16, bw - 2, LV_TEXT_ALIGN_CENTER, pal::alert);
    lv_label_set_text(b1, "NO LINK TO HOME ASSISTANT");
    lv_obj_t *b2 = this->label_(this->banner_, s.s14, 0, 44, bw - 2, LV_TEXT_ALIGN_CENTER,
                                pal::text_dim);
    lv_label_set_text(b2, "values are held; the state of the mains is unknown");
  }

  // --- what got drawn ------------------------------------------------------
  //
  // Deliberately INFO and not CONFIG: the panel logs at INFO, nobody can see the
  // screen from here, and these boot lines are the only way to check the layout
  // arithmetic against what the owner is actually looking at.
  lv_obj_update_layout(this->root_);
  const char *cfg = has_tap ? (has_pv ? "A" : "B") : (has_pv ? "D" : "C");
  ESP_LOGI(TAG, "configuration %s: grid card %d,%d %dx%d; %u nodes, %u edges", cfg, grid_x,
           (int) ROW1_Y, grid_w, (int) ROW1_H, (unsigned) this->nodes_.size(),
           (unsigned) this->edges_.size());
  ESP_LOGI(TAG, "consumers %u left / %u right, %d rows; bus %d..%d; container %d of %d%s",
           (unsigned) left.size(), (unsigned) right.size(), n_rows, (int) CORE_B,
           (int) SCROLL_Y + content_h, content_h, (int) SCROLL_H, scrolls ? " — SCROLLS" : "");
  ESP_LOGI(TAG, "line heights  sans 10/14/16/18/20/22: %d %d %d %d %d %d   mono 12/14/16/22/28: %d %d %d %d %d   icon %d",
           line_h(s.s10, 0), line_h(s.s14, 0), line_h(s.s16, 0), line_h(s.s18, 0),
           line_h(s.s20, 0), line_h(s.s22, 0), line_h(s.m12, 0), line_h(s.m14, 0),
           line_h(s.m16, 0), line_h(s.m22, 0), line_h(s.m28, 0), line_h(s.icon, 0));
  for (const Node &n : this->nodes_) {
    const Terminal *t = this->term_(n.terminal);
    const Device *d = this->dev_(n.device);
    ESP_LOGI(TAG, "  node %d  %3d,%-3d %3dx%-3d  %s", (int) n.slot, (int) lv_obj_get_x(n.box),
             (int) lv_obj_get_y(n.box), (int) lv_obj_get_width(n.box),
             (int) lv_obj_get_height(n.box),
             d != nullptr ? d->name.c_str() : (t != nullptr ? t->name.c_str() : "-"));
  }
  for (const Edge &e : this->edges_) {
    const Terminal *t = this->term_(e.terminal);
    ESP_LOGI(TAG, "  edge %d  %u runs, badge %dx%d centred %d,%d, %u dots  %s", (int) e.kind,
             (unsigned) e.segs.size(), (int) e.bw, (int) e.bh, (int) e.bcx, (int) e.bcy,
             (unsigned) e.n_dots, t != nullptr ? t->name.c_str() : "-");
  }
}

void FlowRenderer::register_consumer_movables_(uint8_t node_idx) {
  Node &n = this->nodes_[node_idx];
  if (n.edge == INVALID_INDEX)
    return;
  Edge &e = this->edges_[n.edge];
  for (size_t i = 0; i < e.segs.size(); i++)
    n.movable.push_back({e.segs[i], e.seg_rect[i].y});
  for (int k = 0; k < HEAD_STEPS; k++) {
    if (e.head[k] == nullptr)
      continue;
    // The row-0 y of step k, recomputed rather than read back: lv_obj_get_y()
    // needs a layout pass that has not happened yet at this point.
    const int16_t y = e.down_fwd ? (int16_t) (e.head_fwd.y + k * 2)
                                 : (int16_t) (e.head_fwd.y + HEAD_H - 2 - k * 2);
    n.movable.push_back({e.head[k], y});
  }
  n.movable.push_back({e.badge, (int16_t) (e.bcy - e.bh / 2)});
  n.movable.push_back({e.cross, (int16_t) (e.bcy - CROSS_D / 2)});
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

const Terminal *FlowRenderer::term_(uint8_t index) const {
  if (this->pf_ == nullptr || index == INVALID_INDEX)
    return nullptr;
  const auto &t = this->pf_->terminals();
  if (index >= t.size())
    return nullptr;
  return &t[index];
}

const Device *FlowRenderer::dev_(uint8_t index) const {
  if (this->pf_ == nullptr || index == INVALID_INDEX)
    return nullptr;
  const auto &d = this->pf_->devices();
  if (index >= d.size())
    return nullptr;
  return &d[index];
}

/// §6's four rendering states in one function. When Home Assistant is
/// unreachable every meter reads NaN at once, so every edge would otherwise be
/// de-energized and the whole diagram would claim the power is out. It is not:
/// the answer is "we do not know", and no ✕ is drawn at all (§6.2).
uint32_t FlowRenderer::edge_line_color_(const Edge &e, EdgeState st) const {
  if (!this->ha_contact_)
    return pal::nodata_line;
  switch (st) {
    case EdgeState::ACTIVE:
      return e.line_col;
    case EdgeState::OPEN:
      return pal::off_line;
    case EdgeState::DE_ENERGIZED:
      return pal::nodata_line;
    default:
      return pal::idle_line;  // IDLE and NO_DATA: the wire is there and quiet
  }
}

/// For the inverter, offline means neither of its two AC meters is talking; a
/// single dead meter is a dead edge, not a dead inverter.
bool FlowRenderer::inverter_offline_() const {
  if (!this->ha_contact_)
    return false;
  const Terminal *in = this->term_(this->t_in_);
  const Terminal *out = this->term_(this->t_out_);
  if (in == nullptr && out == nullptr)
    return false;
  auto mute = [](const Terminal *t) {
    return t == nullptr || t->state == EdgeState::DE_ENERGIZED || t->state == EdgeState::NO_DATA;
  };
  return mute(in) && mute(out);
}

/// For the battery it means the BLE bridge is down: no state of charge and no
/// power. `unavailable: no_data` keeps that out of the balance, but the box
/// still has to say why the ring is empty.
bool FlowRenderer::battery_offline_() const {
  if (!this->ha_contact_)
    return false;
  const Device *d = this->dev_(this->d_bat_);
  if (d != nullptr && d->soc != nullptr && is_valid(d->soc->state))
    return false;
  const Terminal *t = this->term_(this->t_bat_);
  return t == nullptr || t->state == EdgeState::DE_ENERGIZED || t->state == EdgeState::NO_DATA;
}

int FlowRenderer::set_metric_(lv_obj_t *num, lv_obj_t *unit, std::string &cache,
                              const std::string &value, const char *suffix, int x, int y,
                              bool centre) {
  if (num == nullptr || unit == nullptr)
    return 0;
  const PowerFlowStyle &s = this->pf_->style();
  const int wn = text_width(s.m28, value.c_str());
  const int wu = text_width(s.m12, suffix);
  const std::string key = value + '\x01' + suffix;
  if (cache == key)
    return wn + wu;
  cache = key;
  lv_label_set_text(num, value.c_str());
  lv_label_set_text(unit, suffix);
  const int x0 = centre ? x - (wn + wu) / 2 : x;
  // A big figure and a small unit only read as one number when their baselines
  // agree; the line boxes do not.
  const int dy = baseline_of(s.m28, 27) - baseline_of(s.m12, 14);
  lv_obj_set_pos(num, x0, y);
  lv_obj_set_pos(unit, x0 + wn, y + dy);
  return wn + wu;
}

void FlowRenderer::update_node_(Node &n) {
  const PowerFlowStyle &s = this->pf_->style();
  const Diagnostics &dg = this->pf_->diagnostics();
  const Terminal *t = this->term_(n.terminal);
  const Device *d = this->dev_(n.device);
  const bool ha = this->ha_contact_;

  const EdgeState st = (t == nullptr) ? EdgeState::NO_DATA : t->state;
  // §4.3 and §6: a de-energized *or* deliberately open node goes pale. Only the
  // ✕ on the connector separates the two, and that is the point — a load the
  // owner switched off is behaving exactly as intended and earns a caption.
  bool dead = ha && (st == EdgeState::DE_ENERGIZED || st == EdgeState::OPEN);

  std::string name;
  if (n.slot == Slot::CONSUMER || n.slot == Slot::OTHER || n.slot == Slot::TAP)
    name = (t != nullptr) ? t->name : "";
  else if (d != nullptr)
    name = d->name;
  else if (t != nullptr)
    name = t->name;
  set_text(n.name, n.txt_name, name);

  switch (n.slot) {
    case Slot::GRID: {
      // The sub-line is the grid device's voltage, integer + V.
      //
      // INVENTED, and the only thing that ever displaces it: when the inference
      // distrusts the grid (§6.7) the verdict takes this line, because the
      // voltage is precisely the number the verdict is about. Nothing is drawn
      // here when the verdict is TRUSTED — a healthy panel says nothing, which
      // is why the old `GRID TRUSTED` strip was deleted.
      const bool bad = ha && dg.grid != GridVerdict::TRUSTED;
      if (bad) {
        set_text(n.sub, n.txt_sub, dg.grid == GridVerdict::OFFLINE ? "OFFLINE" : "SUSPECT");
        set_color(n.sub, n.col_sub, pal::alert);
        dead = dead || dg.grid == GridVerdict::OFFLINE;
      } else {
        const float v = (d != nullptr && d->voltage != nullptr) ? d->voltage->state : NAN;
        if (ha && is_valid(v)) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%.0f V", v);
          set_text(n.sub, n.txt_sub, buf);
          set_color(n.sub, n.col_sub, pal::text_dim);
        } else {
          set_text(n.sub, n.txt_sub, DASH);
          set_color(n.sub, n.col_sub, pal::text_off);
        }
      }
      break;
    }

    case Slot::PV: {
      // `—` until a PV meter exists. A PV device with its own voltage sensor
      // gets that instead; there is none today.
      const float v = (d != nullptr && d->voltage != nullptr) ? d->voltage->state : NAN;
      if (ha && is_valid(v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f V", v);
        set_text(n.sub, n.txt_sub, buf);
        set_color(n.sub, n.col_sub, pal::text_dim);
      } else {
        set_text(n.sub, n.txt_sub, DASH);
        set_color(n.sub, n.col_sub, pal::text_off);
      }
      break;
    }

    case Slot::INVERTER: {
      const bool off = this->inverter_offline_();
      if (off != n.offline_shown) {
        n.offline_shown = off;
        set_hidden(n.offline, !off);
        set_hidden(n.cap_loss, off);
        set_hidden(n.cap_eff, off);
        set_hidden(n.val_loss, off);
        set_hidden(n.unit_loss, off);
        set_hidden(n.val_eff, off);
        set_hidden(n.unit_eff, off);
      }
      dead = dead || off;
      if (!off) {
        // LOSS and EFF, straight off the component's EnergyReading. Both show a
        // dash when their validity condition fails — never an estimate (§9).
        const EnergyReading &er = dg.energy;
        std::string num, unit;
        if (ha && er.show_losses && is_valid(er.losses)) {
          split_number(fmt_power(er.losses), num, unit);
        } else {
          num = DASH;
          unit.clear();
        }
        this->set_metric_(n.val_loss, n.unit_loss, n.txt_loss, num, unit.c_str(), INV_PAD_X,
                          INV_VAL_Y, false);

        if (ha && er.show_efficiency && is_valid(er.efficiency)) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%.0f", er.efficiency * 100.0f);
          this->set_metric_(n.val_eff, n.unit_eff, n.txt_eff, buf, " %", INV_EFF_X, INV_VAL_Y,
                            false);
        } else {
          this->set_metric_(n.val_eff, n.unit_eff, n.txt_eff, DASH, "", INV_EFF_X, INV_VAL_Y,
                            false);
        }
      }
      // INVENTED: an implausible balance. See the build for the reasoning.
      const bool unrel = ha && !dg.reliable && !off;
      if (unrel != n.unreliable_shown) {
        n.unreliable_shown = unrel;
        set_hidden(n.unreliable, !unrel);
      }
      break;
    }

    case Slot::BATTERY: {
      const bool off = this->battery_offline_();
      if (off != n.offline_shown) {
        n.offline_shown = off;
        set_hidden(n.offline, !off);
        set_hidden(n.ring, off);
        set_hidden(n.soc_num, off);
        set_hidden(n.soc_pct, off);  // re-hidden below if the SOC itself is a dash
      }
      dead = dead || off;
      if (!off) {
        const float soc = (d != nullptr && d->soc != nullptr) ? d->soc->state : NAN;
        char buf[12];
        if (is_valid(soc))
          snprintf(buf, sizeof(buf), "%.0f", soc);
        else
          snprintf(buf, sizeof(buf), "%s", DASH);
        // Both labels are fixed-width and centre their own text, so nothing has
        // to be re-placed as the value changes width.
        set_text(n.soc_num, n.txt_soc, buf);
        set_hidden(n.soc_pct, !is_valid(soc));
        const int32_t pct =
            is_valid(soc) ? (int32_t) lroundf(std::min(100.0f, std::max(0.0f, soc))) : 0;
        if (pct != n.ring_pct) {
          n.ring_pct = pct;
#if LV_USE_ARC
          if (n.ring != nullptr)
            lv_arc_set_value(n.ring, pct);
#endif
        }
      }
      break;
    }

    default:
      // A consumer and the remainder say nothing in words: the box is one line,
      // an icon and a name, and the ✕ on the connector already says what is
      // wrong. A second line there would cost the box twice its height.
      break;
  }

  const uint32_t bg = dead ? pal::card_dead : pal::card;
  uint32_t border = dead ? pal::border_dead : pal::border;
  if (n.slot == Slot::PV && !dead)
    border = pal::pv_border;
  if (n.slot == Slot::INVERTER && n.unreliable_shown)
    border = pal::alert;
  if (n.slot == Slot::GRID && ha && dg.grid != GridVerdict::TRUSTED)
    border = pal::alert;

  if (bg != n.col_bg) {
    n.col_bg = bg;
    lv_obj_set_style_bg_color(n.box, lv_color_hex(bg), LV_PART_MAIN);
  }
  if (border != n.col_border) {
    n.col_border = border;
    lv_obj_set_style_border_color(n.box, lv_color_hex(border), LV_PART_MAIN);
  }
  set_color(n.name, n.col_name, dead ? pal::text_off : pal::text);
  if (n.icon != nullptr) {
    uint32_t ic = pal::load;
    switch (n.slot) {
      case Slot::GRID: ic = pal::grid; break;
      case Slot::PV: ic = pal::pv; break;
      case Slot::TAP: ic = pal::tap_val; break;
      default: break;
    }
    set_color(n.icon, n.col_icon, dead ? pal::icon_off : ic);
  }
  n.dead = dead;
}

void FlowRenderer::set_badge_(Edge &e, const std::string &txt, uint32_t line, uint32_t text,
                              int width, const lv_font_t *font) {
  if (e.badge == nullptr)
    return;
  if (txt.empty()) {
    if (e.badge_shown) {
      e.badge_shown = false;
      set_hidden(e.badge, true);
    }
    return;
  }
  if (!e.badge_shown) {
    e.badge_shown = true;
    set_hidden(e.badge, false);
  }
  if (font != e.badge_font) {
    e.badge_font = font;
    lv_obj_set_style_text_font(e.badge_lbl, font, LV_PART_MAIN);
    // The `OFF` caption is the one letter-spaced badge in the design.
    lv_obj_set_style_text_letter_space(e.badge_lbl,
                                       font == this->pf_->style().s14 ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_y(e.badge_lbl, (e.bh - 2 - line_h(font, 20)) / 2);
    e.txt_badge.clear();
  }
  if (txt != e.txt_badge) {
    e.txt_badge = txt;
    lv_label_set_text(e.badge_lbl, txt.c_str());
    // §5.4 fixes the width and centres the text so digits of unequal width are
    // absorbed. Montserrat is wider than the mockups' mono face, so the pill is
    // allowed to grow symmetrically about its centre rather than clip; it never
    // shrinks below the spec's number.
    int w = width;
    const int need = text_width(font, txt.c_str()) + BADGE_PAD;
    if (need > w)
      w = need;
    lv_obj_set_pos(e.badge, e.bcx - w / 2, e.bcy + ROW_PITCH * e.row - e.bh / 2);
    lv_obj_set_size(e.badge, w, e.bh);
    lv_obj_set_width(e.badge_lbl, w - 2);
  }
  if (line != e.col_badge) {
    e.col_badge = line;
    lv_obj_set_style_border_color(e.badge, lv_color_hex(line), LV_PART_MAIN);
  }
  set_color(e.badge_lbl, e.col_badge_txt, text);
}

void FlowRenderer::update_edge_(Edge &e) {
  const PowerFlowStyle &s = this->pf_->style();
  const Terminal *t = this->term_(e.terminal);
  const bool ha = this->ha_contact_;
  const EdgeState st = (t == nullptr) ? EdgeState::NO_DATA : t->state;
  // `display` for a measured edge, `value` for a solved one — the component
  // fills both, and the difference is why a socket's badge tracks the kettle
  // while `Other` and LOSS stay steady through the ten seconds it takes the two
  // meters either side of the inverter to agree again.
  const float v = (t != nullptr) ? t->display : NAN;

  const bool active = ha && st == EdgeState::ACTIVE;
  const bool open = ha && st == EdgeState::OPEN;
  // The ✕ means contact with this device is lost and nothing else. A load the
  // owner switched off gets the OFF caption. With Home Assistant gone there is
  // no ✕ anywhere at all (§6.2).
  const bool cross = ha && st == EdgeState::DE_ENERGIZED;

  // The battery is the one edge whose colour follows the sign of its value:
  // charging green, discharging amber, and the head moves with the hue, so the
  // edge says which way the energy is going twice over.
  const bool discharging = e.kind == Kind::BATTERY && active && is_valid(v) && v < 0.0f;
  const uint32_t role_line = discharging ? pal::discharge_line : e.line_col;
  const uint32_t role_val = discharging ? pal::discharge : e.val_col;

  const uint32_t col = active ? role_line : this->edge_line_color_(e, st);
  if (col != e.col_line) {
    e.col_line = col;
    const lv_color_t c = lv_color_hex(col);
    for (lv_obj_t *o : e.segs)
      lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    for (lv_obj_t *o : e.head)
      if (o != nullptr)
        lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    for (lv_obj_t *o : e.dots)
      if (o != nullptr)
        lv_obj_set_style_bg_color(o, lv_color_hex(role_val), LV_PART_MAIN);
  }

  // --- the head: only an active edge has a direction worth drawing ---------
  const int8_t want = !active ? -1 : (int8_t) ((discharging && e.has_rev) ? 1 : 0);
  if (want != e.head_state) {
    e.head_state = want;
    if (want < 0) {
      for (lv_obj_t *o : e.head)
        set_hidden(o, true);
    } else {
      this->place_head_(e, want == 1);
      for (lv_obj_t *o : e.head)
        set_hidden(o, false);
    }
    // A consumer's stub starts 8 px lower when there is a head below the card.
    if (e.elastic >= 0 && e.elastic < (int) e.segs.size()) {
      const Rect &r = want >= 0 ? e.elastic_head : e.elastic_bare;
      lv_obj_set_pos(e.segs[e.elastic], r.x, r.y + ROW_PITCH * e.row);
      lv_obj_set_size(e.segs[e.elastic], r.w, r.h);
    }
  }

  if (cross != e.cross_shown) {
    e.cross_shown = cross;
    set_hidden(e.cross, !cross);
  }

  // --- the badge: this edge's flow figure, and the only place it appears ---
  std::string txt;
  uint32_t bcol = role_val;
  int bw = e.bw;
  // Every badge is a number, so every badge is mono; only the bus total steps up
  // a size (§8). The row-1 grid badge used to be its own size and no longer is.
  const lv_font_t *bfont = e.kind == Kind::BUS ? s.m22 : s.m16;

  if (!ha) {
    txt = DASH;
    bcol = pal::text_off;
  } else if (st == EdgeState::DE_ENERGIZED) {
    txt.clear();  // the ✕ is the statement; a figure beside it would lie
  } else if (open) {
    txt = "OFF";
    bcol = pal::text_dim;
    bw = 66;
    bfont = s.s14;  // OFF is a word
  } else if (st == EdgeState::NO_DATA) {
    txt = DASH;
    bcol = pal::text_off;
  } else if (st == EdgeState::IDLE) {
    txt = "0 W";  // §6: an idle edge reads zero, whatever the residual says
    bcol = pal::text_off;
    bw = e.bw_idle;
  } else if (t != nullptr && t->bidirectional) {
    txt = fmt_signed_power(v);  // the battery: the sign is the direction
  } else {
    txt = fmt_power(v);
  }
  this->set_badge_(e, txt, col, bcol, bw, bfont);

  // --- dots ----------------------------------------------------------------
  const bool run = active && is_valid(v) && e.n_dots > 0 && e.dot_len > 0;
  if (run != e.animate) {
    e.animate = run;
    for (lv_obj_t *o : e.dots)
      set_hidden(o, !run);
  }
  if (run) {
    const float k = std::sqrt(std::min(1.0f, std::fabs(v) / DOT_SPEED_REF));
    const float px_s = DOT_SPEED_MIN + (DOT_SPEED_MAX - DOT_SPEED_MIN) * k;
    e.speed = px_s / (float) e.dot_len;  // runs per second
  }
}

/// §4.3 — entries that are off or no-data sink to the last rows of their column,
/// so the list is ordered by the config and not by what the power happens to be
/// doing. `side:` is the config's, never inferred.
///
/// The order is only recomputed while Home Assistant is answering: at boot every
/// meter is NaN and sinking would be meaningless. One shuffle a few seconds
/// after power-up is preferable to a list that settles into the wrong order and
/// stays there.
void FlowRenderer::resort_consumers_() {
  // With Home Assistant gone every meter reads NaN at once, so nothing may sink:
  // that is loss of contact, not five sockets switching off, and reshuffling the
  // list on a network blip only to reshuffle it back is exactly the churn §4.3
  // exists to prevent. But the *first* pass must run regardless — until it does,
  // every card is still sitting at row 0 where it was built.
  if (!this->ha_contact_ && this->sorted_once_)
    return;
  auto sunk = [&](uint8_t node_idx) {
    if (!this->ha_contact_)
      return false;
    const Terminal *t = this->term_(this->nodes_[node_idx].terminal);
    return t != nullptr && (t->state == EdgeState::OPEN || t->state == EdgeState::DE_ENERGIZED);
  };
  std::vector<uint8_t> *cols[2] = {&this->col_left_, &this->col_right_};
  for (std::vector<uint8_t> *col : cols) {
    int row = 0;
    for (int pass = 0; pass < 2; pass++) {
      for (uint8_t idx : *col) {
        if ((pass == 1) != sunk(idx))
          continue;
        Node &n = this->nodes_[idx];
        if (n.row != (int8_t) row) {
          n.row = (int8_t) row;
          for (const Movable &m : n.movable)
            if (m.obj != nullptr)
              lv_obj_set_y(m.obj, m.base + ROW_PITCH * row);
          if (n.edge != INVALID_INDEX) {
            Edge &e = this->edges_[n.edge];
            e.row = (int8_t) row;
            e.txt_badge.clear();  // force the pill to be re-placed
            e.head_state = -2;    // and the head and the elastic stub with it
          }
        }
        row++;
      }
    }
  }
  // The setup dump prints where the cards were *built*, which is row 0 for all
  // of them; this prints where they ended up. Once, because after that it only
  // reports churn.
  if (!this->sorted_once_) {
    std::string line;
    for (const std::vector<uint8_t> *col : cols) {
      for (uint8_t idx : *col) {
        const Node &n = this->nodes_[idx];
        const Terminal *t = this->term_(n.terminal);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s=row%d%s ", t != nullptr ? t->name.c_str() : "?", n.row,
                 sunk(idx) ? "(sunk)" : "");
        line += buf;
      }
    }
    ESP_LOGI(TAG, "rows placed: %s", line.c_str());
  }
  this->sorted_once_ = true;
}

void FlowRenderer::update_overlays_() {
  const bool dim = !this->ha_contact_;
  if (dim != this->dimmed_) {
    this->dimmed_ = dim;
    lv_obj_set_style_opa(this->root_, dim ? OPA_NO_HA : (lv_opa_t) LV_OPA_COVER, LV_PART_MAIN);
    set_hidden(this->banner_, !dim);
  }
}

void FlowRenderer::update() {
  if (this->pf_ == nullptr || this->root_ == nullptr)
    return;

  this->ha_contact_ = this->pf_->diagnostics().ha_contact;
  this->update_overlays_();
  this->resort_consumers_();

  for (Node &n : this->nodes_)
    this->update_node_(n);
  for (Edge &e : this->edges_)
    this->update_edge_(e);
}

// ---------------------------------------------------------------------------
// §7 — the dots
// ---------------------------------------------------------------------------

void FlowRenderer::frame(uint32_t now) {
  if (this->root_ == nullptr)
    return;
  const uint32_t dt = now - this->last_frame_;
  if (dt < FRAME_MS)
    return;
  this->last_frame_ = now;
  if (this->dimmed_)
    return;  // nothing on this diagram is moving; do not spend the redraws

  // A long stall — a blocking component, a page change — must not teleport a dot
  // the length of the bus.
  const float secs = std::min<uint32_t>(dt, 250) / 1000.0f;

  for (Edge &e : this->edges_) {
    if (!e.animate || e.n_dots == 0 || e.dot_len <= 0)
      continue;
    e.phase += e.speed * secs;
    e.phase -= std::floor(e.phase);
    for (uint8_t k = 0; k < e.n_dots && k < 2; k++) {
      lv_obj_t *dot = e.dots[k];
      if (dot == nullptr)
        continue;
      float p = e.phase + (float) k / (float) e.n_dots;  // two dots, 50 % apart
      p -= std::floor(p);
      const int along = (int) (p * (float) e.dot_len);
      lv_obj_set_pos(dot, e.dot_x,
                     e.dot_dir > 0 ? e.dot_y0 + along : e.dot_y0 + e.dot_len - along);
    }
  }
}

// ---------------------------------------------------------------------------
// §11 — the tap
// ---------------------------------------------------------------------------

void FlowRenderer::node_event_cb_(lv_event_t *e) {
  auto *self = static_cast<FlowRenderer *>(lv_event_get_user_data(e));
  lv_obj_t *obj = lv_event_get_target_obj(e);
  if (self == nullptr || obj == nullptr)
    return;
  self->on_node_clicked_((uint8_t) (uintptr_t) lv_obj_get_user_data(obj));
}

/// Resolve the tapped node to its device and fire whatever the YAML bound to it.
/// The component still knows nothing about pages. A consumer belongs to no
/// device and therefore has nothing to fire — that is not an error, it is a
/// socket with no page behind it.
void FlowRenderer::on_node_clicked_(uint8_t node) {
  if (node >= this->nodes_.size())
    return;
  const Node &n = this->nodes_[node];
  const Terminal *t = this->term_(n.terminal);

  // Tell the detail screen which card this is *before* anything navigates. A
  // consumer has no device of its own, and that absence is the discriminator:
  // the screen reads the pair and decides which of the two carries the name.
  this->pf_->select_node(n.device, n.terminal);

  // A device may bind its own action — the battery does, because it opens a
  // different screen. Everything else falls through to the shared one.
  const Device *d = this->dev_(n.device);
  Trigger<> *trig = d != nullptr && d->on_click != nullptr ? d->on_click : this->pf_->on_node_click();
  if (trig == nullptr) {
    ESP_LOGD(TAG, "tap on %s: nothing bound", t != nullptr ? t->name.c_str() : "?");
    return;
  }
  ESP_LOGI(TAG, "tap on %s (device %u, terminal %u)",
           t != nullptr ? t->name.c_str() : (d != nullptr ? d->name.c_str() : "?"),
           (unsigned) n.device, (unsigned) n.terminal);
  trig->trigger();
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
