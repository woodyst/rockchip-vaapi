# Developer documentation — rockchip-vaapi

**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com

---

## Project overview

`rockchip-vaapi` is a VA-API 1.20 driver (`rockchip_drv_video.so`) that
bridges the `libva` API to the Rockchip MPP (Media Process Platform) library.
It was written from scratch because no open VA-API driver exists for the
Rockchip RK3588 SoC.

### Why a VA-API driver and not GStreamer or V4L2?

- **GStreamer**: Firefox 128+ has no GStreamer media backend. The code path
  (`dom/media/platforms/gstreamer/`) was removed; only the VA-API PDM remains.
- **V4L2**: Rockchip exposes a V4L2 interface, but it uses virtual M2M devices
  that are incompatible with Firefox's V4L2 backend (which expects real
  `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` M2M).
- **VA-API**: Firefox uses `libva` for hardware decode on Linux via its
  VA-API Platform Decode Module (PDM), running inside the RDD (Remote Data
  Decoder) sandboxed process.

---

## Source tree

```
rockchip-vaapi/
├── src/
│   ├── rockchip_drv_video.c   # Main driver: full VA-API vtable
│   ├── h264.c                 # H.264 SPS/PPS Annex B reconstruction
│   ├── h264.h
│   └── bs.h                   # Header-only Exp-Golomb bitstream writer
├── debian/                    # Debian packaging metadata
├── docs/
│   └── DEVELOPMENT.md         # This file
├── Makefile
├── README.md                  # English
├── README.es.md               # Español
└── INSTALL.md
```

---

## Architecture

```
Firefox (RDD process)
    │
    │  VA-API calls (libva 1.20)
    ▼
rockchip_drv_video.so          ← this driver
    │
    │  MppApi calls (librockchip-mpp1)
    ▼
Rockchip MPP
    │
    │  kernel ioctl
    ▼
RK3588 VPU (hardware)
    │
    │  DMA-BUF fd (PRIME 2)
    ▼
Firefox compositor (zero-copy NV12)
```

### Object model

The driver maintains four ID-namespaced flat arrays in a single `RKDriver`
heap allocation (`ctx->pDriverData`):

| Array | Max | ID base | Struct |
|-------|-----|---------|--------|
| `configs` | 16 | `0x01000000` | `RKConfig` |
| `contexts` | 8 | `0x02000000` | `RKContext` |
| `surfaces` | 64 | `0x03000000` | `RKSurface` |
| `buffers` | 256 | `0x04000000` | `RKBuffer` |

---

## VA-API vtable (`VADriverVTable`)

The entry point `__vaDriverInit_1_20` fills all 50+ function pointers. The
functions that actually do meaningful work are:

| Function | Role |
|----------|------|
| `rk_CreateConfig` | Validate profile/entrypoint; allocate `RKConfig` |
| `rk_CreateContext` | `mpp_create` + `mpp_init`; allocate `RKContext` |
| `rk_CreateSurfaces2` | Allocate `RKSurface` slots; surface dimensions stored |
| `rk_CreateBuffer` | `malloc` + copy caller data into `RKBuffer` |
| `rk_BeginPicture` | Store render target in `RKContext` |
| `rk_RenderPicture` | Collect buffer IDs into `pending[]` |
| `rk_EndPicture` | Dispatch to `do_h264_decode()` (or future codec dispatch) |
| `rk_QuerySurfaceAttrs` | Report NV12 + `DRM_PRIME_2` support (critical for Firefox) |
| `rk_ExportSurfaceHandle` | Return `VADRMPRIMESurfaceDescriptor` with `dup()`'d DMABUF fd |
| `rk_SyncSurface` | Wait on `surface->cond` until `decoded == true` |
| `rk_Terminate` | Destroy MPP contexts; close fds; `free(RKDriver)` |

All other vtable slots are filled with stubs that return `VA_STATUS_SUCCESS`
(or `VA_STATUS_ERROR_UNIMPLEMENTED` where appropriate) to satisfy libva's
initialization checks.

---

## H.264 decode pipeline

### The SPS/PPS problem

VA-API decouples parameter parsing from decoding. Firefox (via FFmpeg) parses
the H.264 bitstream and extracts the SPS and PPS as C structs
(`VAPictureParameterBufferH264`, `VAIQMatrixBufferH264`), then passes them via
`vaRenderPicture`. It does **not** pass the original binary NALUs.

MPP's `decode_put_packet` requires a complete Annex B bitstream including SPS
and PPS NALUs prepended before each IDR slice. The driver must therefore
**reconstruct** the binary SPS/PPS from the parsed structs before each IDR
frame.

