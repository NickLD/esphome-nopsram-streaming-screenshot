#pragma once
// Stream a pixel-perfect BMP of the current LVGL screen over HTTP, without
// ever allocating a full frame buffer. We chain LVGL's existing flush_cb to
// also push converted pixel rows into a small "band" buffer handed back and
// forth between a producer (this component, on the main loop task, inside
// the one lv_refr_now() call a capture triggers) and a consumer (the HTTP
// handler, on an httpd worker task) via two tiny FreeRTOS queues.
//
// Allocation lifetime: the band buffer(s) and both queues are allocated at
// the START of handleRequest() and freed before it returns - NOT held for
// the device's entire uptime. An earlier version of this file created a
// 40-slot row queue + a 30-row batch buffer once in setup() and kept them
// forever (~102KB permanently, on this panel, whether or not a capture was
// ever requested - about 80% of this device's documented free DRAM). The
// "no full frame buffer" design goal was already met; what wasn't met was
// "no *permanent* allocation for a feature used maybe once an hour during
// active development." Now: ~0 extra RAM at idle, ~NUM_BANDS * (BATCH_ROWS
// * ROW_BYTES) while a capture is actually in flight (see the NUM_BANDS and
// band-size comments below for the exact numbers on each panel this file is
// shared with, and why NUM_BANDS is 1, not the more obvious-looking 2).
//
// Producer/consumer handoff and its failure modes (read this before
// touching capture_done_/collecting_/request_in_progress_ - the ordering
// between them is load-bearing, see the comments at each one):
//  - request_in_progress_ gates a second handleRequest() call from starting
//    at all, and is only released once we're SURE the producer triggered by
//    THIS call has fully stopped touching the buffers we're about to free
//    (see the capture_done_ wait at the end of handleRequest()). Releasing
//    it any earlier would let a second request tear down/reallocate buffers
//    while the first capture's producer might still be mid-flush.
//  - collecting_ is a separate, narrower signal: "is handleRequest() still
//    actively pulling filled bands right now." It's what on_flush_ checks
//    to decide whether to keep waiting for the consumer or bail. It must be
//    a DIFFERENT flag from request_in_progress_ - if the producer's bailout
//    depended on request_in_progress_ instead, and request_in_progress_'s
//    release depended on waiting for the producer to finish (which it does,
//    for the reason above), that's a deadlock: consumer waits for producer,
//    producer waits for a signal that only fires after consumer is done
//    waiting. collecting_ has no such cycle - handleRequest() sets it false
//    itself, synchronously, the moment its own row-collection loop exits,
//    before it ever starts waiting on capture_done_.
//  - capture_done_ is set by loop() itself, right after its lv_refr_now()
//    call returns - the only reliable "the producer's burst for this
//    request is completely finished" signal. Older code used the absence
//    of any tracked completion signal to justify never worrying about it,
//    which only worked because everything was permanently allocated and
//    never freed. Now that buffers ARE freed per-capture, something has to
//    know when it's actually safe to do that.
//
// Worst-case bound: if a capture goes badly (slow/stalled client, or the
// main loop was busy >1s when the request came in and didn't pick up
// capture_requested_ promptly), the whole request is bounded by
// CAPTURE_BUDGET_MS end-to-end, and on_flush_ notices collecting_ go false
// and stops producing within a few hundred ms of that deadline firing - not
// the ~10-14 minutes possible in an earlier version of this file, where a
// timed-out consumer would leave nobody draining a now-permanent queue and
// each of the remaining ~280 rows separately blocked for its own multi-
// second producer-side send timeout, one after another, on the shared main
// loop task (freezing touch input, the backlight auto-off script, HA API
// traffic - everything - for the whole stall).
//
// Known rough edges (still true, not attempted here):
//  - A second concurrent request is rejected outright (503) rather than
//    queued - simplest correct thing given this is a single-capture-at-a-
//    time design, not a real multi-client server.
//  - RGB565 byte-order assumption (big-endian) has been confirmed working
//    unmodified across two real devices with DIFFERENT display-driver
//    `color_order` settings (RGB on an ILI9341 2.8" panel, BGR on an
//    ST7796 3.5" panel) - that setting is handled entirely by the display
//    driver's own MADCTL/output stage, not by this component, so it was
//    never actually the risk it looked like. The real still-open risk is
//    LVGL's own color depth/byte-order BUILD setting (not exercised by
//    either device above, both of which use LVGL's default), which
//    actually governs px_map's layout. Flagged, not fixed, in this pass.

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/network/util.h"
#include "lvgl.h"
#include "display/lv_display_private.h"  // fallback only - see loop()'s flush_cb lookup

