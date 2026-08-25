#include "pf_screenshot.h"
#ifdef USE_POWER_FLOW_SCREENSHOT

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

namespace esphome {
namespace power_flow {

static const char *const TAG = "pf.screenshot";

static const char *const URL_PAGE = "/pf-screenshot";
static const char *const URL_BMP = "/pf-screenshot.bmp";

// BITMAPFILEHEADER + BITMAPINFOHEADER + the three BI_BITFIELDS masks.
static constexpr uint32_t BMP_HEADER = 14 + 40 + 12;
// Where the pixels start inside `mem_`. The allocation is 64-byte aligned and
// so is this offset, which is what LVGL wants of a draw buffer; the header is
// written into the slack just before it, so the response is one contiguous run
// of bytes and needs no second buffer.
static constexpr uint32_t PIXELS_AT = 128;
static_assert(PIXELS_AT >= BMP_HEADER, "no room for the BMP header");

// How long the httpd task waits for the main loop to draw. A snapshot is tens
// of milliseconds; anything near this means the loop is wedged.
static constexpr uint32_t RENDER_TIMEOUT_MS = 5000;

// The viewer. Deliberately inert: one capture per press, one per reload, and
// the polling line left commented out — a wall panel should not be re-rendering
// its own screen into a browser forever because a tab was left open.
static const char *const VIEWER_HTML =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Panel screenshot</title><style>"
    "body{margin:0;padding:12px;background:#111;color:#ddd;"
    "font:14px system-ui,sans-serif}"
    "button{font:inherit;padding:6px 14px;border:0;border-radius:4px;"
    "background:#2d6cdf;color:#fff;cursor:pointer}"
    "img{display:block;margin-top:12px;max-width:100%;image-rendering:pixelated;"
    "border:1px solid #333}"
    "</style></head><body>"
    "<button onclick=\"shoot()\">Capture</button> <span id=\"t\"></span>"
    "<img id=\"s\" alt=\"panel\">"
    "<script>\n"
    "function shoot(){var i=document.getElementById('s'),d=new Date();\n"
    "  i.src='/pf-screenshot.bmp?'+d.getTime();\n"
    "  document.getElementById('t').textContent=d.toLocaleTimeString();}\n"
    "shoot();\n"
    "// Auto-refresh is off by default. Uncomment to poll every 5 s:\n"
    "// setInterval(shoot, 5000);\n"
    "</script></body></html>";

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}

void PfScreenshot::setup() {
  if (web_server_base::global_web_server_base == nullptr) {
    ESP_LOGE(TAG, "no web server to attach to");
    this->mark_failed();
    return;
  }
  web_server_base::global_web_server_base->init();
  web_server_base::global_web_server_base->add_handler(this);
  // Nothing to do until a request arrives; handleRequest() wakes us.
  this->disable_loop();
}

void PfScreenshot::dump_config() {
  ESP_LOGCONFIG(TAG, "Debug screenshot: http://<panel>%s (viewer), %s (BMP)", URL_PAGE, URL_BMP);
}

// --- httpd task -------------------------------------------------------------

bool PfScreenshot::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;
  char url[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef path = request->url_to(url);
  return path == URL_PAGE || path == URL_BMP;
}

void PfScreenshot::handleRequest(AsyncWebServerRequest *request) {
  char url[AsyncWebServerRequest::URL_BUF_SIZE];
  if (request->url_to(url) == URL_PAGE) {
    request->send(200, "text/html", VIEWER_HTML);
    return;
  }
  this->serve_bmp_(request);
}

void PfScreenshot::serve_bmp_(AsyncWebServerRequest *request) {
  {
    LockGuard guard(this->lock_);
    if (this->state_ != State::IDLE) {
      // One capture at a time: a second would race the first for `mem_`.
      request->send(503, "text/plain", "capture already in progress");
      return;
    }
    this->state_ = State::REQUESTED;
    this->orphan_ = false;
  }
  this->enable_loop_soon_any_context();

  const uint32_t started = millis();
  bool done = false;
  while (true) {
    {
      LockGuard guard(this->lock_);
      done = this->state_ == State::DONE;
      if (!done && millis() - started >= RENDER_TIMEOUT_MS) {
        // Give up, but leave `mem_` to loop(): it may be drawing into it right
        // now. Setting this inside the same critical section loop() uses to
        // publish DONE is what makes the handover unambiguous.
        this->orphan_ = true;
        break;
      }
    }
    if (done)
      break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!done) {
    ESP_LOGW(TAG, "main loop did not answer in %ums", (unsigned) RENDER_TIMEOUT_MS);
    request->send(503, "text/plain", "timed out waiting for the main loop");
    return;
  }

  if (this->ok_) {
    request->send(request->beginResponse(200, "image/bmp", this->mem_ + PIXELS_AT - BMP_HEADER, this->bmp_len_));
  } else {
    request->send(500, "text/plain", "snapshot failed");
  }
  this->release_();
  LockGuard guard(this->lock_);
  this->state_ = State::IDLE;
}

// --- main loop --------------------------------------------------------------

void PfScreenshot::loop() {
  {
    LockGuard guard(this->lock_);
    if (this->state_ != State::REQUESTED) {
      // DONE means the httpd task owns the buffer; IDLE means there is nothing
      // to do until the next request re-enables us.
      if (this->state_ == State::IDLE)
        this->disable_loop();
      return;
    }
  }

  const bool ok = this->render_();

  bool orphaned = false;
  {
    LockGuard guard(this->lock_);
    this->ok_ = ok;
    if (this->orphan_) {
      // Nobody is waiting any more — clean up here rather than leak the frame.
      this->orphan_ = false;
      this->state_ = State::IDLE;
      orphaned = true;
    } else {
      this->state_ = State::DONE;
    }
  }
  if (orphaned)
    this->release_();
}

bool PfScreenshot::render_() {
  lv_obj_t *scr = lv_screen_active();
  const uint32_t w = lv_obj_get_width(scr);
  const uint32_t h = lv_obj_get_height(scr);
  const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
  // A BMP row is padded to four bytes, which at 16 bpp any even width already
  // satisfies, and LVGL's own alignment is a multiple of four as well. If the
  // two ever disagree the file would come out sheared, so refuse rather than
  // serve something subtly wrong.
  const uint32_t bmp_stride = ((w * 16 + 31) / 32) * 4;
  if (stride != bmp_stride) {
    ESP_LOGE(TAG, "LVGL stride %u is not the BMP row size %u for %u px", (unsigned) stride, (unsigned) bmp_stride,
             (unsigned) w);
    return false;
  }
  const uint32_t pixels = stride * h;

  // LVGL's own pool is 64 KB, so the frame has to come from PSRAM and be handed
  // to the snapshot as a pre-initialised draw buffer.
  this->mem_ = (uint8_t *) heap_caps_aligned_alloc(64, PIXELS_AT + pixels, MALLOC_CAP_SPIRAM);
  if (this->mem_ == nullptr) {
    ESP_LOGE(TAG, "cannot allocate %u bytes of PSRAM", (unsigned) (PIXELS_AT + pixels));
    return false;
  }
  lv_draw_buf_t buf{};
  if (lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, stride, this->mem_ + PIXELS_AT, pixels) != LV_RESULT_OK ||
      lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &buf) != LV_RESULT_OK) {
    ESP_LOGE(TAG, "snapshot failed");
    this->release_();
    return false;
  }

  // BI_BITFIELDS/RGB565 with a negative height, i.e. rows top-down in the order
  // LVGL already produced them. Browsers and Pillow both read it as it stands.
  uint8_t *hdr = this->mem_ + PIXELS_AT - BMP_HEADER;
  hdr[0] = 'B';
  hdr[1] = 'M';
  put_u32(hdr + 2, BMP_HEADER + pixels);  // file size
  put_u32(hdr + 6, 0);                    // both reserved words
  put_u32(hdr + 10, BMP_HEADER);          // offset to the pixels
  put_u32(hdr + 14, 40);                  // BITMAPINFOHEADER
  put_u32(hdr + 18, w);
  put_u32(hdr + 22, (uint32_t) (-(int32_t) h));
  put_u16(hdr + 26, 1);   // planes
  put_u16(hdr + 28, 16);  // bits per pixel
  put_u32(hdr + 30, 3);   // BI_BITFIELDS
  put_u32(hdr + 34, pixels);
  put_u32(hdr + 38, 2835);  // 72 dpi, in pixels per metre
  put_u32(hdr + 42, 2835);
  put_u32(hdr + 46, 0);  // palette: none used
  put_u32(hdr + 50, 0);  // palette: none important
  put_u32(hdr + 54, 0xF800);
  put_u32(hdr + 58, 0x07E0);
  put_u32(hdr + 62, 0x001F);

  this->bmp_len_ = BMP_HEADER + pixels;
  ESP_LOGD(TAG, "captured %ux%u, %u bytes", (unsigned) w, (unsigned) h, (unsigned) this->bmp_len_);
  return true;
}

void PfScreenshot::release_() {
  this->bmp_len_ = 0;
  if (this->mem_ != nullptr) {
    heap_caps_free(this->mem_);
    this->mem_ = nullptr;
  }
}

}  // namespace power_flow
}  // namespace esphome

#endif  // USE_POWER_FLOW_SCREENSHOT
