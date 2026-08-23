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
// Three layers, per §7, and the split is a redraw-cost decision:
//
//   connectors  static rectangles, positioned once at setup
//   nodes       obj + labels, so LVGL invalidates only the box that changed
//   dots        small objs stepped along a precomputed point table
//
// §7 suggests a canvas for the connectors. This firmware does not have one:
// ESPHome emits LV_USE_CANVAS 0 and LV_USE_LINE 0 into lv_conf.h because the
// page declares neither widget, and the page is not ours to edit. It costs
// nothing — the fixed slot layout makes every connector axis-aligned, so a
// thin `obj` is both cheaper than an lv_line and exactly as static. The same
// constraint is why an arrowhead is a five-rectangle staircase and not a
// polygon. LV_USE_ARC *is* 1 — the battery page declares an `arc` — which is
// what makes the state-of-charge ring possible where a canvas is not.
//
// Layout, reworked against the owner's own sketches. Five bands:
//
//   1 sources      GRID (wide) · PV
//   2 conversion   ONGRID LOAD · INVERTER (Loss/Eff/Supply) · BATTERY (ring)
//   3 remainder    OTHER, off the top of the bus trunk
//   4 bus          a vertical trunk down from the inverter
//   5 loads        one vertical slot each, left and right alternating
//
// Two rules shaped the geometry:
//
//   * **Every flow figure is a badge on its own connector**, never a number
//     inside a box. A measurement belongs to a terminal and a terminal is the
//     end of an edge, so the number is drawn on the edge. A box carries only
//     what is true of the device.
//   * **The badge sets the minimum length of the run it sits on**, measured at
//     full label_font for the widest string the formatter can produce. That is
//     why a consumer's connector leaves the trunk on a long horizontal run and
//     turns down into the top of its box, instead of poking at its side: the
//     side stub is barely half as long and would force a smaller font.
//
#include "power_flow.h"

#ifdef USE_LVGL

#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace power_flow {

class FlowRenderer {
 public:
  /// Build the whole widget tree under `pf->parent()` with the raw LVGL C API.
  /// §8 is explicit that this component must not register an LVGL widget type;
  /// it parents plain objects to an `obj` the user declared on the page.
  void setup(PowerFlow *pf);

  /// Push resolved values, edge states and diagnostics into the widgets. Only
  /// what changed should be touched — LVGL invalidates per object, and that is
  /// the whole reason nodes are objects rather than canvas paint (§7).
  void update();

  /// Advance the flow dots. Called every pass, so it must return immediately
  /// when nothing is due.
  void frame(uint32_t now);

 protected:
  /// Five rectangles make one arrowhead. A polygon would need the canvas this
  /// build does not have, and a rotated object would need transform support
  /// that is not guaranteed to be compiled in.
  static const int ARROW_STEPS = 5;

  /// A point on a connector's centreline. int16_t because the panel is 480x800
  /// and a path of 150 of these is 600 bytes rather than 1200.
  struct Pt {
    int16_t x, y;
  };

  /// One axis-aligned run of a connector, stored as its centreline. The obj's
  /// rectangle is derived from this whenever the line weight changes, which is
  /// the only thing about a connector that is not static.
  struct Seg {
    lv_obj_t *obj{nullptr};
    int16_t x1{0}, y1{0}, x2{0}, y2{0};
  };

  /// Which fixed slot a node occupies (§4). Kept as an enum rather than
  /// inferred from the device kind so the click handler and the value
  /// formatter can tell a battery from a socket without a string compare.
  ///
  /// SELF is gone: the inverter's standing draw is the residual of its own
  /// node, which is exactly what `Loss` in the inverter box reports, and the
  /// owner's sketch gives that slot to ONGRID LOAD. BUS is gone too — the bus
  /// is drawn as a bare trunk with a badge, with no box of its own.
  enum class Slot : uint8_t { GRID, PV, ONGRID, INVERTER, BATTERY, CONSUMER, OTHERS };

  /// A drawn box. `terminal` is the edge this node sits at the end of; it is
  /// used to colour the box and to resolve a tap, never to print a number —
  /// numbers live on the connectors now.
  struct Node {
    Slot slot{Slot::CONSUMER};
    uint8_t terminal{INVALID_INDEX};
    uint8_t device{INVALID_INDEX};

    lv_obj_t *box{nullptr};
    lv_obj_t *icon{nullptr};
    lv_obj_t *name{nullptr};
    lv_obj_t *r1{nullptr};  ///< first device-level reading, or the state flag
    lv_obj_t *r2{nullptr};  ///< second reading  (inverter: efficiency)
    lv_obj_t *r3{nullptr};  ///< third reading   (inverter: supply mode)
    lv_obj_t *ring{nullptr};      ///< state of charge, battery only (lv_arc)
    lv_obj_t *ring_lbl{nullptr};  ///< the percentage, inside the ring

    // Last written state, so update() touches only what moved.
    std::string txt_name, txt_r1, txt_r2, txt_r3, txt_ring;
    uint32_t col_border{0xFFFFFFFFu}, col_bg{0xFFFFFFFFu};
    uint32_t col_r1{0xFFFFFFFFu}, col_ring{0xFFFFFFFFu};
    int32_t ring_pct{-1};
    uint8_t opa{0};
    bool ring_off{false};
  };

