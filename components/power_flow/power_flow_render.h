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
#include "power_flow.h"

#ifdef USE_LVGL

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
  PowerFlow *pf_{nullptr};
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
