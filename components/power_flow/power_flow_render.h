#pragma once
//
// power_flow — the drawing half.
//
// Kept in its own translation unit so the arithmetic and the LVGL calls never
// share a file. Everything here reads the component's resolved state and writes
// pixels; nothing here decides what a number means.
//
// The component owns one of these and calls, in this order:
//
//   setup(pf)     once, from PowerFlow::setup(), after the graph is built
//   update()      whenever the values have been re-resolved (~4 Hz)
//   frame(now)    every main-loop pass, for the dot animation (~25 fps)
//
// ---------------------------------------------------------------------------
// This file is a transcription of DEV/UI/POWER_FLOW_UI_SPEC.md. Every
// coordinate in the .cpp is that document's, 1x and absolute, relative to
// `pf_->parent()`, origin top-left. Nothing is centred by the layout engine and
// nothing is derived from the parent's measured size: the panel is 480 x 800
// and the spec is written against that. Where a number here disagrees with the
// spec, the spec is right and this file is wrong.
//
// The palette is twenty-six named constants at the top of the .cpp, transcribed
// from spec §2. It is deliberately not configurable: there is a document that
// specifies every colour, and a YAML key per colour would be twenty-six ways to
// contradict it.
//
// Three layers, and the split is a redraw-cost decision:
//
//   connectors  static rectangles, positioned once at setup (spec §5)
//   nodes       obj + labels, so LVGL invalidates only the box that changed
//   dots        small objs stepped along one straight run (spec §7)
//
// §5 suggests a canvas for the connectors. This firmware does not have one:
// ESPHome emits LV_USE_CANVAS 0 and LV_USE_LINE 0 into lv_conf.h because the
// page declares neither widget, and the page is not ours to edit. It costs
// nothing — every connector in the spec is axis-aligned, so a thin `obj` is
// both cheaper than an lv_line and exactly as static. The same constraint is
// why an arrowhead is a four-rectangle staircase and not a polygon. LV_USE_ARC
// *is* 1 — the battery page declares an `arc` — which is what makes the
// state-of-charge ring possible where a canvas is not.
//
#include "power_flow.h"

#ifdef USE_LVGL

#include <initializer_list>
#include <string>
#include <vector>

namespace esphome {
namespace power_flow {

class FlowRenderer {
 public:
  /// Build the whole widget tree under `pf->parent()` with the raw LVGL C API.
  /// §8 of the task doc is explicit that this component must not register an
  /// LVGL widget type; it parents plain objects to an `obj` the user declared.
  void setup(PowerFlow *pf);

  /// Push resolved values, edge states and diagnostics into the widgets. Only
  /// what changed is touched — LVGL invalidates per object, and that is the
  /// whole reason nodes are objects rather than canvas paint.
  void update();

  /// Advance the flow dots. Called every pass, so it must return immediately
  /// when nothing is due.
  void frame(uint32_t now);

 protected:
  /// Four rectangles make one 10 x 8 arrowhead. A polygon would need the canvas
  /// this build does not have, and a rotated object would need transform
  /// support that is not guaranteed to be compiled in.
  static const int HEAD_STEPS = 4;

  /// An absolute rectangle, in the spec's own coordinates.
  struct Rect {
    int16_t x{0}, y{0}, w{0}, h{0};
  };

  /// Which element of the drawing this box is. Kept as an enum rather than
  /// inferred from the device kind so the click handler and the value
  /// formatter can tell a battery from a socket without a string compare.
  enum class Slot : uint8_t { GRID, TAP, PV, OTHER, INVERTER, BATTERY, CONSUMER };

  /// One object whose y is a function of its consumer's row (§4.3: entries that
  /// are off or no-data sink to the last rows). `base` is the y it would have
  /// at row 0; the row multiplies the 114 px pitch on top of it.
  struct Movable {
    lv_obj_t *obj{nullptr};
    int16_t base{0};
  };