### SPS/PPS reconstruction (`h264.c`)

`h264_write_sps()` and `h264_write_pps()` encode the structs back to binary
using Exp-Golomb coding:

1. A `BSWriter` (from `bs.h`) writes individual fields as `u(n)`, `ue(v)` or
   `se(v)` elements into a temporary `raw[]` buffer.
2. The NAL header byte is written first (`0x67` for SPS, `0x68` for PPS).
3. High-profile SPS includes `chroma_format_idc` and bit-depth fields.
4. `emulation_prevent()` scans the raw bytes and inserts `0x03` bytes before
   any `0x000001` or `0x000002` sequences (required by the H.264 spec).
5. A 4-byte Annex B start code (`00 00 00 01`) is prepended to the output.

### PPS `num_ref_idx` defaults (since 1.0.12)

`VAPictureParameterBufferH264` does not carry the PPS fields
`num_ref_idx_l0_default_active_minus1` / `num_ref_idx_l1_default_active_minus1`.
They are not cosmetic: a slice whose header has
`num_ref_idx_active_override_flag = 0` uses them to parse
`ref_pic_list_modification()` and `pred_weight_table()`. Up to 1.0.11 the
driver wrote 0 for both, so MPP misparsed every non-overriding P/B slice of
streams with a different default (YouTube 720p60 H.264 uses l0 = 2 with
explicit weighted prediction). The symptom was output drifting to stale or
corrupt frames after a few hundred frames, then a `SyncSurface` timeout that
Firefox reports as `NS_ERROR_DOM_MEDIA_FATAL_ERR`.

The fix recovers the original values from the stream itself:

1. `h264_slice_ref_override()` (`h264.c`) parses the start of each slice
   header (with emulation-prevention bytes removed) up to
   `num_ref_idx_active_override_flag`. It uses the SPS/PPS fields in the VA
   picture parameters to know the width of `frame_num`, `pic_order_cnt_lsb`
   and the other optional fields.
2. For a slice that does **not** override, the effective counts in its
   `VASliceParameterBufferH264` (`num_ref_idx_l0/l1_active_minus1`) are, by
   definition, the PPS defaults. The context stores them in
   `pps_l0_def` / `pps_l1_def` (l1 is learned only from B slices).
3. `do_h264_decode` sends the SPS+PPS before each IDR as before, and resends
   the PPS alone before any picture where the learned defaults change.

I slices carry no reference counts, so the placeholder 0 written before the
first P/B slice is harmless.

### MPP output order (`fast_out`)

By default MPP outputs H.264 frames in display order and holds back as many
frames as its DPB allows, which can be more than the client's own reorder
delay (`max_num_reorder_frames`). A client that calls `vaSyncSurface` on a
surface MPP is still holding waits for output, while MPP waits for more
input: a deadlock that ends in a timeout. Since 1.0.12 the driver sets
`base:fast_out = 1` for H.264, so each frame is emitted as soon as it is
decoded; the client (FFmpeg) already does its own reordering, and frames are
routed back to their surface by PTS. Without it, frames are lost on B-frame
content.

### Slice dispatch (`do_h264_decode`)

For each `EndPicture` call the driver:

1. Finds the `VAPictureParameterBufferH264` and all `VASliceDataBuffer` blobs
   in `pending[]`.
2. On IDR frames (detected via `nal_unit_type`), prepends SPS+PPS; resends
   the PPS whenever the learned `num_ref_idx` defaults change.
3. Prepends `00 00 00 01` start codes to each slice NALU.
4. Packs everything into a single MPP packet; encodes the target surface ID
   as the packet `pts` (used to route the decoded frame back to the right
   surface).
5. Calls `mpi->decode_put_packet`.
6. Polls `mpi->decode_get_frame` in a loop; matches the output frame by `pts`
   to find the target surface.
7. Stores the `MppFrame` in `surface->frame`, `dup()`s the DMABUF fd into
   `surface->prime_fd`, then signals `surface->cond`.

---

## DRM PRIME 2 surface export

