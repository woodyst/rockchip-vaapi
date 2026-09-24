# rockchip-vaapi

*English · [Español](README.es.md)*

VA-API driver for Rockchip RK3588 / RK3576 that bridges **libva** to
**librockchip-mpp** (MPP), enabling hardware-accelerated video decode in
applications such as Firefox.

**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com
**License:** LGPL-2.1-or-later

---

## What it does

The Rockchip RK3588 SoC includes a dedicated VPU capable of decoding H.264,
HEVC, VP9 and AV1 at up to 8K resolution. However, no vendor-supplied VA-API
driver exists for it. This project fills that gap by implementing the complete
`VADriverVTable` (VA-API 1.20) and forwarding decode work to the Rockchip MPP
library, which in turn uses the hardware VPU.

Key features:

- H.264 / HEVC / VP9 hardware decode (AV1 is not advertised, see below)
- Zero-copy DRM PRIME 2 surface export (NV12, DMABUF)
- Compatible with Firefox 128+ (VA-API PDM path, RDD process)
- Implements the full VA-API 1.20 vtable (`__vaDriverInit_1_20`)

## Supported hardware

| SoC | Board (tested) |
|-----|---------------|
| RK3588 | Orange Pi 5 Plus |
| RK3588S | Orange Pi 5 / Rock 5B (untested, should work) |
| RK3576 | Likely compatible (untested) |

## Supported codecs

| Codec | Profile | Max resolution |
|-------|---------|---------------|
| H.264 | Constrained Baseline, Main, High, High10 | 4K |
| HEVC | Main, Main10 | 8K |
| VP9 | Profile 0, 2 | 8K |

AV1 is **not advertised**: MPP needs a complete OBU bitstream, which VA-API
does not provide, so browsers fall back to VP9, which is hardware-decoded.

## What's new in 1.0.12

Fixes YouTube H.264 videos that kept reloading in Firefox
(`NS_ERROR_DOM_MEDIA_FATAL_ERR` in a loop):

- **H.264 PPS reference defaults.** The rebuilt PPS always said "1 reference
  by default". Streams with a different default (YouTube 720p60 uses 3, with
  weighted prediction) were misparsed by the VPU: after a few hundred frames
  the picture drifted and decoding timed out. The driver now learns the real
  defaults from the slice headers.
- **B-frame deadlock.** MPP held back frames the player was waiting for.
  H.264 frames are now emitted as soon as they are decoded.
- **Backpressure and logging** (from [PR #2](https://github.com/woodyst/rockchip-vaapi/pull/2)
  by TruongSinh Tran-Nguyen): a full MPP input queue is no longer a fatal
  error, and `RK_VAAPI_LOG` is flushed after every line.

Result on a YouTube 720p60 H.264 clip: 2107/2107 frames bit-identical to
software decoding (1.0.11: 506/2107). Details in
[debian/changelog](debian/changelog) and
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#pps-num_ref_idx-defaults-since-1012).

## Dependencies

Runtime:
- `libva2` (>= 2.0)
- `librockchip-mpp1`

Build:
- `libva-dev`
- `librockchip-mpp-dev`
- `pkg-config`, `gcc`

## Quick start

```bash
# Build and install
make
sudo make install

# Launch Firefox with hardware decode
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
MOZ_DISABLE_RDD_SANDBOX=1 \
firefox
```

In Firefox, also enable via `about:config`:

| Preference | Value |
|-----------|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Verifying hardware decode

Logging is off by default. Set `RK_VAAPI_LOG` to a file path, play a video,
and check the log:

```bash
LIBVA_DRIVER_NAME=rockchip MOZ_DISABLE_RDD_SANDBOX=1 RK_VAAPI_LOG=/tmp/rk.log firefox
# Should show BeginPicture, EndPicture, assign_mpp_frame, ExportSurfaceHandle
grep -c assign_mpp_frame /tmp/rk.log
```

If a video keeps reloading, count Firefox's decoder errors:

```bash
journalctl -b --since "-10min" | grep -c NS_ERROR_DOM_MEDIA_FATAL_ERR
```

You can also check VPU activity:

```bash
cat /sys/class/devfreq/*/cur_freq   # VPU frequency rises under load
```

## Permanent Firefox launcher

Create `/usr/local/bin/firefox-hw`:

```bash
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
```

```bash
chmod +x /usr/local/bin/firefox-hw
```

## Development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for architecture, VA-API
internals, MPP integration details, and how to add support for new codecs.

## AI-assisted development

This driver was designed and implemented with the assistance of
**Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), an AI model developed
by Anthropic. Total interactive development time: approximately **3–4 hours**
across two sessions (24 April 2026).

The AI assisted with: architecture design, VA-API vtable implementation,
H.264 Annex B SPS/PPS reconstruction via Exp-Golomb encoding, MPP API
integration, DMABUF/DRM PRIME 2 surface export, and iterative debugging of
Firefox integration issues.

The 1.0.12 H.264 fixes (PPS reference defaults, `fast_out`) were diagnosed
and implemented with **Claude Opus 5.5** (`claude-opus-5-5`) on 24 September
2026, by reproducing the Firefox failure with mpv and comparing every frame
against software decoding.

All code was reviewed, tested, and validated on real hardware by
Eduardo García-Mádico Portabella — EGP Sistemas.
