// Debug screenshot endpoint (`debug_screenshot: true`).
//
// The panel serves the frame it is showing over HTTP. `/pf-screenshot.bmp` is
// the active LVGL screen as a 16-bit BMP; `/pf-screenshot` is a one-page
// viewer around it. Both capture on demand — nothing is cached, nothing
// refreshes on its own, and no buffer exists between requests. The
// counterpart that fetches one and writes a PNG is tools/screenshot.py.
//
// The snapshot has to happen on the main loop: ESPHome's LVGL has no lock and
// the HTTP handler runs on the httpd task. So the handler parks, loop() draws
// into a PSRAM buffer, and the handler sends it. `state_` under `lock_` is the
// whole handover, and it exists to keep exactly one of the two tasks owning
// `mem_` at any moment.
//
// Debug tooling only: compiled in solely when the YAML asks for it.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_POWER_FLOW_SCREENSHOT

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/lvgl/lvgl_esphome.h"

#include <cstdint>

namespace esphome {
namespace power_flow {

class PfScreenshot : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// Same slot the web_server component uses, and for the same reason: setup()
  /// starts the listener, and starting one before the wifi component has
  /// brought up the network stack asserts inside lwIP.
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override;
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  // Which task may touch `mem_`: REQUESTED means loop(), DONE means the httpd
  // task, IDLE means neither and there is nothing allocated.
  enum class State : uint8_t { IDLE, REQUESTED, DONE };

  bool render_();  ///< main loop only; fills mem_ and bmp_len_
  void release_();
  void serve_bmp_(AsyncWebServerRequest *request);

  uint8_t *mem_{nullptr};
  uint32_t bmp_len_{0};

  Mutex lock_;
  State state_{State::IDLE};
  bool ok_{false};      ///< did render_() succeed (valid while state_ == DONE)
  bool orphan_{false};  ///< the waiter gave up; loop() frees what it produced
};

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_POWER_FLOW_SCREENSHOT