  /// A drawn box. `terminal` is the edge it sits at the end of; it is used to
  /// colour the box and to resolve a tap, never to print a number — every flow
  /// figure is a badge on a connector (README, non-negotiable 1).
  struct Node {
    Slot slot{Slot::CONSUMER};
    uint8_t device{INVALID_INDEX};
    uint8_t terminal{INVALID_INDEX};

    lv_obj_t *box{nullptr};
    lv_obj_t *icon{nullptr};
    lv_obj_t *name{nullptr};
    lv_obj_t *sub{nullptr};  ///< voltage / "Load" / the PV dash

    // Inverter only.
    lv_obj_t *rule{nullptr};
    lv_obj_t *cap_loss{nullptr}, *cap_eff{nullptr};
    lv_obj_t *val_loss{nullptr}, *unit_loss{nullptr};
    lv_obj_t *val_eff{nullptr}, *unit_eff{nullptr};
    lv_obj_t *offline{nullptr};
    lv_obj_t *unreliable{nullptr};  ///< invented, see the .cpp: §2 has no drawing for it

    // Battery only.
    lv_obj_t *ring{nullptr};
    lv_obj_t *soc_num{nullptr}, *soc_pct{nullptr};

    // Consumer only.
    std::vector<Movable> movable;
    uint8_t edge{INVALID_INDEX};  ///< its connector, so both move together
    int8_t row{-1};

    // Last written state, so update() touches only what moved.
    std::string txt_name, txt_sub, txt_loss, txt_eff, txt_soc;
    uint32_t col_bg{0xFFFFFFFFu}, col_border{0xFFFFFFFFu};
    uint32_t col_name{0xFFFFFFFFu}, col_icon{0xFFFFFFFFu}, col_sub{0xFFFFFFFFu};
    int32_t ring_pct{-1};
    bool dead{false}, offline_shown{false}, unreliable_shown{false};
  };

  /// Which connector this is. The role decides the line colour and the badge
  /// text colour. It used to decide whether the edge animated as well; §7 as
  /// amended 2026-08-25 gives dots to every edge, so it no longer does.
  enum class Kind : uint8_t { GRID, TAP, PV, OTHER, BATTERY, BUS, CONSUMER };

  /// A point on a flow path: the *centreline* of the run it lies on, absolute
  /// in `root_` coordinates. A dot is placed at the point minus half its size,
  /// so a 6 px disc sits centred on a 2 px wire.
  struct Pt {
    int16_t x{0}, y{0};
  };

  /// The most dots one run may show at once. Every dot is an LVGL object moved
  /// 25 times a second and every move invalidates two small rectangles, so the
  /// cap is what bounds the frame cost when the whole flat is drawing.
  static const uint8_t MAX_DOTS = 4;

  /// One stream of dots along a polyline, source end first.
  ///
  /// An edge owns one — except the bus, which owns one per length between two
  /// branch points, because the current in each of them is different and that
  /// difference is the entire point of the amendment. Count comes from the
  /// power: a run holds one dot per `gap` pixels and `gap` shrinks as the run
  /// carries more, which makes the *linear density* of the dots the reading of
  /// the current rather than their speed.
  struct DotRun {
    std::vector<Pt> pts;
    std::vector<int16_t> cum;  ///< distance along the path at each point; cum[0] = 0
    int16_t len{0};            ///< cum.back()
    uint8_t size{6};

    /// Consumer runs sink with their row (§4.3), so their y is the built y plus
    /// this. Everything else leaves it at zero.
    int16_t dy{0};
    /// The battery is the one run that reverses: discharging, the dots come
    /// back the other way.
    bool rev{false};

    lv_obj_t *dots[MAX_DOTS]{};
    uint8_t n{0};  ///< how many of them are shown, 0 = the run is still
    float phase{0.0f}, speed{0.0f};  ///< speed in runs per second
    bool animate{false};

    /// Last written per-dot state, so a frame that does not move a dot by a
    /// whole pixel costs nothing. At 18 px/s a dot stands still for three
    /// frames in four.
    Pt last[MAX_DOTS]{};
    uint8_t vis_mask{0};

