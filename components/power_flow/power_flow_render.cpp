#include "power_flow_render.h"

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
// Renderer geometry.
//
// These describe *how* a connector is drawn — line weights, the resolution of
// the point table, the animation tick, the padding inside a badge. Everything
// the owner would want to tune (colour, box size, radius, ring thickness,
// fonts) comes from PowerFlowStyle, and nothing here duplicates it (§8).
// ---------------------------------------------------------------------------
static const int16_t EDGE_THICK_ACTIVE = 4;
static const int16_t EDGE_THICK_IDLE = 2;
static const int16_t EDGE_THICK_OFF = 1;  ///< "a thin off_color line" (§6.1)
static const int16_t DOT_SIZE = 8;
static const int PATH_STEP = 2;       ///< px between point-table entries
static const int MIN_DOT_PATH = 24;   ///< px; a shorter run cannot read as motion
static const int16_t CROSS_SIZE = 22;
static const uint32_t FRAME_MS = 40;  ///< ~25 fps (§7)
static const int BADGE_PAD_X = 7;
static const int BADGE_PAD_Y = 2;
static const int MARGIN = 10;         ///< page edge to the outermost box
static const int GAP = 12;            ///< between boxes on the same band
static const int LINE_GAP = 4;        ///< between stacked lines inside a box
static const int ARROW_LEN = 10;      ///< tip to base
static const int ARROW_HALF = 6;      ///< half-width at the base
/// Clear space between the ring's inner edge and the glyphs inside it. The
/// owner's complaint was that the two ran together; this is the gap that stops
/// them, and it is what the font choice is measured against.
static const int RING_TEXT_MARGIN = 5;

/// The widest string either power formatter can ever produce. Every run that
/// has to carry a badge is sized against this, not against today's values —
/// otherwise the first time a load crosses 500 W the pill outgrows its wire.
static const char *const WIDEST_POWER = "-888.88 kW";
/// The widest string the ring can ever hold. `100%` is one glyph more than
/// `99%`, and sizing for the latter is how a full battery breaks the layout.
static const char *const WIDEST_SOC = "100%";

/// Dot speed is proportional to averaged power, compressed with a square root:
/// linear scaling puts every domestic load in the bottom tenth of the range,
/// so 50 W and 500 W would look identical.
static const float DOT_SPEED_MIN = 16.0f;
static const float DOT_SPEED_MAX = 72.0f;
static const float DOT_SPEED_REF = 1500.0f;  ///< W at which the dots run flat out

/// Opacity is the only dimming mechanism here, because it needs no colour
/// constant of its own: a stale edge is the same colour shown weaker (§6.4),
/// and a diagram with no Home Assistant behind it is the whole tree shown
/// weaker (§6.2).
static const lv_opa_t OPA_STALE = 130;
static const lv_opa_t OPA_NO_HA = 90;
static const lv_opa_t OPA_CAPTION = 190;
static const lv_opa_t OPA_LINE_IDLE = 120;

/// Material Design Icons, one per slot, in Slot order. Codepoints taken from
/// scss/_variables.scss at tag v7.4.47 — the tag the panel pins — so they stay
/// valid as long as that pin does. Each is probed with the font's own glyph
/// lookup before it is used and its label is dropped when the glyph is absent:
/// an unlisted codepoint renders as nothing, silently, and a silent blank is
/// worse than no icon at all.
///
/// Consumers all take the same plug: there is no per-consumer `icon:` key, and
/// guessing an appliance from its name would be wrong the first time the owner
/// renames one.
static uint32_t icon_for_slot(uint8_t slot) {
  switch (slot) {
    case 0: return 0x0F0D3E;   // GRID      transmission-tower
    case 1: return 0x0F0A72;   // PV        solar-power
    case 2: return 0x0F1425;   // ONGRID    power-plug-outline
    case 3: return 0x0F095B;   // INVERTER  sine-wave
    case 4: return 0x0F008E;   // BATTERY   battery-outline
    case 5: return 0x0F1425;   // CONSUMER  power-plug-outline
    default: return 0x0F01D8;  // OTHERS    dots-horizontal
  }
}

static const char *const SLOT_NAME[] = {"grid", "pv", "ongrid", "inv", "batt", "load", "rest"};

/// UTF-8 for one codepoint. MDI lives in a private use plane, so these are all
/// four bytes; the shorter branches exist so the helper is not quietly wrong
/// if it is reused.
static void encode_utf8(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char) cp;
    out[1] = 0;
  } else if (cp < 0x800) {
    out[0] = (char) (0xC0 | (cp >> 6));
    out[1] = (char) (0x80 | (cp & 0x3F));
    out[2] = 0;
  } else if (cp < 0x10000) {
    out[0] = (char) (0xE0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F));
    out[3] = 0;
  } else {
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    out[4] = 0;
  }
}

// ---------------------------------------------------------------------------
// Formatting — §6.9 in a handful of functions: every NaN is a dash, never a
// rough number. U+2014 EM DASH and U+00D7 MULTIPLICATION SIGN are both present
// in GF_Latin_Core, the glyphset the panel's Roboto faces are built with, so
// both are safe to emit. (Checked against esphome.components.font.glyphsets.)
// ---------------------------------------------------------------------------
static const char *const DASH = "\xE2\x80\x94";  // U+2014
static const char *const CROSS = "\xC3\x97";     // U+00D7

/// Two ranges and one threshold (§7): below 500 W as whole watts, 500 W and up
/// as kilowatts to two decimals. One crossing point, so the eye is never asked
/// to compare `900 W` against `1.1 kW`.
///
/// The unit is the SI `kW`. The owner wrote `Kw`; flagged to him rather than
/// transcribed, and it is one string here if he prefers otherwise.
static const float KW_FROM = 500.0f;

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

static const char *supply_word(SupplyMode m) {
  switch (m) {
    case SupplyMode::GRID: return "GRID";
    case SupplyMode::BATTERY: return "BATTERY";
    default: return DASH;
  }
}

static const char *grid_word(GridVerdict v) {
  switch (v) {
    case GridVerdict::SUSPECT: return "SUSPECT";
    case GridVerdict::OFFLINE: return "OFFLINE";
    default: return "TRUSTED";
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

static int text_width(const lv_font_t *font, const char *txt) {
  if (font == nullptr || txt == nullptr)
    return 0;
  lv_point_t sz;
  lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return (int) sz.x;
}

static int line_h(const lv_font_t *font, int fallback) {
  return font != nullptr ? lv_font_get_line_height(font) : fallback;
}

/// Which way an arrowhead points: 0 right, 1 down, 2 left, 3 up.
static uint8_t dir_of(int dx, int dy) {
  if (dx > 0)
    return 0;
  if (dx < 0)
    return 2;
  if (dy > 0)
    return 1;
  return 3;
}

/// Lay one axis-aligned connector run out for a given weight. Both ends are
/// grown by half the weight so the corner between two runs is filled rather
/// than notched, and so the free end meets the node box it points at.
static void place_run(lv_obj_t *obj, int x1, int y1, int x2, int y2, int thick) {
  const int t = thick < 1 ? 1 : thick;
  const int h = t / 2;
  if (y1 == y2) {
    const int x = std::min(x1, x2) - h;
    const int len = std::abs(x2 - x1) + t;
    lv_obj_set_pos(obj, x, y1 - h);
    lv_obj_set_size(obj, len, t);
  } else {
    const int y = std::min(y1, y2) - h;
    const int len = std::abs(y2 - y1) + t;
    lv_obj_set_pos(obj, x1 - h, y);
    lv_obj_set_size(obj, t, len);
  }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

bool FlowRenderer::has_glyph_(const lv_font_t *font, uint32_t cp) const {
  if (font == nullptr || font->get_glyph_dsc == nullptr)
    return false;
  lv_font_glyph_dsc_t dsc;
  // Deliberately the font's own callback and not lv_font_get_glyph_dsc(),
  // which falls back to a placeholder and would report success for a glyph
  // that is not there.
  return font->get_glyph_dsc(font, &dsc, cp, 0);
}

/// A bare rectangle: no theme fill, no border, no padding, no scrolling. Every
/// object in this renderer starts life here so nothing inherits a surprise
/// from the default theme.
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

lv_obj_t *FlowRenderer::label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                               lv_text_align_t align, uint32_t color, uint8_t opa) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
  if (font != nullptr)
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_opa(l, opa, LV_PART_MAIN);
  lv_obj_set_width(l, w);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, "");
  return l;
}