#include <esp_http_server.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <algorithm>

namespace esphome {
namespace screenshot {

static const char *const TAG = "screenshot";

// Set via this component's own `width:`/`height:` config keys (see
// examples/minimal.yaml) - __init__.py emits these as build defines
// itself, so a consumer's YAML never needs to touch platformio_options
// directly. The #ifndef defaults below (320x240) only matter if this
// header is ever compiled standalone, outside the __init__.py codegen
// path that always supplies both.
#ifndef SCREENSHOT_SCREEN_W
#define SCREENSHOT_SCREEN_W 320
#endif
#ifndef SCREENSHOT_SCREEN_H
#define SCREENSHOT_SCREEN_H 240
#endif
static constexpr int32_t SCREEN_W = SCREENSHOT_SCREEN_W;
static constexpr int32_t SCREEN_H = SCREENSHOT_SCREEN_H;
static constexpr size_t ROW_BYTES = SCREEN_W * 3;  // BGR888, no row padding
static_assert(ROW_BYTES % 4 == 0,
              "BMP rows must be 4-byte aligned and this encoder writes no row padding; "
              "a future SCREENSHOT_SCREEN_W that fails this needs padding logic added, not just a wider panel.");
static constexpr size_t IMAGE_BYTES = ROW_BYTES * SCREEN_H;
static constexpr size_t HEADER_BYTES = 54;

// Rows per band buffer, sized off a target HTTP chunk byte count rather
// than a row count borrowed from one specific panel's LVGL draw-buffer band
// height (which isn't even a constant - ESPHome's lvgl: component
// auto-shrinks the draw buffer to whatever allocation succeeds, and this
// file's batching was never actually coupled to that at runtime anyway,
// only in a since-stale comment).
//
// Measurements on this device (NUM_BANDS=1, so these ARE the transient
// peak bytes, not doubled): 8,192B (BATCH_ROWS=5, 7.2KB peak) ~1.71s/
// capture; 4,096B (BATCH_ROWS=2, 2.88KB peak) ~1.70s/capture - within
// noise of the 5-row number, meaning per-chunk network overhead stopped
// being the dominant cost well before this point (the per-row producer/
// consumer handoff itself - queue send/receive, task wake latency - is
// roughly constant regardless of how many rows are batched per handoff,
// once NUM_BANDS=1 already serializes everything). Priority is minimizing
// memory, not speed: pushed to BATCH_ROWS=1 (1.44KB peak, the true floor -
// LVGL delivers flush data a row at a time at minimum) since 5->2 showed
// no meaningful cost from finer batching.
static constexpr size_t TARGET_CHUNK_BYTES = 1440;
static constexpr int32_t BATCH_ROWS = (TARGET_CHUNK_BYTES / ROW_BYTES) > 0 ? (int32_t) (TARGET_CHUNK_BYTES / ROW_BYTES)
                                                                            : 1;
static_assert(BATCH_ROWS >= 1, "ROW_BYTES exceeds the chunk byte budget - shrink TARGET_CHUNK_BYTES's assumptions");

// Number of band buffers. 2 (double buffering, producer fills one band
// while the previous one is still being sent) is the more obvious choice
// and was this file's first cut - it lets rendering and network I/O
// overlap. But that overlap is a THROUGHPUT optimization, and this
// component's stated priority is minimizing memory use, not capture speed
// (an occasional dev-tool screenshot pull taking longer doesn't matter).
// With NUM_BANDS=1, there is no overlap: the producer blocks waiting for
// the consumer to finish sending band N and hand the same buffer back
// before it can start filling band N+1, fully serializing render and
// network time instead of pipelining them - and that's fine here. Halves
// the transient peak (1 * BATCH_ROWS * ROW_BYTES instead of 2 *) for a
// speed cost this component doesn't need to pay for. The rest of the
// producer/consumer code needs no changes to support this - free_q_/
// full_q_ are still real FreeRTOS queues, just depth 1 instead of 2, and
// naturally serialize the handoff on their own.
static constexpr int32_t NUM_BANDS = 1;

// A queue message is a pointer + row count, not row pixel data - this is
// the difference between the four full-row-sized memcpys per row an
// earlier version of this file did (conversion -> stack -> queue storage ->
// request buffer -> batch buffer) and the one write per row this version
// does (conversion writes directly into the band buffer that eventually
// gets sent; nothing else touches the pixel bytes until httpd copies them
// to the socket).
struct Band {
  uint8_t *buf;  // capacity: BATCH_ROWS * ROW_BYTES
  int32_t rows;  // valid rows currently in buf (0..BATCH_ROWS)
};

// Overall wall-clock budget for one capture, from request-received to
// give-up-and-pad. ~3x the ~10-15ms/row network cost this file's own
// earlier testing measured for a full-panel capture, so it shouldn't ever
// fire against a merely-slow-but-working client - only against a genuinely
// stalled one or a main-loop scheduling delay. See the file-level comment
// above for what bounds on_flush_'s own reaction time to this firing.
static constexpr uint32_t CAPTURE_BUDGET_MS = 15000;

class ScreenshotComponent;
// Single-translation-unit assumption: ESPHome's codegen includes each
// external_component's headers exactly once, into one generated main.cpp,
// so this has one definition in practice. If that ever changes (this file
// split across multiple .cpp files, reused in a context with its own build
// unit), this global would silently split into independent per-TU
// instances and flush_trampoline in one of them would see nullptr forever.
static ScreenshotComponent *g_screenshot_instance = nullptr;

// Releases an std::atomic<bool> "in progress" flag on scope exit, so
// handleRequest()'s multiple early-return paths can't accidentally leave
// it stuck true.
class AtomicFlagGuard {
 public:
  explicit AtomicFlagGuard(std::atomic<bool> *flag) : flag_(flag) {}
  ~AtomicFlagGuard() { this->flag_->store(false); }

