#include "pf_screenshot.h"
#ifdef USE_POWER_FLOW_SCREENSHOT

#include "esphome/core/log.h"
#include "esphome/core/alloc_helpers.h"

#include <esp_crc.h>
#include <esp_heap_caps.h>

#include <algorithm>

namespace esphome {
namespace power_flow {

static const char *const TAG = "pf.screenshot";

// Payload bytes per log line: 240 raw bytes encode to 320 base64 characters,
// which fits the logger's line buffer with the prefix to spare.
static constexpr uint32_t CHUNK = 240;
// Lines per loop() pass. Four lines (~1 KB of payload) per pass drains the
// 768 KB frame in about a minute; burstier pacing overflowed the console
// buffer and dropped bytes mid-frame when tried at eight.
static constexpr int LINES_PER_PASS = 4;

void PfScreenshot::setup() { this->register_service(&PfScreenshot::take_, "screenshot"); }

void PfScreenshot::dump_config() { ESP_LOGCONFIG(TAG, "Debug screenshot service: <node>_screenshot"); }

void PfScreenshot::take_() {
  if (this->dumping_) {
    // A fresh request wins: the previous dump's reader is gone (or it would
    // not be re-triggering), so finishing the old frame helps nobody.
    ESP_LOGW(TAG, "restarting: previous dump abandoned");
    this->release_();
  }
  lv_obj_t *scr = lv_screen_active();
  const uint32_t w = lv_obj_get_width(scr);
  const uint32_t h = lv_obj_get_height(scr);
  const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
  const uint32_t size = stride * h;
  // LVGL's own pool is 64 KB, so the frame buffer has to come from PSRAM and
  // be handed to the snapshot as a pre-initialised draw buffer.
  this->mem_ = (uint8_t *) heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
  if (this->mem_ == nullptr) {
    ESP_LOGE(TAG, "cannot allocate %u bytes of PSRAM", (unsigned) size);
    return;
  }
  if (lv_draw_buf_init(&this->buf_, w, h, LV_COLOR_FORMAT_RGB565, stride, this->mem_, size) != LV_RESULT_OK ||
      lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &this->buf_) != LV_RESULT_OK) {
    ESP_LOGE(TAG, "snapshot failed");
    this->release_();
    return;
  }
  this->len_ = size;
  this->pos_ = 0;
  this->dumping_ = true;
  ESP_LOGI(TAG, "BEGIN w=%u h=%u stride=%u fmt=RGB565LE len=%u crc=%08x", (unsigned) w, (unsigned) h,
           (unsigned) stride, (unsigned) size, (unsigned) esp_crc32_le(0, this->buf_.data, size));
}

void PfScreenshot::loop() {
  if (!this->dumping_)
    return;
  for (int i = 0; i < LINES_PER_PASS && this->pos_ < this->len_; i++) {
    const uint32_t n = std::min(CHUNK, this->len_ - this->pos_);
    ESP_LOGI(TAG, "D%s", base64_encode(this->buf_.data + this->pos_, n).c_str());
    this->pos_ += n;
  }
  if (this->pos_ >= this->len_) {
    ESP_LOGI(TAG, "END");
    this->release_();
  }
}

void PfScreenshot::release_() {
  this->dumping_ = false;
  this->len_ = this->pos_ = 0;
  if (this->mem_ != nullptr) {
    heap_caps_free(this->mem_);
    this->mem_ = nullptr;
  }
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_POWER_FLOW_SCREENSHOT
