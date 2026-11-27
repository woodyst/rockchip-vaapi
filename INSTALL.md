# Installation guide — rockchip-vaapi

## System requirements

### Contiguous memory (CMA) — required for 4K

The RK3588 VPU decodes into **physically contiguous** memory (CMA —
Contiguous Memory Allocator), and the Mali GPU also draws compositing buffers
from the same CMA pool. A single 4K NV12 frame is ~12.5 MB and the VP9 decoder
keeps ~10 reference frames in flight (~125–200 MB), so 4K decode **plus** 4K GPU
compositing together need well over the default `cma=256M`.

With only 256 MB CMA, 4K playback in Firefox decodes correctly for a few
seconds and then dies with `NS_ERROR_DOM_MEDIA_FATAL_ERR` and falls back to
software — the VPU can no longer allocate a contiguous buffer once the GPU has
taken its share. (1080p frames are 4× smaller and never hit the limit.)

**Set `cma=512M`** (or higher) on the kernel command line and reboot:

- Armbian / mainline u-boot: edit `/boot/armbianEnv.txt`, add or change
  `extraargs=cma=512M` (append to any existing `extraargs`).
- extlinux: edit the `append` line in `/boot/extlinux/extlinux.conf`, add `cma=512M`.

Verify after reboot:

```bash
cat /proc/cmdline | tr ' ' '\n' | grep cma     # → cma=512M
grep CmaTotal /proc/meminfo                     # → CmaTotal: 524288 kB
```

> The driver's own per-surface buffers come from the *system* dma-heap (not CMA),
> so they are not the constraint; the CMA pressure is MPP's decode DPB plus the
> GPU compositor. See `docs/DEVELOPMENT.md` for details.

## Option A: Install from Debian package (recommended)

```bash
sudo dpkg -i rockchip-vaapi_1.0.1-1_arm64.deb
```

This installs `rockchip_drv_video.so` to
`/usr/lib/aarch64-linux-gnu/dri/` and registers the package with `dpkg`.
To uninstall: `sudo dpkg -r rockchip-vaapi`.

## Option B: Build and install from source

### 1. Install build dependencies

```bash
sudo apt install gcc pkg-config libva-dev librockchip-mpp-dev \
                 libx11-dev libegl-dev libgles2-dev
```

### 2. Build

```bash
cd /path/to/rockchip-vaapi
make
```

Expected output: `rockchip_drv_video.so` in the project root.

### 3. Install

```bash
sudo make install
# installs to /usr/lib/aarch64-linux-gnu/dri/
```

To install to a custom path:

```bash
sudo install -m 755 rockchip_drv_video.so /your/path/rockchip_drv_video.so
```

## Option C: Build your own Debian package

```bash
# Create orig tarball (exclude build artifacts and debian/)
tar --exclude='rockchip-vaapi/debian' \
    --exclude='rockchip-vaapi/*.so' \
    --exclude='rockchip-vaapi/src/*.o' \
    -czf rockchip-vaapi_1.0.1.orig.tar.gz rockchip-vaapi/

# Build binary package
cd rockchip-vaapi
dpkg-buildpackage -us -uc -b

# Build source package
dpkg-buildpackage -us -uc -S
```

## Verifying the installation

```bash
# Check the driver file is in place
ls -l /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so

# Verify VA-API sees the driver
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
vainfo
```

Expected `vainfo` output:

```
libva info: VA-API version 1.20.0
libva info: User environment variable requested driver 'rockchip'
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so
libva info: Found init function __vaDriverInit_1_20
...
VA profile VAProfileH264Main               : VAEntrypointVLD
VA profile VAProfileH264High               : VAEntrypointVLD
VA profile VAProfileHEVCMain               : VAEntrypointVLD
...
```

## Configuring Firefox

### Environment variables (required)

| Variable | Value | Reason |
|----------|-------|--------|
| `LIBVA_DRIVER_NAME` | `rockchip` | Selects this driver |
| `LIBVA_DRIVERS_PATH` | `/usr/lib/aarch64-linux-gnu/dri` | Driver search path |
| `MOZ_DISABLE_RDD_SANDBOX=1` | `1` | Allows RDD process to open `/dev/dri` |

### about:config (required)

Open `about:config` in Firefox and set:

| Key | Value |
|-----|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

### Permanent launcher

```bash
sudo tee /usr/local/bin/firefox-hw > /dev/null <<'EOF'
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
EOF
sudo chmod +x /usr/local/bin/firefox-hw
```

## Troubleshooting

**`vainfo` reports "driver not found"**
Verify `LIBVA_DRIVER_NAME=rockchip` and that the `.so` exists at
`/usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so`.

**Firefox still uses SWDEC**
Check `about:support` → Media → Video Decoder. If it shows `FFmpegVideo`,
hardware decode is active. If it shows `Softpipe` or similar, verify the three
`about:config` keys and that `MOZ_DISABLE_RDD_SANDBOX=1` is set.

**`/dev/dri` permission denied in RDD process**
Add your user to the `video` and `render` groups:
```bash
sudo usermod -aG video,render $USER
```
Then log out and back in (or use `newgrp video`).

**No frames decoded / black screen**
Enable verbose logging by setting `RK_VAAPI_LOG` to a file path:
```bash
LIBVA_DRIVER_NAME=rockchip RK_VAAPI_LOG=/tmp/rk.log MOZ_DISABLE_RDD_SANDBOX=1 firefox
tail -f /tmp/rk.log | grep -E "copied|TIMEOUT|ERROR|failed"
```
Look for errors after `BeginPicture` or `EndPicture`. Missing SPS/PPS or
MPP decode errors will appear there. Without `RK_VAAPI_LOG`, the driver
produces no output (logging is disabled by default for performance).

**4K plays for a few seconds then falls back to software
(`NS_ERROR_DOM_MEDIA_FATAL_ERR`)**
This is CMA exhaustion. The driver log will show frames decoded normally
(`copied=1`, no errors) for ~75 frames then a fatal error with no driver-side
cause — MPP cannot report a failed contiguous allocation through libva.
Increase CMA to `cma=512M` (see *System requirements* above).

Note: on some RK3588 boards the DTB hardcodes the CMA region with a fixed
physical address (`reg` property), which overrides any `cma=` command-line
parameter. If `grep CmaTotal /proc/meminfo` still shows 262144 kB after adding
`cma=512M`, patch the DTB directly:

```bash
sudo cp /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb.bak
sudo fdtput -t x /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /reserved-memory/cma reg 0x00 0x10000000 0x00 0x20000000
```

Then reboot and verify `grep CmaTotal /proc/meminfo` → `524288 kB`.
