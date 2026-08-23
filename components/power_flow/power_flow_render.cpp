#include "power_flow_render.h"

#ifdef USE_LVGL

#include "esphome/core/log.h"

namespace esphome {
namespace power_flow {

static const char *const TAG = "power_flow.render";

// Stub. The diagram is stage 3; this exists so the panel keeps linking and
// running while the arithmetic is verified against live data first, which §8
// asks for in as many words: the numbers must be trusted before anything is
// drawn.

void FlowRenderer::setup(PowerFlow *pf) {
  this->pf_ = pf;
  ESP_LOGCONFIG(TAG, "renderer not built yet — the diagram area stays empty");
}

void FlowRenderer::update() {}

void FlowRenderer::frame(uint32_t now) {}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_LVGL