 private:
  std::atomic<bool> *flag_;
};

class ScreenshotComponent : public Component, public AsyncWebHandler {
 public:
  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }

  void setup() override {
    g_screenshot_instance = this;
    // No queue/buffer allocation here - see the file-level comment on
    // allocation lifetime. (An earlier version of this method also
    // precomputed a pair of 5/6-bit -> 8-bit channel-expansion lookup
    // tables here, as permanent members, purely to shave a few ALU ops per
    // pixel off on_flush_'s conversion loop - reverted: that's a capture-
    // speed optimization, and this component's priority is minimizing
    // permanent memory, not speed. on_flush_ does the shift/or math inline
    // again instead of paying ~96 bytes of permanent RAM to skip it.)
  }

  void loop() override {
    if (!this->flush_hooked_) {
      // Deferred from setup(): ESPHome doesn't guarantee our setup() runs
      // after the lvgl: component's own setup() creates its lv_display_t
      // (both default to the same setup_priority). Every component's
      // setup() has finished by the time any component's loop() runs, so
      // this is safe here - but the display still might not exist if the
      // lvgl: component's own setup failed, hence the null check below.
      auto *disp = lv_display_get_default();
      if (disp == nullptr)
        return;  // retry next tick rather than deref a null display
      // disp->flush_cb via the private header, not a public accessor: kept
      // as-is from the original version of this file rather than guessing
      // at a public LVGL 9.x API (lv_display_get_flush_cb()) that hasn't
      // been verified to exist in the LVGL version this project bundles -
      // not worth risking a build break over in an otherwise-unrelated
      // change. Revisit if this file ever needs to bump its LVGL private-
      // header dependency for another reason.
      this->orig_flush_cb_ = disp->flush_cb;
      lv_display_set_flush_cb(disp, &ScreenshotComponent::flush_trampoline);
      this->flush_hooked_ = true;
      ESP_LOGI(TAG, "flush_cb hooked (disp=%p, orig_flush_cb=%p)", disp, (void *) this->orig_flush_cb_);
    }
    if (!this->web_server_started_ && network::is_connected()) {
      this->base_->init();
      this->base_->add_handler(this);
      this->web_server_started_ = true;
      ESP_LOGI(TAG, "web server started, /screenshot.bmp now registered");
    }
    if (this->capture_requested_.exchange(false)) {
      ESP_LOGI(TAG, "loop() saw capture_requested_, forcing lv_refr_now()");
      this->capturing_ = true;
      auto *disp = lv_display_get_default();
      if (disp != nullptr) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
      }
      this->capturing_ = false;
      // The one signal handleRequest() can safely wait on before freeing
      // this capture's buffers/queues - see the file-level comment.
      this->capture_done_ = true;
      ESP_LOGI(TAG, "capture burst finished");
    }
  }

  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->method() == HTTP_GET && request->url_to(buf) == "/screenshot.bmp";
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    // Reject a second concurrent request outright: this is a single-
    // capture-at-a-time design (see the file-level "known rough edges").
    bool expected = false;
    if (!this->request_in_progress_.compare_exchange_strong(expected, true)) {
      ESP_LOGW(TAG, "handleRequest: capture already in progress, rejecting concurrent request");
      httpd_resp_set_status(*request, "503 Service Unavailable");
      httpd_resp_send(*request, "Screenshot capture already in progress; try again shortly.",
                       HTTPD_RESP_USE_STRLEN);
      return;
    }
    // Guards the ENTIRE function, including the wait-for-producer-to-finish
    // step at the very end - see the file-level comment on why
    // request_in_progress_ must not release before capture_done_ is true.
    AtomicFlagGuard in_progress_guard(&this->request_in_progress_);