Firefox calls `vaExportSurfaceHandle` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` to get a zero-copy handle to the
decoded frame's GPU memory.

`rk_ExportSurfaceHandle` returns a `VADRMPRIMESurfaceDescriptor`:

```c
desc->fourcc    = VA_FOURCC_NV12;
desc->width     = surface->width;
desc->height    = surface->height;
desc->num_layers = 2;
// Layer 0: Y plane
desc->layers[0].drm_format = DRM_FORMAT_R8;
desc->layers[0].object_index[0] = 0;
desc->layers[0].offset[0] = 0;
desc->layers[0].pitch[0]  = hstride;
// Layer 1: UV plane
desc->layers[1].drm_format = DRM_FORMAT_GR88;
desc->layers[1].object_index[0] = 0;
desc->layers[1].offset[0] = hstride * vstride;
desc->layers[1].pitch[0]  = hstride;
// Object (shared DMABUF)
desc->objects[0].fd             = dup(surface->prime_fd);
desc->objects[0].size           = hstride * vstride * 3 / 2;
desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
```

The `dup()` is essential: Firefox closes the fd when done; the driver keeps the
original alive in `surface->prime_fd` (tied to `surface->frame`).

### Why `vaQuerySurfaceAttributes` matters

This is the function Firefox calls **before** attempting hardware decode to
check whether the driver supports DRM PRIME 2. If it returns 0 attributes
or omits `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`, Firefox silently falls back
to software decode. The driver must return:

```c
// Pixel format
attrs[0].type  = VASurfaceAttribPixelFormat;
attrs[0].value.value.i = VA_FOURCC_NV12;
// Memory type: both VA and DRM PRIME 2
attrs[1].type  = VASurfaceAttribMemoryType;
attrs[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA
                       | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
// Max dimensions
attrs[2].type  = VASurfaceAttribMaxWidth;
attrs[2].value.value.i = 7680;
attrs[3].type  = VASurfaceAttribMaxHeight;
attrs[3].value.value.i = 4320;
```

---

## Memory model and the CMA constraint (4K)

There are two distinct pools of memory in play, and confusing them leads to
chasing the wrong bug:

| Buffer | Allocated by | Backing memory | Must be contiguous? |
|--------|--------------|----------------|---------------------|
| Per-surface output buffer (`priv_buf`) | this driver, in `CreateSurfaces` via `mpp_buffer_group_get_internal(MPP_BUFFER_TYPE_DRM)` | **system** dma-heap (not CMA) | no |
| MPP decode DPB (reference + work frames) | MPP internally | **CMA** (the VPU does DMA into it) | **yes** |
| GPU compositor textures | Mali / Mesa in Firefox | **CMA** | **yes** |

The driver copies each decoded frame out of MPP's DPB buffer into the surface's
own `priv_buf` (in `assign_mpp_frame`) so that MPP can immediately recycle its
small internal pool — see *Per-surface output buffers* below. Because `priv_buf`
lives in system memory, the driver is **not** a significant CMA consumer; you
can allocate hundreds of MB of surfaces without touching CMA.

The CMA pressure comes entirely from **MPP's DPB + the GPU compositor**, which
share the single kernel CMA region (`cma=` on the kernel command line, default
256 MB on most RK3588 images). A 4K NV12 frame is ~12.5 MB; VP9 keeps ~10
references, so the DPB alone is ~125–200 MB. Add the GPU's 4K compositing
buffers and 256 MB is not enough.

**Symptom of CMA exhaustion (important — it does not look like a driver bug):**
the 4K context acknowledges `info_change`, decodes 70–80 frames cleanly
(`copied=1`, no `TIMEOUT`, no VA error), then Firefox reports
`NS_ERROR_DOM_MEDIA_FATAL_ERR` with **no driver-side error at all**. MPP cannot
surface a failed contiguous allocation through the libva API, so from the
driver's point of view every call succeeded. The standalone `va_barcode_test`
decodes the same 4K stream fine because it does no GPU compositing and therefore
never contends for CMA.

**Fix:** raise CMA to `cma=512M` (or more) on the kernel command line. This is a
platform/deployment setting, not a driver change. See `INSTALL.md`.

Contrast this with AV1, which is a genuine *parse* failure: AV1 never
acknowledges `info_change` and decodes zero frames, because VA-API hands MPP
headerless tile data instead of a full OBU bitstream. CMA exhaustion is the
opposite — decode works, then a resource runs out.

### Per-surface output buffers

Each `RKSurface` owns a permanent `priv_buf` allocated in `CreateSurfaces` and
kept for the surface's lifetime (its `prime_fd` never changes). `assign_mpp_frame`
copies MPP's decoded pixels into it using MPP's *real* hor/ver strides
(e.g. 3840×2176 for 4K — note the 2176 ver-stride padding) and then releases the
MPP frame immediately. This avoids two earlier bugs:

- **Buffer aliasing ("saltando frames")**: MPP's internal display pool is only
  ~3 buffers for 4K; exporting MPP's fd directly meant MPP overwrote a buffer the
  compositor was still showing. Copying into a dedicated per-surface buffer fixes
  this.
- **Stale altref export**: VP9 altref (hidden, `show_frame=0`) frames are decoded
  by MPP as references but **not** output via `decode_get_frame` in this
  environment. The driver therefore does **not** poll/block for them (blocking
  per altref accumulated latency until Firefox's pipeline underran); it sends the
  altref to MPP, marks the surface decoded immediately, and lets the permanent
  `priv_buf` keep `ExportSurfaceHandle` valid.

## `bs.h` — Exp-Golomb bitstream writer

A header-only, zero-dependency, inline bitstream writer:

```c
BSWriter bs;
bs_init(&bs, buf, buf_size);

bs_write(&bs, value, n_bits);   // write n_bits of value
bs_write1(&bs, bit);            // write single bit
bs_write_ue(&bs, val);          // write unsigned Exp-Golomb (ue(v))
bs_write_se(&bs, val);          // write signed Exp-Golomb (se(v))
bs_rbsp_trailing(&bs);          // write RBSP trailing bits (1 + zeros to byte boundary)
size_t n = bs_bytes(&bs);       // number of bytes written
```

Exp-Golomb encoding for `ue(v)`:

```
val++ → find bit length k of val → write k zeros → write val in k+1 bits
```

---

## Extending to other codecs

To add HEVC/VP9/AV1 decode, implement a `do_hevc_decode()` (etc.) function
analogous to `do_h264_decode()`:

1. In `rk_EndPicture`, dispatch on `ctx->coding`:
   ```c
   if (ctx->coding == MPP_VIDEO_CodingHEVC)
       return do_hevc_decode(d, ctx, target);
   ```
2. HEVC/VP9/AV1 use `VAPictureParameterBufferHEVC`, `VASliceParameterBufferHEVC`,
   etc. The header reconstruction challenge is codec-specific:
   - HEVC: reconstruct VPS/SPS/PPS NALUs (more complex than H.264)
   - VP9/AV1: no NAL structure; MPP accepts raw IVF/OBU frames directly

3. Update `rk_QueryConfigProfiles` and `rk_QueryConfigEntrypoints` to expose
   the new profile (already done in the current driver for all four codecs).

---

## Firefox process model

Firefox decodes video in the **RDD (Remote Data Decoder)** sandboxed process.
The VA-API driver runs inside RDD. By default, RDD's seccomp sandbox blocks
`/dev/dri` device access, which prevents MPP from opening the VPU. The
workaround is `MOZ_DISABLE_RDD_SANDBOX=1`.

For a hardened deployment, the correct long-term fix is to add `/dev/dri` and
Rockchip's `/dev/mpp_service` to Firefox's RDD sandbox allow-list (requires
patching Firefox's sandbox policy).

---

## Known limitations

- H.264 SPS reconstruction uses `level_idc=51` (5.1) unconditionally. The
  actual level is not exposed by `VAPictureParameterBufferH264`; 5.1 is safe
  for all content up to 4K@60fps.
- Maximum 64 surfaces / 8 concurrent decode contexts. Increase `MAX_SURFACES`
  and `MAX_CONTEXTS` if needed.
- Only H.264 header reconstruction is implemented. HEVC/VP9/AV1 decode paths
  call `do_h264_decode` as a stub; full implementation is pending.

---

## AI-assisted development notice

This driver was designed and implemented through interactive pair programming
with **Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), a large language
model developed by Anthropic.

**Development timeline:** 24 April 2026
**Estimated interactive development time:** ~3–4 hours across two sessions

**AI contributions:**
- Initial architecture design (VA-API vtable layout, object model, ID namespacing)
- Complete `rockchip_drv_video.c` implementation (~1,000 lines)
- H.264 Annex B SPS/PPS reconstruction algorithm (`h264.c`, `bs.h`)
- DRM PRIME 2 / DMABUF surface export implementation
- Iterative debugging: `MPP_FRAME_FLAG_EOS` compatibility, STUB macro issues,
  `max_subpic_formats` libva init requirement, `rk_QuerySurfaceAttrs`
  returning correct attributes for Firefox
- Debian package structure (`debian/`)

**Human contributions:**
- Problem identification and system analysis (RK3588 hardware context)
- Live testing on Orange Pi 5 Plus hardware
- Firefox `about:config` configuration and environment variable tuning
- Validation that video plays correctly with hardware acceleration active
- Code review and approval at each iteration
- License, authorship, and packaging decisions

All code was validated on real hardware by Eduardo García-Mádico Portabella —
EGP Sistemas.
