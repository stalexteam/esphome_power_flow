// Debug screenshot service (`debug_screenshot: true`).
//
// One Home Assistant service call — `esphome.<node>_screenshot` — renders the
// active LVGL screen into a PSRAM buffer and drains it into the serial log as
// base64, a few lines per loop() pass so the panel stays responsive. The
// counterpart that triggers the service and reassembles the frame into a PNG
// is tools/screenshot.py.
//
// Debug tooling only: compiled in solely when the YAML asks for it, and the
// buffer exists only between the service call and the end of the dump.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_POWER_FLOW_SCREENSHOT

#include "esphome/core/component.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/lvgl/lvgl_esphome.h"

#include <cstdint>

namespace esphome {
namespace power_flow {

class PfScreenshot : public Component, public api::CustomAPIDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void take_();
  void release_();

  uint8_t *mem_{nullptr};
  lv_draw_buf_t buf_{};
  uint32_t len_{0};
  uint32_t pos_{0};
  bool dumping_{false};
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_POWER_FLOW_SCREENSHOT