    ESP_LOGI(TAG, "handleRequest: GET /screenshot.bmp");

    uint8_t *band_bufs[NUM_BANDS] = {};  // value-initializes every element to nullptr regardless of NUM_BANDS
    QueueHandle_t free_q = nullptr;
    QueueHandle_t full_q = nullptr;
    bool setup_ok = true;
    for (int i = 0; i < NUM_BANDS && setup_ok; i++) {
      band_bufs[i] = (uint8_t *) heap_caps_malloc((size_t) BATCH_ROWS * ROW_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      setup_ok = band_bufs[i] != nullptr;
    }
    if (setup_ok) {
      free_q = xQueueCreate(NUM_BANDS, sizeof(Band));
      full_q = xQueueCreate(NUM_BANDS, sizeof(Band));
      setup_ok = free_q != nullptr && full_q != nullptr;
    }
    if (!setup_ok) {
      ESP_LOGE(TAG, "handleRequest: allocation failed, rejecting request");
      for (int i = 0; i < NUM_BANDS; i++)
        if (band_bufs[i] != nullptr)
          heap_caps_free(band_bufs[i]);
      if (free_q != nullptr)
        vQueueDelete(free_q);
      if (full_q != nullptr)
        vQueueDelete(full_q);
      httpd_resp_set_status(*request, "503 Service Unavailable");
      httpd_resp_send(*request, "Not enough free heap for a capture right now; try again shortly.",
                       HTTPD_RESP_USE_STRLEN);
      return;
    }
    for (int i = 0; i < NUM_BANDS; i++) {
      Band b{band_bufs[i], 0};
      xQueueSend(free_q, &b, 0);
    }
    // Published for on_flush_ (producer, other task) to use. Safe without
    // extra synchronization: these plain writes happen-before the
    // capture_requested_ store below, and on_flush_ never runs until loop()
    // has observed that store via its own exchange() - see the file-level
    // comment.
    this->free_q_ = free_q;
    this->full_q_ = full_q;
    this->collecting_ = true;

    uint8_t header_buf[HEADER_BYTES];  // stack local, not a member - see its declaration-site comment below
    this->build_header_(header_buf);
    this->capture_done_ = false;
    this->capture_requested_ = true;

    httpd_req_t *req = *request;
    httpd_resp_set_type(req, "image/bmp");
    // An agent re-fetching this endpoint to verify a just-made layout
    // change getting back a cached response would be a silent wrong
    // answer - the one failure mode this dev tool must never produce.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    bool client_ok = httpd_resp_send_chunk(req, (const char *) header_buf, HEADER_BYTES) == ESP_OK;
    if (!client_ok)
      ESP_LOGW(TAG, "client disconnected before any rows were sent (header chunk failed)");

    uint32_t deadline = millis() + CAPTURE_BUDGET_MS;
    int32_t rows_sent = 0;
    while (rows_sent < SCREEN_H) {
      if ((int32_t) (millis() - deadline) >= 0) {
        ESP_LOGW(TAG, "capture deadline exceeded with %d/%d rows sent, padding and closing", (int) rows_sent,
                 (int) SCREEN_H);
        break;
      }
      Band band;
      if (xQueueReceive(full_q, &band, pdMS_TO_TICKS(500)) != pdTRUE)
        continue;  // nothing ready yet - loop back and re-check the deadline
      if (client_ok) {
        if (httpd_resp_send_chunk(req, (const char *) band.buf, (size_t) band.rows * ROW_BYTES) != ESP_OK) {
          ESP_LOGW(TAG, "client disconnected mid-capture at row %d", (int) rows_sent);
          client_ok = false;
        }
      }
      // Even once client_ok is false, keep draining full_q_ and handing
      // buffers back rather than bailing immediately: on_flush_ blocks
      // (briefly, bounded - see there) trying to hand off a full band, so a
      // consumer that stops entirely would stall the producer for no
      // reason right up until it notices collecting_ went false anyway.
      // Draining costs nothing extra and lets the render finish sooner.
      rows_sent += band.rows;
      band.rows = 0;
      xQueueSend(free_q, &band, 0);
    }
    this->collecting_ = false;  // on_flush_ notices this and stops producing - see there

    // Wait for the producer to fully stop BEFORE touching any band buffer
    // again - including reusing one for the padding fill below - or
    // freeing anything. Doing this any later (e.g. only right before the
    // free calls) would leave a window where the padding code below and
    // on_flush_ could both be writing into band_bufs[0] at once. Normally
    // near-instant: the burst that produced the rows we already sent has,
    // by construction, usually already returned from lv_refr_now() by the
    // time we get here, or does within a few ms. In the deadline/disconnect
    // case, on_flush_ notices collecting_ went false and stops within one
    // retry interval (a few hundred ms - see there), so this is a real
    // wait in that case, not a formality, but it's bounded and runs on the
    // httpd worker task, not the main loop task, so nothing else on the
    // device stalls while it waits.
    uint32_t stop_deadline = millis() + 5000;
    while (!this->capture_done_.load() && (int32_t) (millis() - stop_deadline) < 0)
      vTaskDelay(pdMS_TO_TICKS(10));
    bool producer_stopped = this->capture_done_.load();
    if (!producer_stopped)
      ESP_LOGE(TAG, "producer did not finish within the cleanup safety window");

    if (rows_sent < SCREEN_H && client_ok && producer_stopped) {
      // Pad the unsent remainder with an unmistakably-synthetic fill
      // (magenta) rather than leaving the BMP's header promising more
      // bytes than the body has - most decoders reject a short BMP
      // outright, so an otherwise-fine partial capture would be totally
      // unusable instead of "clearly complete at the top, obviously
      // synthetic at the bottom." Skipped in the (should-not-happen)
      // producer_stopped==false case - touching band_bufs[0] wouldn't be
      // safe there either.
      uint8_t *pad = band_bufs[0];
      for (size_t p = 0; p + 2 < (size_t) BATCH_ROWS * ROW_BYTES; p += 3) {
        pad[p + 0] = 0xFF;  // B
        pad[p + 1] = 0x00;  // G
        pad[p + 2] = 0xFF;  // R
      }
      while (rows_sent < SCREEN_H) {
        int32_t rows = std::min(BATCH_ROWS, SCREEN_H - rows_sent);
        if (httpd_resp_send_chunk(req, (const char *) pad, (size_t) rows * ROW_BYTES) != ESP_OK) {
          client_ok = false;
          break;
        }
        rows_sent += rows;
      }
    }

    ESP_LOGI(TAG, "capture done: sent %d/%d rows%s", (int) rows_sent, (int) SCREEN_H,
             client_ok ? "" : " (client disconnected)");
    if (client_ok)
      httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response

    this->free_q_ = nullptr;
    this->full_q_ = nullptr;
    if (producer_stopped) {
      for (int i = 0; i < NUM_BANDS; i++)
        heap_caps_free(band_bufs[i]);
      vQueueDelete(free_q);
      vQueueDelete(full_q);
    } else {
      // Leaking these buffers once is far better than freeing memory a
      // still-running task might dereference next.
      ESP_LOGE(TAG, "leaking this capture's buffers rather than risk a use-after-free");
    }
  }