    /// What the rectangles tagged with this run should be painted, and what they
    /// were painted last. Only the bus uses it: it is the one edge whose lengths
    /// are not all in the same state, because each of them answers to a
    /// different set of consumers.
    uint32_t col{0}, col_written{0xFFFFFFFFu};
  };

  /// One rendered edge: its rectangles, its arrowhead, its badge, its ✕ and
  /// (for the four that carry them) its dots.
  struct Edge {
    uint8_t terminal{INVALID_INDEX};
    Kind kind{Kind::CONSUMER};
    uint32_t line_col{0}, val_col{0};

    std::vector<lv_obj_t *> segs;
    std::vector<Rect> seg_rect;
    /// Which dot run each rectangle belongs to, and so which state paints it.
    /// Empty, or `INVALID_INDEX` per entry, means the segment follows the edge's
    /// own state — which is every edge but the bus.
    std::vector<uint8_t> seg_run;
    /// Which segment the ✕ is centred on. -1 picks the longest, which is right
    /// everywhere except the bus, where the trunk is several rectangles and the
    /// longest of them is not the middle of the wire.
    int8_t cross_seg{-1};

    /// The run whose length changes when the arrowhead disappears: a consumer's
    /// stub starts 8 px lower when there is a head to make room for.
    int8_t elastic{-1};
    Rect elastic_head, elastic_bare;

    /// Both ends are precomputed; only one is ever drawn, and it moves on a
    /// sign change, which is a state transition and not a frame.
    lv_obj_t *head[HEAD_STEPS]{nullptr, nullptr, nullptr, nullptr};
    Rect head_fwd, head_rev;
    bool down_fwd{false}, down_rev{false};
    bool has_rev{false};

    /// The flow figure, as a pill centred on the run it labels. Its width is
    /// per-state: the spec gives the battery edge 78 px charging and 64 px at
    /// rest, and every `OFF` caption 66 px.
    lv_obj_t *badge{nullptr}, *badge_lbl{nullptr};
    /// `bcy` is the centre at row 0; a consumer that has sunk down the list
    /// adds 114 px per row on top of it (§4.3).
    int16_t bcx{0}, bcy{0}, bw{0}, bw_idle{0}, bh{28};
    int8_t row{0};
    const lv_font_t *badge_font{nullptr};
    /// The pill as drawn, absolute and unscrolled, so a dot that would cross it
    /// can be hidden instead. `w == 0` when no pill is shown.
    Rect badge_hit;

    /// A 36 px disc in `bg` with an `alert` cross on it, centred on the middle
    /// of the longest run. It hides the line, so it reads as a break in the
    /// wire rather than a mark on top of one (§6).
    lv_obj_t *cross{nullptr};

    /// One for every edge but the bus, which gets one per length between two
    /// consumer rows.
    std::vector<DotRun> runs;

    // Last written state.
    std::string txt_badge;
    uint32_t col_line{0xFFFFFFFFu}, col_badge{0xFFFFFFFFu}, col_badge_txt{0xFFFFFFFFu};
    uint32_t col_dot{0xFFFFFFFFu};
    int8_t head_state{-2};  ///< -2 unwritten, -1 none, 0 forward, 1 reverse
    bool badge_shown{false}, cross_shown{false};
  };

  // --- construction
  void build_(lv_obj_t *parent);
  lv_obj_t *plain_(lv_obj_t *parent);
  lv_obj_t *rect_(lv_obj_t *parent, const Rect &r, uint32_t color);
  lv_obj_t *label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w,
                   lv_text_align_t align, uint32_t color);
  lv_obj_t *card_(int x, int y, int w, int h, int radius);
  uint8_t add_node_(Node &&n);

  void build_source_(Node &n, int x, int w, uint32_t icon_col, const lv_font_t *name_font,
                     const lv_font_t *sub_font);
  void build_other_(Node &n);
  void build_inverter_(Node &n);
  void build_battery_(Node &n);
  void build_consumer_(Node &n, bool left, int row);
  /// Everything a consumer takes with it when it sinks down the list: its card,
  /// its two runs, its head, its badge and its ✕.
  void register_consumer_movables_(uint8_t node_idx);

