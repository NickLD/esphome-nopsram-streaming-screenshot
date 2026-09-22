# esphome-nopsram-streaming-screenshot

An ESPHome external_component that serves a live, pixel-perfect screenshot
of your LVGL UI over HTTP — **without needing PSRAM or a full frame
buffer**. Built so a coding agent (or you) can `curl` a real screenshot of
the physical panel's actual rendered state during development, instead of
asking for a photo.

```
GET http://<your-device>.local/screenshot.bmp
```
returns an uncompressed 24bpp BMP of whatever LVGL is currently displaying.
No auth, no query params. A request forces a synchronous redraw first, so
it always reflects live state, not a stale buffer.

## Why not an existing project?

Two other ESPHome/LVGL screenshot components already exist
([dcgrove/esphome-lvgl-screenshot](https://github.com/dcgrove/esphome-lvgl-screenshot),
[ay129-35MR/esphome-display-screenshot](https://github.com/ay129-35MR/esphome-display-screenshot))
— both explicitly require PSRAM (they hold a full frame buffer, or a full
JPEG-encode buffer, in memory). This component hooks LVGL's `flush_cb` and
streams the frame out row-by-row through a ~1.4KB producer/consumer buffer
instead, so it also works on a plain ESP32 with no PSRAM and very little
free heap — e.g. the ESP32-2432S028R "Cheap Yellow Display".

If you have PSRAM and want multi-page support or a JSON page-discovery
endpoint, ay129-35MR's project is a better fit than this one.

## Usage

```yaml
external_components:
  - source: github://NickLD/esphome-nopsram-streaming-screenshot
    components: [screenshot]

web_server_base:
  id: web_server_base_id

screenshot:
  id: my_screenshot
  web_server_base_id: web_server_base_id
  width: 320   # must match your LVGL logical canvas width (post-rotation)
  height: 240  # must match your LVGL logical canvas height (post-rotation)
```

See `examples/minimal.yaml` for a complete, working config.

### Config keys

| Key                 | Required | Description                                                        |
|----------------------|----------|----------------------------------------------------------------------|
| `id`                 | no       | Component ID, auto-generated if omitted.                            |
| `web_server_base_id` | yes      | ID of a `web_server_base:` instance to register the handler on.     |
| `width`              | yes      | LVGL logical canvas width in pixels (post-rotation, if rotated).     |
| `height`             | yes      | LVGL logical canvas height in pixels (post-rotation, if rotated).    |

## Known limitations

- **Single capture at a time.** A second concurrent request gets a 503.
  This is a dev tool for one person iterating on one device, not a
  multi-client server.
- **No auth, no TLS.** Don't expose this port beyond a trusted local
  network.
- **Captures LVGL's buffer, not the physical panel.** The screenshot is
  taken from LVGL's own rendered buffer via `flush_cb`, *before* your
  display driver's own color-order/MADCTL logic writes it to the physical
  screen over SPI. A display-driver-level bug (wrong rotation, a
  `color_order` mismatch) can make the real screen look different from —
  and worse than — what this endpoint shows. A clean screenshot proves
  LVGL's own rendering is correct; it does not prove the physical display
  is correct.
- **RGB565 byte-order assumption.** Verified working unmodified across two
  real devices with different display-driver `color_order` settings — but
  a non-default LVGL color depth/byte-order *build* setting (rare) is not
  defended against. See the comments in `screenshot.h` for detail.

## License

MIT — see [LICENSE](LICENSE).