 protected:
  web_server_base::WebServerBase *base_{nullptr};
  lv_display_flush_cb_t orig_flush_cb_{nullptr};
  std::atomic<bool> capture_requested_{false};  // consumer -> producer: "please run a capture"
  std::atomic<bool> capture_done_{false};       // producer -> consumer: "that capture's burst is fully finished"
  std::atomic<bool> collecting_{false};         // consumer -> producer: "I'm still pulling bands, keep going"
  std::atomic<bool> request_in_progress_{false};
  // Set/read only from loop()/on_flush_, which always run on the main loop
  // task (on_flush_ only fires here via the synchronous lv_refr_now() call
  // loop() itself makes) - plain bool, not atomic, is correct.
  bool capturing_{false};
  bool web_server_started_{false};
  bool flush_hooked_{false};
  // header_buf_ is deliberately NOT a member (an earlier version made it
  // one): it's only ever touched within a single handleRequest() call, so
  // there's no reason to pay for it permanently - it's a stack local in
  // handleRequest() instead, freed the instant that call returns.
  //
  // Valid only while a capture is in flight - set by handleRequest() before
  // capture_requested_, cleared after capture_done_ (see there for why that
  // ordering is safe without extra locking).
  QueueHandle_t free_q_{nullptr};
  QueueHandle_t full_q_{nullptr};
  // Producer-side in-progress band, persists across on_flush_ calls when a
  // flush area's row range doesn't land exactly on a BATCH_ROWS boundary.
  uint8_t *cur_band_buf_{nullptr};
  int32_t cur_band_rows_{0};