/// Pick one font for a whole group of names. Per-name fitting would leave a
/// column of consumers in two different type sizes, which reads as a mistake;
/// one size for the group reads as a decision. Each name is measured against
/// its own box, so one narrow box does not shrink the whole group.
const lv_font_t *FlowRenderer::fit_font_(const std::vector<NameFit> &names) const {
  const PowerFlowStyle &s = this->pf_->style();
  if (s.value_font == nullptr)
    return s.label_font;
  for (const NameFit &n : names)
    if (text_width(s.value_font, n.first.c_str()) > n.second)
      return s.label_font;
  return s.value_font;
}

uint8_t FlowRenderer::add_node_(Slot slot, uint8_t device, uint8_t terminal, int x, int y, int w,
                                int h, const lv_font_t *nf, int lines) {
  const PowerFlowStyle &s = this->pf_->style();
  const int pad = std::max(6, (int) s.radius / 2);

  Node n;
  n.slot = slot;
  n.device = device;
  n.terminal = terminal;

  n.box = this->plain_(this->root_);
  lv_obj_set_pos(n.box, x, y);
  lv_obj_set_size(n.box, w, h);
  lv_obj_set_style_radius(n.box, s.radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(n.box, lv_color_hex(s.node_bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(n.box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(n.box, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(n.box, lv_color_hex(s.node_border), LV_PART_MAIN);
  lv_obj_set_style_border_opa(n.box, LV_OPA_COVER, LV_PART_MAIN);

  // Every box is tappable; the handler resolves it to a device and fires that
  // device's trigger. The owner's first use is the battery, whose page has no
  // other way in since the bottom button was removed.
  lv_obj_add_flag(n.box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(n.box, (void *) (uintptr_t) this->nodes_.size());
  lv_obj_add_event_cb(n.box, FlowRenderer::node_event_cb_, LV_EVENT_CLICKED, this);

  const uint32_t cp = icon_for_slot((uint8_t) slot);
  const bool want_icon = this->has_glyph_(s.icon_font, cp);
  const int ih = line_h(s.icon_font, 26);
  const int nh_line = line_h(nf, 20);
  const int sh = line_h(s.label_font, 16);
  const int row_h = std::max(want_icon ? ih : 0, nh_line);

  int tx = pad;
  int tw = w - 2 * pad;
  if (want_icon && w - 2 * pad - ih - 6 >= 20) {
    tx = pad + ih + 6;
    tw = w - pad - tx;
  }

  if (slot == Slot::BATTERY) {
    // The ring first, then the name row under it. The ring is the only level
    // on a diagram of flows and says so without a caption (§7); its diameter
    // therefore comes from the box, and the percentage is fitted to the ring
    // rather than the ring being assumed to fit the percentage.
    const int name_y = h - pad - row_h;
    const int d = std::min(w - 2 * pad, name_y - LINE_GAP - pad);
    const int rx = (w - d) / 2;
    const int inner = d - 2 * (int) s.ring_width;
    const int usable = inner - 2 * RING_TEXT_MARGIN;
    const lv_font_t *rf = s.value_font;
    if (rf == nullptr || text_width(rf, WIDEST_SOC) > usable || line_h(rf, 28) > usable)
      rf = s.label_font;
    ESP_LOGI(TAG, "ring: outer %d, ring_width %d, inner %d, usable %d — \"%s\" is %d px, using %s",
             d, (int) s.ring_width, inner, usable, WIDEST_SOC, text_width(rf, WIDEST_SOC),
             rf == s.value_font ? "value_font" : "label_font");

#if LV_USE_ARC
    n.ring = lv_arc_create(n.box);
    lv_obj_remove_style(n.ring, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(n.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(n.ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(n.ring, rx, pad);
    lv_obj_set_size(n.ring, d, d);
    lv_obj_set_style_pad_all(n.ring, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(n.ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_arc_set_mode(n.ring, LV_ARC_MODE_NORMAL);
    lv_arc_set_rotation(n.ring, 270);
    lv_arc_set_bg_angles(n.ring, 0, 360);
    lv_arc_set_range(n.ring, 0, 100);
    lv_arc_set_value(n.ring, 0);
    lv_obj_set_style_arc_width(n.ring, s.ring_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(n.ring, s.ring_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(n.ring, lv_color_hex(s.idle_color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(n.ring, lv_color_hex(s.active_color), LV_PART_INDICATOR);
#endif
    const int rh = line_h(rf, 20);
    n.ring_lbl = this->label_(n.box, rf, rx, pad + (d - rh) / 2, d, LV_TEXT_ALIGN_CENTER,
                              s.text_color, LV_OPA_COVER);
    // Where the ring goes when the BMS is not reachable at all. The battery is
    // spine: it says OFFLINE in words rather than only going pale (§6.2).
    n.r1 = this->label_(n.box, s.label_font, pad, pad + (d - sh) / 2, w - 2 * pad,
                        LV_TEXT_ALIGN_CENTER, s.dead_color, LV_OPA_COVER);

    if (tx > pad)
      n.icon = this->label_(n.box, s.icon_font, pad, name_y + (row_h - ih) / 2, ih + 4,
                            LV_TEXT_ALIGN_LEFT, s.text_color, LV_OPA_COVER);
    n.name = this->label_(n.box, nf, tx, name_y + (row_h - nh_line) / 2, tw, LV_TEXT_ALIGN_LEFT,
                          s.text_color, LV_OPA_COVER);
  } else {
    // A centred vertical stack: the name row, then `lines` small reading lines.
    const int total = row_h + lines * (sh + LINE_GAP);
    int ty = std::max(2, (h - total) / 2);
    if (tx > pad)
      n.icon = this->label_(n.box, s.icon_font, pad, ty + (row_h - ih) / 2, ih + 4,
                            LV_TEXT_ALIGN_LEFT, s.text_color, LV_OPA_COVER);
    n.name = this->label_(n.box, nf, tx, ty + (row_h - nh_line) / 2, tw, LV_TEXT_ALIGN_LEFT,
                          s.text_color, LV_OPA_COVER);
    ty += row_h;
    lv_obj_t **slots[3] = {&n.r1, &n.r2, &n.r3};
    for (int i = 0; i < lines && i < 3; i++) {
      ty += LINE_GAP;
      *slots[i] = this->label_(n.box, s.label_font, tx, ty, tw, LV_TEXT_ALIGN_LEFT, s.text_color,
                               OPA_CAPTION);
      ty += sh;
    }
  }

  if (n.icon != nullptr) {
    char utf8[8];
    encode_utf8(cp, utf8);
    lv_label_set_text(n.icon, utf8);
  }
  if (n.name != nullptr)
    lv_label_set_long_mode(n.name, LV_LABEL_LONG_MODE_WRAP);

  this->nodes_.push_back(n);
  return (uint8_t) (this->nodes_.size() - 1);
}

/// Five rectangles, widest at the base. Re-placed only when the direction of
/// flow flips, which is a state transition and not a frame.
void FlowRenderer::place_arrow_(Edge &e, bool rev) {
  const Pt tip = rev ? e.tip_rev : e.tip_fwd;
  const uint8_t d = rev ? e.dir_rev : e.dir_fwd;
  for (int k = 0; k < ARROW_STEPS; k++) {
    lv_obj_t *o = e.arrow[k];
    if (o == nullptr)
      continue;
    const int hw = std::max(1, ARROW_HALF * (ARROW_STEPS - k) / ARROW_STEPS);
    const int off = k * 2;
    switch (d) {
      case 0:  // pointing right
        lv_obj_set_pos(o, tip.x - ARROW_LEN + off, tip.y - hw);
        lv_obj_set_size(o, 2, 2 * hw);
        break;
      case 1:  // down
        lv_obj_set_pos(o, tip.x - hw, tip.y - ARROW_LEN + off);
        lv_obj_set_size(o, 2 * hw, 2);
        break;
      case 2:  // left
        lv_obj_set_pos(o, tip.x + ARROW_LEN - 2 - off, tip.y - hw);
        lv_obj_set_size(o, 2, 2 * hw);
        break;
      default:  // up
        lv_obj_set_pos(o, tip.x - hw, tip.y + ARROW_LEN - 2 - off);
        lv_obj_set_size(o, 2 * hw, 2);
        break;
    }
  }
}

void FlowRenderer::add_edge_(uint8_t terminal, const Pt *corners, size_t count, int bx, int by) {
  if (terminal == INVALID_INDEX || count < 2)
    return;

  const PowerFlowStyle &s = this->pf_->style();

  Edge e;
  e.terminal = terminal;

  // Static geometry, laid out once (§7). Positions are re-derived only when
  // the line weight changes, which happens on a state transition, not a frame.
  for (size_t i = 0; i + 1 < count; i++) {
    if (corners[i].x == corners[i + 1].x && corners[i].y == corners[i + 1].y)
      continue;
    Seg sg;
    sg.obj = this->plain_(this->root_);
    lv_obj_set_style_bg_opa(sg.obj, LV_OPA_COVER, LV_PART_MAIN);
    sg.x1 = corners[i].x;
    sg.y1 = corners[i].y;
    sg.x2 = corners[i + 1].x;
    sg.y2 = corners[i + 1].y;
    place_run(sg.obj, sg.x1, sg.y1, sg.x2, sg.y2, EDGE_THICK_IDLE);
    e.segs.push_back(sg);
  }
  if (e.segs.empty())
    return;

  // The precomputed point table (§7). Sampled every PATH_STEP px along the
  // centreline, so moving a dot is one index lookup and two set_pos operands.
  int total = 0;
  for (size_t i = 0; i + 1 < count; i++) {
    const int dx = corners[i + 1].x - corners[i].x;
    const int dy = corners[i + 1].y - corners[i].y;
    const int len = std::abs(dx) + std::abs(dy);
    const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    for (int d = 0; d < len; d += PATH_STEP)
      e.path.push_back({(int16_t) (corners[i].x + sx * d), (int16_t) (corners[i].y + sy * d)});
    total += len;
  }
  e.path.push_back({corners[count - 1].x, corners[count - 1].y});

  if (total >= MIN_DOT_PATH) {
    for (auto &dot : e.dots) {
      dot = this->plain_(this->root_);
      lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Both arrowheads are precomputed; only one is ever drawn. Direction is a
  // large part of what this diagram is for, and it flips with the sign on the
  // battery leg.
  e.tip_fwd = corners[count - 1];
  e.dir_fwd = dir_of(corners[count - 1].x - corners[count - 2].x,
                     corners[count - 1].y - corners[count - 2].y);
  e.tip_rev = corners[0];
  e.dir_rev = dir_of(corners[0].x - corners[1].x, corners[0].y - corners[1].y);
  for (int k = 0; k < ARROW_STEPS; k++) {
    e.arrow[k] = this->plain_(this->root_);
    lv_obj_set_style_bg_opa(e.arrow[k], LV_OPA_COVER, LV_PART_MAIN);
  }
  this->place_arrow_(e, false);
  e.arrow_rev = 0;

  // The cross marker, drawn on an opaque disc in the page's own background
  // colour so it visibly breaks the line rather than sitting on top of it.
  // It now means one thing only: contact with this thing is lost (§6.1).
  const Pt mid = e.path[e.path.size() / 2];
  e.cross = this->plain_(this->root_);
  lv_obj_set_pos(e.cross, mid.x - CROSS_SIZE / 2, mid.y - CROSS_SIZE / 2);
  lv_obj_set_size(e.cross, CROSS_SIZE, CROSS_SIZE);
  lv_obj_set_style_radius(e.cross, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(e.cross, lv_color_hex(this->bg_color_), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(e.cross, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(e.cross, LV_OBJ_FLAG_HIDDEN);
  const int vh = line_h(s.value_font, CROSS_SIZE);
  lv_obj_t *xl = this->label_(e.cross, s.value_font, 0, (CROSS_SIZE - vh) / 2, CROSS_SIZE,
                              LV_TEXT_ALIGN_CENTER, s.dead_color, LV_OPA_COVER);
  lv_label_set_text(xl, CROSS);

  // The badge: this edge's flow figure, as a pill on the connector itself.
  // Sized on demand from the text, because "0 W" and "-1.23 kW" are not the
  // same width and a fixed pill would either clip or float.
  e.bx = (int16_t) bx;
  e.by = (int16_t) by;
  e.badge = this->plain_(this->root_);
  lv_obj_set_style_radius(e.badge, s.badge_radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(e.badge, lv_color_hex(s.badge_bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(e.badge, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_pos(e.badge, bx, by);
  lv_obj_set_size(e.badge, 2, 2);
  lv_obj_add_flag(e.badge, LV_OBJ_FLAG_HIDDEN);
  e.badge_lbl = this->label_(e.badge, s.label_font, 0, BADGE_PAD_Y, 2, LV_TEXT_ALIGN_CENTER,
                             s.badge_text, LV_OPA_COVER);

  // Does the run this badge sits on actually hold it at full font? The whole
  // point of the long elbows is that the answer is yes; say so out loud rather
  // than quietly letting a pill overhang its wire.
  const int need_w = text_width(s.label_font, WIDEST_POWER) + 2 * BADGE_PAD_X;
  const int need_h = line_h(s.label_font, 16) + 2 * BADGE_PAD_Y;
  for (const Seg &sg : e.segs) {
    const bool horiz = sg.y1 == sg.y2;
    const bool on_it =
        horiz ? (by == sg.y1 && bx >= std::min(sg.x1, sg.x2) && bx <= std::max(sg.x1, sg.x2))
              : (bx == sg.x1 && by >= std::min(sg.y1, sg.y2) && by <= std::max(sg.y1, sg.y2));
    if (!on_it)
      continue;
    const int len = horiz ? std::abs(sg.x2 - sg.x1) : std::abs(sg.y2 - sg.y1);
    const int need = horiz ? need_w : need_h;
    if (len < need)
      ESP_LOGW(TAG, "badge run is %d px, needs %d for \"%s\" at label_font", len, need,
               WIDEST_POWER);
    break;
  }

  this->edges_.push_back(std::move(e));
}

void FlowRenderer::setup(PowerFlow *pf) {
  this->pf_ = pf;
  lv_obj_t *parent = pf->parent();
  if (parent == nullptr) {
    ESP_LOGE(TAG, "no parent object — nothing drawn");
    return;
  }

  lv_obj_t *screen = lv_screen_active();
  if (screen != nullptr)
    this->bg_color_ = lv_color_to_u32(lv_obj_get_style_bg_color(screen, LV_PART_MAIN)) & 0xFFFFFFu;

  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(parent);

  this->build_(parent);
  this->update();
}

void FlowRenderer::build_(lv_obj_t *parent) {
  const PowerFlowStyle &s = this->pf_->style();
  const auto &devs = this->pf_->devices();
  const auto &terms = this->pf_->terminals();

  int W = lv_obj_get_content_width(parent);
  int H = lv_obj_get_content_height(parent);
  if (W <= 0 || H <= 0) {
    // Layout has not run yet. The style values are what the page asked for and
    // are the honest fallback; anything else would be a guess.
    W = lv_obj_get_style_width(parent, LV_PART_MAIN);
    H = lv_obj_get_style_height(parent, LV_PART_MAIN);
  }
  if (W <= 0 || H <= 0) {
    ESP_LOGE(TAG, "parent has no size (%dx%d) — nothing drawn", W, H);
    return;
  }

  this->root_ = this->plain_(parent);
  lv_obj_set_pos(this->root_, 0, 0);
  lv_obj_set_size(this->root_, W, H);

  // --- who exists -----------------------------------------------------------
  //
  // Presence is derived from the graph, never hardcoded: a node the YAML does
  // not describe is not drawn and the bands close up around it (§4).
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
  this->d_bat_ = d_bat;

  uint8_t t_in = INVALID_INDEX, t_out = INVALID_INDEX, t_bat = INVALID_INDEX, t_pv = INVALID_INDEX;
  if (d_inv != INVALID_INDEX) {
    for (uint8_t ti : devs[d_inv].terminals) {
      if (!terms[ti].enabled)
        continue;  // `enabled: false` hides the node entirely (§5) — pv, today
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

  // ONGRID LOAD is a tap off the grid, drawn only when one is configured. It
  // is not in this flat today; the mechanism is (§9 item 4).
  uint8_t t_tap = INVALID_INDEX;
  for (uint8_t i = 0; i < terms.size() && t_tap == INVALID_INDEX; i++)
    if (terms[i].enabled && terms[i].role == TerminalRole::TAP)
      t_tap = i;

  // Whatever the bus is fed from is what the trunk carries.
  uint8_t t_bus = t_out;
  for (const Device &d : devs)
    if (d.kind == DeviceKind::BUS && d.source_terminal != INVALID_INDEX)
      t_bus = d.source_terminal;

  // Consumers are the terminals that hang off no device (§4), in declaration
  // order. The one declared `power: auto` is the remainder: it belongs beside
  // the inverter output it is derived from, not at the bottom of the page.
  std::vector<uint8_t> left, right;
  uint8_t t_others = INVALID_INDEX;
  for (uint8_t i = 0; i < terms.size(); i++) {
    const Terminal &t = terms[i];
    if (t.device != INVALID_INDEX || !t.enabled)
      continue;
    if (t.is_auto && t_others == INVALID_INDEX) {
      t_others = i;
      continue;
    }
    (t.side == Side::LEFT ? left : right).push_back(i);
  }

  const bool has_grid = d_grid != INVALID_INDEX;
  const bool has_pv = t_pv != INVALID_INDEX || d_pv != INVALID_INDEX;
  const bool has_tap = t_tap != INVALID_INDEX;

  // --- widths ---------------------------------------------------------------
  //
  // One number to tune. Everything is `node_width` as given, except the grid
  // and the inverter, which are a third wider because they carry reading lines
  // as well as a name. At node_width: 150 that is 150/200; the owner's mockup
  // scales to 165/220, which is exactly node_width: 165.
  const int nw = s.node_width;
  const int w_load = nw;
  const int w_pv = nw;
  const int w_grid = std::min(W - 2 * MARGIN, nw * 4 / 3);
  const int w_inv = std::min(W - 2 * MARGIN, nw * 4 / 3);
  const int col_c = (W - w_inv) / 2;
  const int side_w = std::max(40, col_c - MARGIN - GAP);
  const int bus_x = W / 2;
  const int x_bat = col_c + w_inv + GAP;
  const int w_bat = std::max(40, W - MARGIN - x_bat);

  // --- heights --------------------------------------------------------------
  //
  // `node_height` is for boxes that carry readings. A consumer carries one
  // line — an icon and a name — and has its own, shorter key; the height that
  // saves goes straight back into the packing below.
  const int h_src = s.node_height;  // a name and one reading line
  // Tall enough for the battery's ring to hold WIDEST_SOC at value_font,
  // which is
  // the binding constraint in this band — the inverter's name plus two reading
  // lines needs less.
  const int h_conv = s.node_height * 2 + 20;
  const int h_load = s.load_height;

  const int badge_h = line_h(s.label_font, 16) + 2 * BADGE_PAD_Y;
  const int badge_w = text_width(s.label_font, WIDEST_POWER) + 2 * BADGE_PAD_X;
  const int drop = badge_h / 2 + 4;  ///< elbow to the top of the box it feeds

  const int y_src = MARGIN;
  const int y_src_b = y_src + h_src;
  const int y_verdict = y_src_b + 4;
  // Enough for the grid verdict line, the grid badge and the arrowhead.
  const int y_conv = y_verdict + line_h(s.label_font, 16) + badge_h + 18;
  const int y_conv_b = y_conv + h_conv;
  const int y_dip = y_conv_b + 22;           // the inverter→battery elbow
  const int y_ot = y_dip + badge_h / 2 + 6;  // the remainder taps the trunk here
  const int y_other = y_ot + drop;
  const int y_load_top = y_other + h_load + badge_h / 2 + 10;
  const int y_bottom = H - MARGIN;

  // --- the staggered load column -------------------------------------------
  //
  // Every consumer gets its own vertical slot, left and right interleaved at
  // half a pitch. Two things fall out of that, and both were asked for: each
  // connector can leave the trunk on a long horizontal run and turn down into
  // the top of its box — which is what seats the badge at full font, where a
  // side stub of half the length could not — and the vertical space is filled
  // by boxes instead of by empty gutters between paired rows.
  const int n_l = (int) left.size(), n_r = (int) right.size();
  // Two boxes on the same side must clear each other *and* the badge hanging
  // above the lower one.
  const int need_same = h_load + drop + badge_h / 2 + 2;
  const int hp_min = (need_same + 1) / 2;
  int mh = 0;  // the lowest slot, counted in half-pitches
  if (n_l > 0)
    mh = std::max(mh, 2 * (n_l - 1));
  if (n_r > 0)
    mh = std::max(mh, 2 * (n_r - 1) + 1);
  int hp = hp_min;
  if (mh > 0) {
    hp = (y_bottom - y_load_top - h_load) / mh;
    if (hp < hp_min) {
      ESP_LOGW(TAG, "%d loads do not fit at full font: half-pitch %d < %d needed. "
                    "Lower style.load_height, or split the two sides more evenly.",
               n_l + n_r, hp, hp_min);
      hp = hp_min;
    }
  }
  auto slot_y = [&](bool is_left, int i) {
    return y_load_top + hp * (is_left ? 2 * i : 2 * i + 1);
  };

  // A source with nothing beside it is centred rather than left hanging on one
  // edge — "the layout closes up around whatever is absent" (§4).
  const int x_grid = (has_pv || has_tap) ? MARGIN : (W - w_grid) / 2;
  const int x_pv = W - MARGIN - w_pv;
  const int x_tap = MARGIN;

  // --- fonts, one per group -------------------------------------------------
  const int pad = std::max(6, (int) s.radius / 2);
  const int ico = line_h(s.icon_font, 26) + 6;
  auto avail = [&](int w) { return w - pad - ico - pad; };
  std::vector<NameFit> spine, loads;
  if (has_grid) spine.emplace_back(devs[d_grid].name, avail(w_grid));
  if (d_inv != INVALID_INDEX) spine.emplace_back(devs[d_inv].name, avail(w_inv));
  if (d_bat != INVALID_INDEX) spine.emplace_back(devs[d_bat].name, avail(w_bat));
  if (has_pv)
    spine.emplace_back(t_pv != INVALID_INDEX ? terms[t_pv].name : devs[d_pv].name, avail(w_pv));
  for (uint8_t i : left) loads.emplace_back(terms[i].name, avail(w_load));
  for (uint8_t i : right) loads.emplace_back(terms[i].name, avail(w_load));
  if (t_others != INVALID_INDEX) loads.emplace_back(terms[t_others].name, avail(w_load));
  if (has_tap) loads.emplace_back(terms[t_tap].name, avail(side_w));
  const lv_font_t *f_spine = this->fit_font_(spine);
  const lv_font_t *f_loads = this->fit_font_(loads);

  // --- band 1: sources ------------------------------------------------------
  if (has_grid)
    this->add_node_(Slot::GRID, d_grid, t_in, x_grid, y_src, w_grid, h_src, f_spine, 1);
  if (has_pv)
    this->add_node_(Slot::PV, d_pv, t_pv, x_pv, y_src, w_pv, h_src, f_spine, 0);

  // The grid verdict, under the grid box and only when the inference distrusts
  // the reading. Nothing announces that everything is fine.
  if (has_grid)
    this->grid_verdict_ = this->label_(this->root_, s.label_font, x_grid, y_verdict, w_grid,
                                       LV_TEXT_ALIGN_CENTER, s.warn_color, LV_OPA_COVER);

  // --- band 2: conversion ---------------------------------------------------
  if (has_tap)
    this->add_node_(Slot::ONGRID, d_grid, t_tap, x_tap, y_conv + (h_conv - h_load) / 2, side_w,
                    h_load, f_loads, 0);
  if (d_inv != INVALID_INDEX)
    this->add_node_(Slot::INVERTER, d_inv, INVALID_INDEX, col_c, y_conv, w_inv, h_conv, f_spine, 2);
  if (d_bat != INVALID_INDEX)
    this->add_node_(Slot::BATTERY, d_bat, t_bat, x_bat, y_conv, w_bat, h_conv, f_spine, 0);

  // --- band 3: the remainder ------------------------------------------------
  if (t_others != INVALID_INDEX)
    this->add_node_(Slot::OTHERS, INVALID_INDEX, t_others, MARGIN, y_other, w_load, h_load,
                    f_loads, 0);

  // --- band 5: the loads ----------------------------------------------------
  std::vector<int> left_y, right_y;
  for (int i = 0; i < n_l; i++) {
    const int by = slot_y(true, i);
    left_y.push_back(by);
    this->add_node_(Slot::CONSUMER, INVALID_INDEX, left[i], MARGIN, by, w_load, h_load, f_loads, 0);
  }
  for (int i = 0; i < n_r; i++) {
    const int by = slot_y(false, i);
    right_y.push_back(by);
    this->add_node_(Slot::CONSUMER, INVALID_INDEX, right[i], W - MARGIN - w_load, by, w_load,
                    h_load, f_loads, 0);
  }

  // --- connectors, each with its badge --------------------------------------
  //
  // Every path runs in the direction of *positive* flow, so a negative value
  // is simply the same table walked backwards and the arrowhead moved to the
  // other end.
  Pt c[4];

  // Grid fans out: down-left to the on-grid tap, down-right into the inverter.
  if (has_grid && has_tap) {
    const int gx = std::min(x_grid + w_grid / 4, x_tap + side_w / 2);
    const int ty = y_conv + (h_conv - h_load) / 2;
    c[0] = {(int16_t) gx, (int16_t) y_src_b};
    c[1] = {(int16_t) gx, (int16_t) (ty - drop)};
    c[2] = {(int16_t) (x_tap + side_w / 2), (int16_t) (ty - drop)};
    c[3] = {(int16_t) (x_tap + side_w / 2), (int16_t) ty};
    this->add_edge_(t_tap, c, 4, (gx + x_tap + side_w / 2) / 2, ty - drop);
  }
  if (has_grid && t_in != INVALID_INDEX) {
    const int gx = x_grid + w_grid / 2;
    // Straight down when the grid already stands over the inverter — which is
    // what happens once PV and the on-grid tap are both absent and band 1
    // closes up. An S-bend between two centred boxes reads as a mistake.
    const int ix = (gx >= col_c + 8 && gx <= col_c + w_inv - 8) ? gx : col_c + w_inv / 4;
    if (gx == ix) {
      c[0] = {(int16_t) gx, (int16_t) y_src_b};
      c[1] = {(int16_t) gx, (int16_t) y_conv};
      this->add_edge_(t_in, c, 2, gx, y_conv - badge_h / 2 - ARROW_LEN - 6);
    } else {
      const int my = (y_src_b + y_conv) / 2;
      c[0] = {(int16_t) gx, (int16_t) y_src_b};
      c[1] = {(int16_t) gx, (int16_t) my};
      c[2] = {(int16_t) ix, (int16_t) my};
      c[3] = {(int16_t) ix, (int16_t) y_conv};
      this->add_edge_(t_in, c, 4, (gx + ix) / 2, my);
    }
  }
  // PV drops down and turns into the inverter.
  if (t_pv != INVALID_INDEX) {
    const int px = x_pv + w_pv / 2;
    const int ix = col_c + 3 * w_inv / 4;
    const int my = (y_src_b + y_conv) / 2;
    c[0] = {(int16_t) px, (int16_t) y_src_b};
    c[1] = {(int16_t) px, (int16_t) my};
    c[2] = {(int16_t) ix, (int16_t) my};
    c[3] = {(int16_t) ix, (int16_t) y_conv};
    this->add_edge_(t_pv, c, 4, (px + ix) / 2, my);
  }

  // Inverter → battery: out of the bottom-right, right, and up into the
  // battery. Positive is a charge, so the path runs towards the battery. The
  // exit is far enough right that the dip never collides with the badge on the
  // trunk, which leaves from the same edge.
  if (d_bat != INVALID_INDEX && t_bat != INVALID_INDEX && d_inv != INVALID_INDEX) {
    const int ox = col_c + w_inv - std::max(30, w_inv / 5);
    const int bxc = x_bat + w_bat / 2;
    c[0] = {(int16_t) ox, (int16_t) y_conv_b};
    c[1] = {(int16_t) ox, (int16_t) y_dip};
    c[2] = {(int16_t) bxc, (int16_t) y_dip};
    c[3] = {(int16_t) bxc, (int16_t) y_conv_b};
    this->add_edge_(t_bat, c, 4, (ox + bxc) / 2, y_dip);
  }

  // --- band 4: the trunk, and everything that taps it ----------------------
  std::vector<int> taps;
  if (t_others != INVALID_INDEX)
    taps.push_back(y_ot);
  for (int y : left_y)
    taps.push_back(y - drop);
  for (int y : right_y)
    taps.push_back(y - drop);
  std::sort(taps.begin(), taps.end());

  int trunk_end = y_conv_b;
  for (int y : taps)
    trunk_end = std::max(trunk_end, y);
  // The trunk's own badge goes in the largest gap between consecutive taps, so
  // it never lands on top of one of theirs.
  int tb_y = (y_conv_b + trunk_end) / 2, best = -1, prev = y_conv_b;
  for (int y : taps) {
    if (y - prev > best) {
      best = y - prev;
      tb_y = (prev + y) / 2;
    }
    prev = y;
  }
  if (t_bus != INVALID_INDEX && trunk_end > y_conv_b) {
    c[0] = {(int16_t) bus_x, (int16_t) y_conv_b};
    c[1] = {(int16_t) bus_x, (int16_t) trunk_end};
    this->add_edge_(t_bus, c, 2, bus_x, tb_y);
  }

  // Long horizontal run out of the trunk, then a short drop into the top of
  // the box. The run is what carries the badge; the drop is only as deep as
  // the badge needs to clear the box below it.
  auto tap_edge = [&](uint8_t terminal, bool is_left, int by) {
    const int cx = is_left ? (MARGIN + w_load / 2) : (W - MARGIN - w_load / 2);
    c[0] = {(int16_t) bus_x, (int16_t) (by - drop)};
    c[1] = {(int16_t) cx, (int16_t) (by - drop)};
    c[2] = {(int16_t) cx, (int16_t) by};
    this->add_edge_(terminal, c, 3, (bus_x + cx) / 2, by - drop);
  };
  if (t_others != INVALID_INDEX)
    tap_edge(t_others, true, y_other);
  for (int i = 0; i < n_l; i++)
    tap_edge(left[i], true, left_y[i]);
  for (int i = 0; i < n_r; i++)
    tap_edge(right[i], false, right_y[i]);

  // --- the "this diagram is lying to you" pill ------------------------------
  //
  // §4 requires an implausible balance to be flagged. It straddles the top of
  // the inverter box, dead centre of the page, and exists only while the flag
  // is set — nothing announces that everything is fine.
  {
    const int fh = line_h(s.value_font, 28) + 10;
    const int fw = text_width(s.value_font, "UNRELIABLE") + 28;
    this->unreliable_ = this->plain_(this->root_);
    lv_obj_set_pos(this->unreliable_, (W - fw) / 2, y_conv - fh / 2);
    lv_obj_set_size(this->unreliable_, fw, fh);
    lv_obj_set_style_radius(this->unreliable_, s.badge_radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(this->unreliable_, lv_color_hex(s.warn_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(this->unreliable_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(this->unreliable_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fl = this->label_(this->unreliable_, s.value_font, 0, 5, fw, LV_TEXT_ALIGN_CENTER,
                                s.node_bg, LV_OPA_COVER);
    lv_label_set_text(fl, "UNRELIABLE");
  }

  // --- the "Home Assistant is gone" banner (§6.2) ---------------------------
  //
  // A sibling of root_, not a child, so dimming the diagram does not dim the
  // one thing that explains why it is dim.
  const int bw = W - 40, bh = 92;
  this->banner_ = this->plain_(parent);
  lv_obj_set_pos(this->banner_, 20, (H - bh) / 2);
  lv_obj_set_size(this->banner_, bw, bh);
  lv_obj_set_style_radius(this->banner_, s.radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(this->banner_, lv_color_hex(this->bg_color_), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(this->banner_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(this->banner_, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(this->banner_, lv_color_hex(s.warn_color), LV_PART_MAIN);
  lv_obj_add_flag(this->banner_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *b1 = this->label_(this->banner_, s.value_font, 8, 16, bw - 16, LV_TEXT_ALIGN_CENTER,
                              s.warn_color, LV_OPA_COVER);
  lv_label_set_text(b1, "NO LINK TO HOME ASSISTANT");
  lv_obj_t *b2 = this->label_(this->banner_, s.label_font, 8, 56, bw - 16, LV_TEXT_ALIGN_CENTER,
                              s.text_color, OPA_CAPTION);
  lv_label_set_text(b2, "values are held; the state of the mains is unknown");

  // --- stacking -------------------------------------------------------------
  //
  // Creation order is per-edge, so a connector built late would otherwise
  // cover a dot or a badge built early. One explicit pass fixes the order:
  // segments, dots, arrowheads, boxes, crosses, badges, then the two flags.
  for (Edge &e : this->edges_)
    for (lv_obj_t *d : e.dots)
      if (d != nullptr)
        lv_obj_move_foreground(d);
  for (Edge &e : this->edges_)
    for (lv_obj_t *a : e.arrow)
      if (a != nullptr)
        lv_obj_move_foreground(a);
  for (Node &n : this->nodes_)
    lv_obj_move_foreground(n.box);
  for (Edge &e : this->edges_)
    lv_obj_move_foreground(e.cross);
  for (Edge &e : this->edges_)
    lv_obj_move_foreground(e.badge);
  if (this->grid_verdict_ != nullptr)
    lv_obj_move_foreground(this->grid_verdict_);
  lv_obj_move_foreground(this->unreliable_);

  // --- what got drawn, and how much of the glass it covers -----------------
  //
  // Deliberately INFO and not CONFIG: the panel logs at INFO, nobody can see
  // the screen from here, and these boot lines are the only way to check the
  // layout arithmetic against what the owner is actually looking at.
  lv_obj_update_layout(this->root_);
  long ink = 0;
  std::vector<std::pair<int, int>> spans;
  for (const Node &n : this->nodes_) {
    const int bw2 = lv_obj_get_width(n.box), bh2 = lv_obj_get_height(n.box);
    ink += (long) bw2 * bh2;
    spans.emplace_back((int) lv_obj_get_y(n.box), (int) lv_obj_get_y(n.box) + bh2);
  }
  for (const Edge &e : this->edges_) {
    ink += (long) badge_w * badge_h;
    for (const Seg &sg : e.segs)
      ink += (long) (std::abs(sg.x2 - sg.x1) + std::abs(sg.y2 - sg.y1) + 2) * EDGE_THICK_IDLE;
  }
  std::sort(spans.begin(), spans.end());
  int gap_max = 0, gap_at = 0, reach = 0;
  for (const auto &sp : spans) {
    if (sp.first - reach > gap_max) {
      gap_max = sp.first - reach;
      gap_at = reach;
    }
    reach = std::max(reach, sp.second);
  }
  if (H - reach > gap_max) {
    gap_max = H - reach;
    gap_at = reach;
  }

  ESP_LOGI(TAG, "%dx%d: %u nodes, %u edges  (fonts: spine %s, loads %s)", W, H,
           (unsigned) this->nodes_.size(), (unsigned) this->edges_.size(),
           f_spine == s.value_font ? "value" : "label",
           f_loads == s.value_font ? "value" : "label");
  ESP_LOGI(TAG, "bands: src y=%d h=%d  conv y=%d h=%d  other y=%d h=%d  loads y=%d half-pitch=%d"
                "  trunk x=%d %d..%d",
           y_src, h_src, y_conv, h_conv, y_other, h_load, y_load_top, hp, bus_x, y_conv_b,
           trunk_end);
  ESP_LOGI(TAG, "badge \"%s\" is %dx%d at label_font = minimum run; elbow drop %d; "
                "same-side pitch needs %d, has %d",
           WIDEST_POWER, badge_w, badge_h, drop, need_same, 2 * hp);
  ESP_LOGI(TAG, "coverage %ld of %ld px = %ld%%; largest empty band %d px at y=%d", ink,
           (long) W * H, ink * 100 / ((long) W * H), gap_max, gap_at);
  // Every codepoint, found or not. The count alone was ambiguous: a node built
  // without an icon looks exactly like a glyph the font does not carry.
  for (uint8_t sl = 0; sl <= (uint8_t) Slot::OTHERS; sl++) {
    const uint32_t cp = icon_for_slot(sl);
    ESP_LOGI(TAG, "  icon %-6s U+%05X %s", SLOT_NAME[sl], (unsigned) cp,
             this->has_glyph_(s.icon_font, cp) ? "ok" : "MISSING FROM f_icons");
  }
  for (const Node &n : this->nodes_) {
    const Terminal *t = this->term_(n.terminal);
    ESP_LOGI(TAG, "  %-6s %3d,%-3d %3dx%-3d  %s", SLOT_NAME[(uint8_t) n.slot],
             (int) lv_obj_get_x(n.box), (int) lv_obj_get_y(n.box), (int) lv_obj_get_width(n.box),
             (int) lv_obj_get_height(n.box), t != nullptr ? t->name.c_str() : "-");
  }
  for (const Edge &e : this->edges_) {
    const Terminal *t = this->term_(e.terminal);
    ESP_LOGI(TAG, "  edge %-14s %u runs, %u pts, badge %d,%d, %s",
             t != nullptr ? t->name.c_str() : "-", (unsigned) e.segs.size(),
             (unsigned) e.path.size(), (int) e.bx, (int) e.by,
             e.dots[0] != nullptr ? "dots" : "no dots");
  }
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

/// §6.1's five states in one switch. When Home Assistant is unreachable every
/// meter reads NaN at once, so every edge would otherwise be DE_ENERGIZED and
/// the whole diagram would claim the power is out. It is not: the answer is
/// "we do not know", which is what NO_DATA looks like (§6.2).
///
/// OPEN and DE_ENERGIZED are two different greys on purpose. They used to be
/// one, and on glass that read as "the boiler is broken" when the owner had
/// simply switched it off (§6.1, amended 2026-08-23).
uint32_t FlowRenderer::state_color_(const Terminal *t) const {
  const PowerFlowStyle &s = this->pf_->style();
  if (t == nullptr || !this->ha_contact_)
    return s.line_color;
  switch (t->state) {
    case EdgeState::ACTIVE: return t->stale ? s.warn_color : s.active_color;
    case EdgeState::OPEN: return s.off_color;
    case EdgeState::DE_ENERGIZED: return s.dead_color;
    default: return s.line_color;  // IDLE, NO_DATA — the wire is there, quiet
  }
}

/// The spine says "OFFLINE" instead of vanishing. For the inverter that means
/// neither of its two AC meters is talking; a single dead meter is a dead edge,
/// not a dead inverter.
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
/// still has to say why the ring is empty (§6.3).
bool FlowRenderer::battery_offline_() const {
  if (!this->ha_contact_)
    return false;
  const Device *d = this->dev_(this->d_bat_);
  const bool soc_ok = d != nullptr && d->soc != nullptr && is_valid(d->soc->state);
  if (soc_ok)
    return false;
  const Terminal *t = this->term_(this->t_bat_);
  return t == nullptr || t->state == EdgeState::DE_ENERGIZED || t->state == EdgeState::NO_DATA;
}

void FlowRenderer::set_badge_(Edge &e, const std::string &txt, uint32_t color) {
  if (e.badge == nullptr)
    return;
  if (txt.empty()) {
    if (!e.badge_hidden) {
      e.badge_hidden = true;
      set_hidden(e.badge, true);
    }
    return;
  }
  if (e.badge_hidden) {
    e.badge_hidden = false;
    set_hidden(e.badge, false);
  }
  if (txt != e.txt_badge) {
    e.txt_badge = txt;
    const lv_font_t *f = this->pf_->style().label_font;
    const int bw = text_width(f, txt.c_str()) + 2 * BADGE_PAD_X;
    const int bh = line_h(f, 16) + 2 * BADGE_PAD_Y;
    lv_obj_set_size(e.badge, bw, bh);
    lv_obj_set_pos(e.badge, e.bx - bw / 2, e.by - bh / 2);
    lv_obj_set_width(e.badge_lbl, bw);
    lv_label_set_text(e.badge_lbl, txt.c_str());
  }
  if (color != e.badge_col) {
    e.badge_col = color;
    lv_obj_set_style_text_color(e.badge_lbl, lv_color_hex(color), LV_PART_MAIN);
  }
}

void FlowRenderer::update_edge_(Edge &e) {
  const PowerFlowStyle &s = this->pf_->style();
  const Terminal *t = this->term_(e.terminal);
  const bool ha = this->ha_contact_;

  const EdgeState st = (t == nullptr) ? EdgeState::NO_DATA : t->state;
  const bool active = ha && st == EdgeState::ACTIVE;
  const bool open = ha && st == EdgeState::OPEN;
  // The ✕ now means one thing only: contact with this thing is lost. An open
  // relay that is online and reporting a true zero is behaving exactly as
  // intended and earns a caption, not a fault marker (§6.1).
  const bool cross = ha && st == EdgeState::DE_ENERGIZED;

  const uint32_t col = this->state_color_(t);
  const int16_t thick = active ? EDGE_THICK_ACTIVE : (open ? EDGE_THICK_OFF : EDGE_THICK_IDLE);
  lv_opa_t opa = LV_OPA_COVER;
  if (t != nullptr && t->stale && ha)
    opa = OPA_STALE;
  else if (ha && (st == EdgeState::IDLE || st == EdgeState::NO_DATA))
    opa = OPA_LINE_IDLE;  // present, but nothing is happening on it

  if (thick != e.thick) {
    e.thick = thick;
    for (Seg &sg : e.segs)
      place_run(sg.obj, sg.x1, sg.y1, sg.x2, sg.y2, thick);
  }
  if (col != e.color) {
    e.color = col;
    const lv_color_t c = lv_color_hex(col);
    for (Seg &sg : e.segs)
      lv_obj_set_style_bg_color(sg.obj, c, LV_PART_MAIN);
    for (lv_obj_t *d : e.dots)
      if (d != nullptr)
        lv_obj_set_style_bg_color(d, c, LV_PART_MAIN);
    for (lv_obj_t *a : e.arrow)
      if (a != nullptr)
        lv_obj_set_style_bg_color(a, c, LV_PART_MAIN);
  }
  if (opa != e.opa) {
    e.opa = opa;
    for (Seg &sg : e.segs)
      lv_obj_set_style_opa(sg.obj, opa, LV_PART_MAIN);
    for (lv_obj_t *a : e.arrow)
      if (a != nullptr)
        lv_obj_set_style_opa(a, opa, LV_PART_MAIN);
  }
  if (cross != e.show_cross) {
    e.show_cross = cross;
    set_hidden(e.cross, !cross);
  }

  // ---- the badge: this edge's flow figure, and the only place it appears.
  const float v = (t != nullptr) ? t->value : NAN;
  std::string btxt;
  uint32_t bcol = s.badge_text;
  if (!ha) {
    btxt = DASH;
  } else if (open) {
    btxt = "OFF";       // a caption, not a number and not a marker
    bcol = s.off_color;
  } else if (st == EdgeState::DE_ENERGIZED) {
    btxt.clear();       // the ✕ is the statement; a figure beside it would lie
  } else if (st == EdgeState::NO_DATA) {
    btxt = DASH;
  } else if (t != nullptr && t->bidirectional) {
    btxt = fmt_signed_power(v);  // the battery: the sign is the direction
  } else {
    btxt = fmt_power(v);
  }
  if (ha && t != nullptr && t->stale && !btxt.empty() && !open)
    bcol = s.warn_color;
  this->set_badge_(e, btxt, bcol);

  // ---- the arrowhead follows the sign, not the table
  const bool rev = ha && is_valid(v) && v < 0.0f;
  if ((int8_t) rev != e.arrow_rev) {
    e.arrow_rev = (int8_t) rev;
    this->place_arrow_(e, rev);
  }

  const bool run = active && is_valid(v) && e.dots[0] != nullptr;
  if (run != e.animate) {
    e.animate = run;
    for (lv_obj_t *d : e.dots)
      set_hidden(d, !run);
  }
  if (run) {
    e.reverse = v < 0.0f;
    const float mag = std::fabs(v);
    const float k = std::sqrt(std::min(1.0f, mag / DOT_SPEED_REF));
    const float px_s = DOT_SPEED_MIN + (DOT_SPEED_MAX - DOT_SPEED_MIN) * k;
    e.speed = px_s / (float) PATH_STEP;  // table indices per second
  }
}

void FlowRenderer::update_node_(Node &n) {
  const PowerFlowStyle &s = this->pf_->style();
  const Diagnostics &dg = this->pf_->diagnostics();
  const Terminal *t = this->term_(n.terminal);
  const Device *d = this->dev_(n.device);
  const bool ha = this->ha_contact_;

  const EdgeState st = (t == nullptr) ? EdgeState::NO_DATA : t->state;
  const bool dead = ha && st == EdgeState::DE_ENERGIZED;
  const bool stale = ha && t != nullptr && t->stale;

  // ---- name: the owner's own, from the config. Nothing else belongs on this
  // line — the flow figure is on the connector now.
  std::string name;
  if (n.slot == Slot::CONSUMER || n.slot == Slot::OTHERS || n.slot == Slot::ONGRID)
    name = (t != nullptr) ? t->name : "";
  else if (d != nullptr)
    name = d->name;
  else if (t != nullptr)
    name = t->name;
  set_text(n.name, n.txt_name, name);

  // ---- the device-level readings, and what replaces them when they are gone
  std::string r1, r2;
  const std::string r3;  // no third reading line on any box today
  uint32_t r1col = s.text_color;
  switch (n.slot) {
    case Slot::GRID:
      if (d != nullptr && d->voltage != nullptr) {
        const float v = d->voltage->state;
        char buf[16];
        if (is_valid(v))
          snprintf(buf, sizeof(buf), "%.0f V", v);
        else
          snprintf(buf, sizeof(buf), "%s V", DASH);
        r1 = buf;
      }
      break;

    case Slot::INVERTER: {
      // Loss, efficiency and where the house is being fed from. Which of the
      // first two is meaningful follows EnergyReading's own flags, so a gate
      // firing for one cycle turns a number into a dash rather than moving the
      // layout; `figure: both` shows them together.
      if (this->inverter_offline_()) {
        r1 = "OFFLINE";
        r1col = s.dead_color;
        break;
      }
      const EnergyReading &er = dg.energy;
      if (ha && er.show_losses)
        r1 = std::string("Loss ") + fmt_power(er.losses);
      if (ha && er.show_efficiency) {
        char buf[24];
        if (is_valid(er.efficiency))
          snprintf(buf, sizeof(buf), "Eff %.0f %%", er.efficiency * 100.0f);
        else
          snprintf(buf, sizeof(buf), "Eff %s", DASH);
        if (r1.empty())
          r1 = buf;
        else
          r2 = buf;
      }
      if (!ha && r1.empty())
        r1 = DASH;
      // No supply-mode line. Which connector is lit and which way its dots run
      // already says whether the house is on the grid or on the battery;
      // spending the box's fourth line to repeat the animation is the same
      // waste the bottom strip was. SupplyMode stays computed and unrendered,
      // like runtime_hours.
      break;
    }

    case Slot::BATTERY:
      if (this->battery_offline_()) {
        r1 = "OFFLINE";
        r1col = s.dead_color;
      }
      break;

    default:
      // A de-energized load says nothing: it goes pale and its connector
      // carries the ✕, which is the same statement without a second line
      // (§6.2, amended after OFFLINE cost BOLIVAR a line for no information).
      break;
  }
  set_text(n.r1, n.txt_r1, r1);
  set_text(n.r2, n.txt_r2, r2);
  set_text(n.r3, n.txt_r3, r3);
  if (r1col != n.col_r1 && n.r1 != nullptr) {
    n.col_r1 = r1col;
    lv_obj_set_style_text_color(n.r1, lv_color_hex(r1col), LV_PART_MAIN);
  }

  // ---- the state-of-charge ring: the one level on a diagram of flows
  if (n.slot == Slot::BATTERY) {
    const bool off = !r1.empty();
    const float soc = (d != nullptr && d->soc != nullptr) ? d->soc->state : NAN;
    if (off != n.ring_off) {
      n.ring_off = off;
      set_hidden(n.ring, off);
      set_hidden(n.ring_lbl, off);
    }
    if (!off) {
      char buf[12];
      if (is_valid(soc))
        snprintf(buf, sizeof(buf), "%.0f%%", soc);
      else
        snprintf(buf, sizeof(buf), "%s", DASH);
      set_text(n.ring_lbl, n.txt_ring, buf);
      const int32_t pct =
          is_valid(soc) ? (int32_t) lroundf(std::min(100.0f, std::max(0.0f, soc))) : 0;
      if (pct != n.ring_pct) {
        n.ring_pct = pct;
#if LV_USE_ARC
        if (n.ring != nullptr)
          lv_arc_set_value(n.ring, pct);
#endif
      }
      const uint32_t rc = !ha ? s.line_color : (is_valid(soc) ? s.active_color : s.dead_color);
      if (rc != n.col_ring) {
        n.col_ring = rc;
#if LV_USE_ARC
        if (n.ring != nullptr)
          lv_obj_set_style_arc_color(n.ring, lv_color_hex(rc), LV_PART_INDICATOR);
#endif
      }
    }
  }

  // ---- colours. The border carries the edge state; the fill only ever
  // changes to say "this thing is not there" (§6.2).
  uint32_t border = s.node_border;
  uint32_t bg = s.node_bg;
  if (!ha) {
    border = s.node_border;
  } else if (n.slot == Slot::INVERTER) {
    if (this->inverter_offline_()) {
      border = s.dead_color;
      bg = s.dead_color;
    } else if (!dg.reliable) {
      border = s.warn_color;
    } else if (dg.supply != SupplyMode::UNKNOWN) {
      border = s.active_color;
    }
  } else if (n.slot == Slot::BATTERY && this->battery_offline_()) {
    border = s.dead_color;
    bg = s.dead_color;
  } else if (t != nullptr) {
    border = this->state_color_(t);
    if (dead)
      bg = s.dead_color;  // "pale fill" (§6.2) — an open relay does not get one
  }
  if (border != n.col_border) {
    n.col_border = border;
    lv_obj_set_style_border_color(n.box, lv_color_hex(border), LV_PART_MAIN);
  }
  if (bg != n.col_bg) {
    n.col_bg = bg;
    lv_obj_set_style_bg_color(n.box, lv_color_hex(bg), LV_PART_MAIN);
  }
  const lv_opa_t opa = stale ? OPA_STALE : (lv_opa_t) LV_OPA_COVER;
  if (opa != n.opa) {
    n.opa = opa;
    lv_obj_set_style_opa(n.box, opa, LV_PART_MAIN);
  }
}

/// The two things that appear only when something is wrong.
void FlowRenderer::update_flags_() {
  const Diagnostics &dg = this->pf_->diagnostics();

  const bool bad = this->ha_contact_ && dg.grid != GridVerdict::TRUSTED;
  set_text(this->grid_verdict_, this->txt_verdict_,
           bad ? std::string("GRID ") + grid_word(dg.grid) : std::string());
  this->flag_verdict_ = bad;

  const bool unrel = this->ha_contact_ && !dg.reliable;
  if (unrel != this->flag_unreliable_) {
    this->flag_unreliable_ = unrel;
    set_hidden(this->unreliable_, !unrel);
  }
}

void FlowRenderer::update() {
  if (this->pf_ == nullptr || this->root_ == nullptr)
    return;

  const Diagnostics &dg = this->pf_->diagnostics();
  this->ha_contact_ = dg.ha_contact;

  // §6.2. Losing Home Assistant is not a blackout: dim everything, say so, and
  // draw no ✕ anywhere. A cross in this state would tell the owner the power
  // is out when it is not.
  const bool dim = !this->ha_contact_;
  if (dim != this->dimmed_) {
    this->dimmed_ = dim;
    lv_obj_set_style_opa(this->root_, dim ? OPA_NO_HA : (lv_opa_t) LV_OPA_COVER, LV_PART_MAIN);
    set_hidden(this->banner_, !dim);
  }

  for (Node &n : this->nodes_)
    this->update_node_(n);
  for (Edge &e : this->edges_)
    this->update_edge_(e);
  this->update_flags_();
}

// ---------------------------------------------------------------------------
// Animation
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

  // A long stall (a blocking component, a page change) must not teleport the
  // dots the length of the bus.
  const float secs = std::min<uint32_t>(dt, 250) / 1000.0f;

  for (Edge &e : this->edges_) {
    if (!e.animate || e.path.empty())
      continue;
    const int n = (int) e.path.size();
    e.phase += e.speed * secs;
    if (e.phase >= (float) n)
      e.phase -= (float) n * std::floor(e.phase / (float) n);
    const int base = (int) e.phase;
    for (int k = 0; k < 3; k++) {
      lv_obj_t *dot = e.dots[k];
      if (dot == nullptr)
        continue;
      int i = (base + k * n / 3) % n;
      if (e.reverse)
        i = n - 1 - i;
      const Pt &p = e.path[i];
      lv_obj_set_pos(dot, p.x - DOT_SIZE / 2, p.y - DOT_SIZE / 2);
    }
  }
}

// ---------------------------------------------------------------------------
// The tap
// ---------------------------------------------------------------------------

void FlowRenderer::node_event_cb_(lv_event_t *e) {
  auto *self = static_cast<FlowRenderer *>(lv_event_get_user_data(e));
  lv_obj_t *obj = lv_event_get_target_obj(e);
  if (self == nullptr || obj == nullptr)
    return;
  self->on_node_clicked_((uint8_t) (uintptr_t) lv_obj_get_user_data(obj));
}

/// Resolve the node to a device and fire whatever the YAML bound to it. The
/// component still knows nothing about pages (§7). A consumer belongs to no
/// device and therefore has nothing to fire — that is not an error, it is a
/// socket with no page behind it.
void FlowRenderer::on_node_clicked_(uint8_t node) {
  if (node >= this->nodes_.size())
    return;
  const Node &n = this->nodes_[node];
  const Terminal *t = this->term_(n.terminal);

  uint8_t di = n.device;
  if (di == INVALID_INDEX && t != nullptr)
    di = t->device;
  const Device *d = this->dev_(di);
  if (d == nullptr) {
    ESP_LOGD(TAG, "tap on %s (%s): no device behind it", SLOT_NAME[(uint8_t) n.slot],
             t != nullptr ? t->name.c_str() : "-");
    return;
  }
  if (d->on_click == nullptr) {
    ESP_LOGD(TAG, "tap on %s: no on_click bound", d->id.c_str());
    return;
  }
  ESP_LOGI(TAG, "tap on %s — firing on_click", d->id.c_str());
  d->on_click->trigger();
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