  void add_edge_(Edge &&e);
  /// A run over `pts`, source first. Lengths are precomputed here because the
  /// geometry never changes and a frame must not do arithmetic it can avoid.
  static DotRun make_run_(std::initializer_list<Pt> pts, uint8_t size);
  /// The centre of a dot at fraction `p` (0 = source, 1 = sink) of the run.
  static Pt run_point_(const DotRun &r, float p);
  /// Turn watts into a dot count and a speed, and show or hide the objects the
  /// count added or dropped. `on` is the edge's own animate decision.
  void size_run_(DotRun &r, float watts, bool on);
  void build_head_(Edge &e, lv_obj_t *parent);
  void place_head_(Edge &e, bool rev);
  void build_badge_(Edge &e, lv_obj_t *parent, int radius, const lv_font_t *font);
  void build_cross_(Edge &e, lv_obj_t *parent, int cx, int cy);

  bool has_glyph_(const lv_font_t *font, uint32_t cp) const;
  /// The configured glyph if the config gave one and the font carries it, the
  /// role default if it did not, and nothing at all if neither is in `f_icons`.
  /// An unlisted codepoint renders as a silent blank, which is worse than an
  /// empty slot.
  std::string icon_text_(const std::string &configured, uint32_t fallback) const;

  // --- per-update painting
  void update_node_(Node &n);
  void update_edge_(Edge &e);
  /// What each length of the bus still carries, from the top down. The trunk is
  /// not one current: every row taps some of it off, and a run whose remainder
  /// has fallen to nothing must stop animating or the diagram claims a load is
  /// drawing when it is not.
  void update_bus_(Edge &e, uint32_t edge_col);
  void update_overlays_();
  void resort_consumers_();
  void set_badge_(Edge &e, const std::string &txt, uint32_t line, uint32_t text, int width,
                  const lv_font_t *font);
  /// Number in `font_metric` plus unit in `font_unit`, sharing a baseline.
  /// Returns the total width so a centred pair can be placed.
  int set_metric_(lv_obj_t *num, lv_obj_t *unit, std::string &cache, const std::string &value,
                  const char *suffix, int x, int y, bool centre);

  // --- helpers
  const Terminal *term_(uint8_t index) const;
  const Device *dev_(uint8_t index) const;
  /// The four rendering states of §6, resolved to the colour of the line.
  uint32_t edge_line_color_(const Edge &e, EdgeState st) const;
  bool inverter_offline_() const;
  bool battery_offline_() const;

  void on_node_clicked_(uint8_t node);
  static void node_event_cb_(lv_event_t *e);

  PowerFlow *pf_{nullptr};
  lv_obj_t *root_{nullptr};    ///< everything dimmable lives under here
  lv_obj_t *scroll_{nullptr};  ///< the consumer list, y = 446, 480 x 354 (§4.4)
  lv_obj_t *banner_{nullptr};  ///< the "Home Assistant is gone" overlay (§6.2)

  /// Terminals resolved once at build time so update() can answer "is the spine
  /// offline" without walking the graph every pass.
  uint8_t t_in_{INVALID_INDEX}, t_out_{INVALID_INDEX}, t_bat_{INVALID_INDEX};
  uint8_t d_bat_{INVALID_INDEX}, d_grid_{INVALID_INDEX}, d_inv_{INVALID_INDEX};
  uint8_t n_inverter_{INVALID_INDEX}, n_grid_{INVALID_INDEX};
  uint8_t e_bus_{INVALID_INDEX};  ///< index into edges_, so update() can drain it

  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  /// Indices into nodes_, in declaration order, split by the configured side.
  std::vector<uint8_t> col_left_, col_right_;

  uint32_t last_frame_{0};
  bool ha_contact_{true};
  bool dimmed_{false};
  bool sorted_once_{false};
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