  static void flush_trampoline(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    if (g_screenshot_instance != nullptr)
      g_screenshot_instance->on_flush_(disp, area, px_map);
  }

  void on_flush_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // CRITICAL: orig_flush_cb_ must be called unconditionally, on every
    // flush, capturing or not - it drives the SPI panel and calls
    // lv_display_flush_ready() to tell LVGL the flush finished. Everything
    // above the final call below is capture-only side effect; nothing here
    // may return without reaching it (hence the goto rather than early
    // returns from the nested loops below - see done_capturing).
    if (this->capturing_) {
      int32_t w = area->x2 - area->x1 + 1;
      if (area->x1 != 0 || w != SCREEN_W || area->y1 < 0 || area->y2 >= SCREEN_H) {
        // Only expected during the one full-screen invalidate this
        // component triggers itself. Anything narrower or out of range
        // means either an unrelated flush slipped past the capturing_
        // gate, or SCREENSHOT_SCREEN_W/H don't match the live display's
        // real resolution - either way, skip it rather than queue
        // partially-uninitialized or out-of-range row data.
        ESP_LOGW(TAG, "skipping unexpected flush area (%d,%d)-(%d,%d) during capture; expected full %dx%d",
                 (int) area->x1, (int) area->y1, (int) area->x2, (int) area->y2, (int) SCREEN_W, (int) SCREEN_H);
      } else {
        ESP_LOGD(TAG, "flush: area (%d,%d)-(%d,%d)", (int) area->x1, (int) area->y1, (int) area->x2, (int) area->y2);
        for (int32_t y = area->y1; y <= area->y2; y++) {
          // Wait for a free band buffer, re-checking collecting_ each
          // attempt so a consumer that gave up is noticed within one retry
          // interval (a few hundred ms) instead of only after this whole
          // row range finishes.
          while (this->cur_band_buf_ == nullptr) {
            if (!this->collecting_.load()) {
              this->cur_band_buf_ = nullptr;
              this->cur_band_rows_ = 0;
              goto done_capturing;
            }
            Band grabbed;
            if (xQueueReceive(this->free_q_, &grabbed, pdMS_TO_TICKS(200)) == pdTRUE) {
              this->cur_band_buf_ = grabbed.buf;
              this->cur_band_rows_ = 0;
            }
            App.feed_wdt();
          }

          const uint8_t *sp = px_map + (size_t) (y - area->y1) * w * 2;
          uint8_t *dp = this->cur_band_buf_ + (size_t) this->cur_band_rows_ * ROW_BYTES;
          for (int32_t x = 0; x < SCREEN_W; x++) {
            uint16_t px = ((uint16_t) sp[0] << 8) | sp[1];
            sp += 2;
            uint8_t r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
            dp[0] = (uint8_t) ((b5 << 3) | (b5 >> 2));
            dp[1] = (uint8_t) ((g6 << 2) | (g6 >> 4));
            dp[2] = (uint8_t) ((r5 << 3) | (r5 >> 2));
            dp += 3;
          }
          this->cur_band_rows_++;

          if (this->cur_band_rows_ == BATCH_ROWS || y == SCREEN_H - 1) {
            Band full{this->cur_band_buf_, this->cur_band_rows_};
            this->cur_band_buf_ = nullptr;
            this->cur_band_rows_ = 0;
            bool sent = false;
            while (!sent) {
              if (!this->collecting_.load())
                goto done_capturing;
              if (xQueueSend(this->full_q_, &full, pdMS_TO_TICKS(250)) == pdTRUE)
                sent = true;
              App.feed_wdt();
            }
          }
          App.feed_wdt();
        }
      }
    }
  done_capturing:
    if (this->orig_flush_cb_ != nullptr)
      this->orig_flush_cb_(disp, area, px_map);
  }

  void build_header_(uint8_t *buf) {
    uint32_t file_size = HEADER_BYTES + IMAGE_BYTES;
    uint32_t image_size = IMAGE_BYTES;
    int32_t neg_height = -SCREEN_H;
    int32_t width = SCREEN_W;
    uint32_t off_bits = HEADER_BYTES;
    uint32_t info_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 24;
    uint32_t compression = 0;
    int32_t zero32 = 0;

    size_t i = 0;
    buf[i++] = 'B';
    buf[i++] = 'M';
    memcpy(buf + i, &file_size, 4); i += 4;
    memset(buf + i, 0, 4); i += 4;
    memcpy(buf + i, &off_bits, 4); i += 4;
    memcpy(buf + i, &info_size, 4); i += 4;
    memcpy(buf + i, &width, 4); i += 4;
    memcpy(buf + i, &neg_height, 4); i += 4;
    memcpy(buf + i, &planes, 2); i += 2;
    memcpy(buf + i, &bpp, 2); i += 2;
    memcpy(buf + i, &compression, 4); i += 4;
    memcpy(buf + i, &image_size, 4); i += 4;
    memcpy(buf + i, &zero32, 4); i += 4;
    memcpy(buf + i, &zero32, 4); i += 4;
    memcpy(buf + i, &zero32, 4); i += 4;
    memcpy(buf + i, &zero32, 4); i += 4;
  }
};

}  // namespace screenshot
}  // namespace esphome