  /// One rendered edge: its rectangles, its point table, its dots, its cross
  /// marker, its arrowhead and its badge. `path` always runs in the direction
  /// of *positive* flow; a negative value is drawn by walking it backwards and
  /// by moving the arrowhead to the other end.
  struct Edge {
    uint8_t terminal{INVALID_INDEX};
    std::vector<Seg> segs;
    std::vector<Pt> path;
    lv_obj_t *dots[3]{nullptr, nullptr, nullptr};
    lv_obj_t *cross{nullptr};

    /// The flow figure, as a rounded pill on the connector itself. Its centre
    /// is fixed at build time — the elbow, or the midpoint of the long run.
    lv_obj_t *badge{nullptr};
    lv_obj_t *badge_lbl{nullptr};
    int16_t bx{0}, by{0};
    std::string txt_badge;
    uint32_t badge_col{0xFFFFFFFFu};
    bool badge_hidden{true};

    /// Direction is most of what the diagram is for, so every connector ends
    /// in a head. Both ends are precomputed; only one is drawn, and it moves
    /// on a sign change, which is a state transition and not a frame.
    lv_obj_t *arrow[ARROW_STEPS]{nullptr, nullptr, nullptr, nullptr, nullptr};
    Pt tip_fwd{0, 0}, tip_rev{0, 0};
    uint8_t dir_fwd{0}, dir_rev{0};
    int8_t arrow_rev{-1};  ///< -1 = never placed

    float phase{0.0f};   ///< position along `path`, in table indices
    float speed{0.0f};   ///< table indices per second
    bool reverse{false};
    bool animate{false};

    uint32_t color{0xFFFFFFFFu};
    int16_t thick{-1};
    uint8_t opa{0};
    bool show_cross{false};
  };

  // --- construction
  void build_(lv_obj_t *parent);
  lv_obj_t *plain_(lv_obj_t *parent);
  lv_obj_t *label_(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w, lv_text_align_t align,
                   uint32_t color, uint8_t opa);
  /// One box. `nf` is the font its name is drawn in — picked per group so a
  /// row of consumers never ends up with mixed type sizes. `lines` is how many
  /// small reading lines sit under the name.
  uint8_t add_node_(Slot slot, uint8_t device, uint8_t terminal, int x, int y, int w, int h,
                    const lv_font_t *nf, int lines);
  /// `bx`,`by` is where this edge's badge sits: the midpoint of the run that
  /// was made long enough to hold it.
  void add_edge_(uint8_t terminal, const Pt *corners, size_t count, int bx, int by);
  void place_arrow_(Edge &e, bool rev);
  bool has_glyph_(const lv_font_t *font, uint32_t cp) const;
  /// A name and the text width of the box it has to fit in.
  using NameFit = std::pair<std::string, int>;
  /// The largest of the two configured fonts in which every one of `names`
  /// fits on one line in its own box. Uniform within a group by design.
  const lv_font_t *fit_font_(const std::vector<NameFit> &names) const;

  // --- per-update painting
  void update_node_(Node &n);
  void update_edge_(Edge &e);
  void update_flags_();
  void set_badge_(Edge &e, const std::string &txt, uint32_t color);

  // --- helpers
  const Terminal *term_(uint8_t index) const;
  const Device *dev_(uint8_t index) const;
  uint32_t state_color_(const Terminal *t) const;
  /// The two spine devices say "OFFLINE" in place of their readings instead of
  /// vanishing: a hole where the inverter stood reads as a bug (§4).
  bool inverter_offline_() const;
  bool battery_offline_() const;

  /// Resolves the tapped node to its device and fires the device's trigger.
  /// The component still knows nothing about pages — the YAML decides (§7).
  void on_node_clicked_(uint8_t node);
  static void node_event_cb_(lv_event_t *e);

  PowerFlow *pf_{nullptr};
  lv_obj_t *root_{nullptr};    ///< everything dimmable lives under here
  lv_obj_t *banner_{nullptr};  ///< the "Home Assistant is gone" overlay (§6.2)

  /// Nothing announces that everything is fine. These two appear only when
  /// something is not: the grid verdict under the grid box when the inference
  /// distrusts the reading, and an amber pill over the inverter when a balance
  /// came out implausible.
  lv_obj_t *grid_verdict_{nullptr};
  lv_obj_t *unreliable_{nullptr};
  std::string txt_verdict_;
  bool flag_unreliable_{false};
  bool flag_verdict_{false};

  /// Terminals of the inverter, resolved once at build time so update() can
  /// answer "is the spine offline" without walking the graph every pass.
  uint8_t t_in_{INVALID_INDEX}, t_out_{INVALID_INDEX}, t_bat_{INVALID_INDEX};
  uint8_t d_bat_{INVALID_INDEX};

  std::vector<Node> nodes_;
  std::vector<Edge> edges_;

  uint32_t bg_color_{0x000000};  ///< the page's own background, read at setup
  uint32_t last_frame_{0};
  bool ha_contact_{true};
  bool dimmed_{false};
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
