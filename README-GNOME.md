# GNOME/Mutter Wayland branch for gxde-hisi-9000c-gpu-compat

Status: verified on a real Kirin 9000c / Maleoon 910 device.

- Device: HiSilicon Kirin 9000c, aarch64, GXDE OS 25 / Debian 13
- Kernel: `5.10.97-38-9000c`
- GPU: Maleoon 910 (`libGFX_hisi.so.0.8.0`, EGL vendor HUAWEI)
- Display: `2560x1440@59.951`, `hisi-dpu`, atomic KMS
- GNOME: Mutter 48.7, Wayland only, Xwayland on demand

This branch adds a GNOME/Mutter Wayland session on top of the GXDE
hisi-9000c GPU compatibility layer.  It does **not** replace GXDE and it
does **not** change the default GXDE session.

## Branch and base

- Base: `master` at `4d73d10` (original GXDE compatibility package)
- Branch: `gnome-wayland-compat`
- Design: additive companion shim, not a fork of the original shim

## What was edited in the original shim

Nothing.

`src/usr/lib/gxde-hisi-9000c-gpu-compat/hisi_gbm_shim.c` is byte-for-byte
identical to `master`.  It was not patched, renamed, or rebuilt with new
behavior.  GXDE sessions still load only
`libhisi_gbm_shim.so`, so GXDE's runtime path is unchanged.

The GNOME work lives in a new companion shim:

`src/usr/local/lib/gxde-hisi-9000c-gnome-compat/gnome_hisi_shim.c`

The companion shim reuses the same `{INVALID}` / `{LINEAR}` modifier-list
algorithm as the original, but it exports a disjoint set of symbols.  The
GNOME launcher preloads the companion first, then the original:

```sh
LD_PRELOAD="/usr/local/lib/gxde-hisi-9000c-gnome-compat/libgnome_hisi_shim.so \
            /usr/lib/gxde-hisi-9000c-gpu-compat/libhisi_gbm_shim.so"
```

No symbol is defined by both shims, so load order is safe and the original
GXDE shim keeps working exactly as before.

## Original project vs this branch

| Item                      | `master` (GXDE)                  | `gnome-wayland-compat`                     |
| ------------------------- | -------------------------------- | ------------------------------------------ |
| Original shim             | `hisi_gbm_shim.c`                | unchanged                                  |
| GNOME companion shim      | absent                           | `gnome_hisi_shim.c`                        |
| Default session           | GXDE via `startgxde_wlcom_9000c` | still GXDE                                 |
| GNOME session             | absent                           | optional `gnome-hisi-9000c.desktop`        |
| Xwayland                  | `xwayland-mesa` wrapper          | same for GXDE; GNOME wrapper via diversion |
| GXDE modifier workaround  | present                          | unchanged                                  |
| GNOME modifier workaround | absent                           | `gnome_hisi_shim.c` + Mutter env           |
| GXDE package behavior     | reference                        | unchanged except additive files            |

Existing files touched only for packaging plumbing:

- `debian/rules`: builds the extra companion `.so`; original build unchanged
- `debian/control`: mentions the optional GNOME session; dependencies unchanged
- `.gitignore`: ignores the new built `.so`
- `README.md`: link to this document
- new `debian/preinst`, `debian/postrm`: divert `/usr/bin/Xwayland`

## Shim comparison

| Symbol / behavior | Original `hisi_gbm_shim.c` | GNOME `gnome_hisi_shim.c` |
|---|---|---|
| `gbm_bo_create_with_modifiers` | fix `{INVALID}` / `{LINEAR}` | not present |
| `gbm_bo_create_with_modifiers2` | fix `{INVALID}` / `{LINEAR}` | not present |
| `gbm_bo_get_fd_for_plane` | fall back to `gbm_bo_get_fd` | not present |
| `eglGetPlatformDisplay` | forward to `eglGetPlatformDisplayEXT` | not present |
| `wl_display_create_queue_with_name` | forward | not present |
| `wl_surface_interface` | v5/v6 data-symbol interposition | not present |
| `gbm_surface_create_with_modifiers` | not present | apply same modifier fix |
| `gbm_bo_get_bpp` | not present | provide missing Mesa helper |
| `gbm_device_get_format_modifier_plane_count` | not present | provide missing Mesa helper |
| `eglQueryString` | not present | add platform GBM, hide partial-update |
| `eglGetProcAddress` | not present | route `eglQueryString` |
| Extension filtering | not present | hide partial-update/damage |

The original shim is a **server/GBM compatibility** shim.  The companion is a
**Mutter/Cogl compatibility** shim.  They are intentionally separate so the
GXDE behavior cannot regress.

## Why GNOME needed extra work

1. **Missing GBM helper symbols**

   `libmutter-16.so.0` is linked with `BIND_NOW` and references
   `gbm_bo_get_bpp()` and `gbm_device_get_format_modifier_plane_count()`.
   `libGFX_hisi` does not export them, so the shell would fail to load.
   The companion shim provides both.

2. **Mutter refuses the GBM renderer without `EGL_KHR_platform_gbm`**

   The vendor EGL works through `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR)`,
   but its extension string does not advertise the standard platform
   extensions.  `eglQueryString(EGL_EXTENSIONS)` now appends
   `EGL_EXT_platform_base`, `EGL_KHR_platform_base`,
   `EGL_KHR_platform_gbm`, and `EGL_MESA_platform_gbm`.
   `eglGetProcAddress()` routes the cached `eglQueryString` pointer used by
   libepoxy through the interposed function.

3. **Modifier lists**

   The hisi GBM/EGL stack returns broken buffers for some modifier lists,
   especially `{INVALID}` and `{INVALID, LINEAR}`.  The companion shim applies
   the original GXDE modifier-list workaround to
   `gbm_surface_create_with_modifiers()`; Mutter also runs with

   ```sh
   MUTTER_DEBUG_USE_KMS_MODIFIERS=0
   MUTTER_DEBUG_SEND_KMS_MODIFIERS=0
   ```

   which mirror GXDE's `WLR_EGL_NO_MODIFIERS=1` /
   `KYWC_EGL_NO_MODIFIERS=1`.

4. **Shaking/tearing root cause**

   The hisi EGL advertises:

   - `EGL_KHR_partial_update`
   - `EGL_HUAWEI_partial_update`
   - `EGL_KHR_swap_buffers_with_damage`
   - `EGL_EXT_swap_buffers_with_damage`

   but it does **not** implement `EGL_EXT_buffer_age`.  Cogl treats
   `partial_update` as evidence that buffer age exists, then queries
   `EGL_BUFFER_AGE_EXT`.  The hisi driver returns an unusable result, so
   Cogl's damage history is wrong.  That produced the shaking/tearing while
   moving windows or the cursor.

   The companion shim removes the partial-update and swap-with-damage
   extensions from `eglQueryString(EGL_EXTENSIONS)`.  Cogl then falls back to
   a full redraw and plain `eglSwapBuffers()` every frame.  This is the
   effective fix; it is not a Mutter setting.

   Set `GNOME_HISI_KEEP_PARTIAL_UPDATE=1` to restore the driver's original
   extension list for comparison/debugging.

5. **Cursor / KMS mode**

   The hisi DPU does not expose a usable cursor plane.  GXDE's
   `gxde-wlroots` fix (`16c20c3` + `5d6f314`) adds a fake cursor plane only
   when `drmModeSetCursor()` succeeds.  On this device it fails with
   `No such device or address` (ENXIO), which aborts `Page flip failed` /
   `Failed to post KMS update`.  GNOME therefore keeps the atomic path and a
   software cursor.  `GNOME_HISI_FORCE_LEGACY=1` is only for experiments and
   is not the default.

## Chrome / Electron: use the render node, not card0

Chrome can still show software rendering even when GNOME itself is on
Maleoon 910.  The GPU process must use the DRM render node:

```text
/dev/dri/renderD128
```

Do **not** force the primary node:

```text
/dev/dri/card0
```

On this hisi DPU, GEM buffer creation on `card0` returns
`DRM_IOCTL_HISI_GEM_CREATE ... (error=-13)` unless the process is DRM master.
Chrome's `GbmPixmapWayland` then fails to allocate
`RGBA_8888` with `usage=Scanout|Rendering|Texturing`, SharedImage creation
fails, and the GPU process crashes three times.  Chrome disables GPU for that
browser process and falls back to software.  `chrome://gpu` then shows:

```text
GPU process crash count: 3
GPU process was unable to boot: GPU access is disabled due to frequent crashes.
```

The render node has no such restriction; `gbmtest` confirms `RGBA8888` with
`SCANOUT|RENDERING` works on `renderD128` and fails on `card0`.

Correct launch:

```sh
google-chrome-stable --ozone-platform=wayland \
    --render-node-override=/dev/dri/renderD128 \
    --disable-accelerated-video-decode
```

The `--disable-accelerated-video-decode` flag is required for Twitch/H.264
playback.  The hisi VA-API decoder initializes (`LIBVA_DRIVER_NAME=vacore`,
`/usr/lib/vacore_drv_video.so`), but Chrome's zero-copy path tries to import
the decoder output into EGL and the hisi EGL rejects the format:

```text
==>[ERROR] (chrome) EGL:
gbm format invalid
```

With hardware video decode enabled, Twitch requests segments and the audio
pipeline starts, but no usable `<video>` frame is presented.  With software
decode, the same Twitch page reaches `readyState=4`, `paused=false`,
`currentTime` advances, and a screenshot shows the actual stream.  This is a
decoder-output/EGL-import incompatibility, not a missing VA-API driver.

The `vacore` decoder itself is fine.  Direct VA-API decode works:

```text
LIBVA_DRIVER_NAME=vacore LIBVA_DRIVERS_PATH=/usr/lib \
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
    -i test_h264.mp4 -f null -
...
150 frames decoded; 0 decode errors
pix_fmt: vaapi -> nv12
```

So the blocker is Chrome importing the VA-API `NV12` surface into the hisi
EGL path, not the decoder.

Vulkan video decode is not an alternative on this stack:

- `libhvgr.so` exposes no `VK_KHR_video_queue`,
  `VK_KHR_video_decode_h264`, or `VK_EXT_video_*` strings/extensions.
- Chrome's media stack contains `VaapiVideoDecoder` only; it has no Vulkan
  video decoder.  Chrome's Vulkan/Dawn path is for WebGPU/compute, not media
  decode.
- Zink is OpenGL-over-Vulkan, not a video decoder.

A hardware-decode fix therefore needs an EGL dma-buf import shim that accepts
the `vacore` decoder output (NV12), or a Chrome path that copies instead of
zero-copy importing.  `--disable-gpu-memory-buffer-video-frames` also avoids
the failure but does not keep the hardware decoder active.

The branch installs desktop overrides at
`src/usr/local/share/applications/google-chrome.desktop` and
`src/usr/local/share/applications/com.google.Chrome.desktop` so GUI and
portal launches use the render node and software video decode automatically.
The `com.google.Chrome` entry stays `NoDisplay=true` to avoid a duplicate menu
entry while keeping the portal/default-browser application ID.  A command-line
launch that still has `--render-node-override=/dev/dri/card0` must be closed
and restarted; Chrome keeps the crash/blacklist state for the life of the
browser process.

GNOME Shell caches application info.  After changing the desktop files, run
`update-desktop-database ~/.local/share/applications` and, if the app grid
still launches the old command, log out/in once.  The user-level entries
shadow the package-owned `/usr/share/applications` entries; no root edit is
needed.

Verified result with `renderD128` + software video decode:

```text
Canvas: Hardware accelerated
Compositing: Hardware accelerated
OpenGL: Enabled
Rasterization: Hardware accelerated
Video Decode: Software only (decoder-output/EGL-import workaround)
WebGL: Hardware accelerated
WebGPU: Hardware accelerated
GL_RENDERER: ANGLE (HUAWEI, Maleoon 910, OpenGL ES 3.2 B250)
GPU process crash count: 0
Twitch: readyState=4, paused=false, currentTime advancing, stream visible
```

The debug tools used for this are in `~/9000c/gbmtest.c`,
`~/9000c/drm_nodes.c`, `~/9000c/chrome_gpu_probe_dom.py`,
`~/9000c/twitch_probe.py`, and `~/9000c/twitch_quality.py`.

## Files in this branch

Added:

- `src/usr/local/lib/gxde-hisi-9000c-gnome-compat/gnome_hisi_shim.c`
- `src/usr/local/bin/startgnome_hisi_9000c`
- `src/usr/share/wayland-sessions/gnome-hisi-9000c.desktop`
- `src/usr/local/share/applications/google-chrome.desktop`
- `src/usr/local/share/applications/com.google.Chrome.desktop`
- `src/usr/bin/Xwayland` (environment-filtering wrapper)
- `debian/preinst`, `debian/postrm` (Xwayland diversion)
- `README-GNOME.md`

Modified:

- `debian/rules` (build the companion shim; original build unchanged)
- `debian/control` (mention the optional GNOME session)
- `.gitignore` (ignore the companion `.so`)
- `README.md` (link to this document)

The original shim and original GXDE launcher are untouched.

## GXDE compatibility

The following guarantees hold on this branch:

- `hisi_gbm_shim.c` is unchanged from `master`.
- GXDE continues to load only `libhisi_gbm_shim.so`.
- The GNOME companion shim is loaded only by
  `/usr/local/bin/startgnome_hisi_9000c`.
- `debian/postinst` is unchanged, so GXDE remains the default gxdm session.
- GXDE still uses `WLR_XWAYLAND=/usr/lib/gxde-hisi-9000c-gpu-compat/xwayland-mesa`
  and is not affected by the `/usr/bin/Xwayland` diversion.
- The diversion wrapper only strips the hisi compat entries from
  `LD_PRELOAD` / `LD_LIBRARY_PATH`; it does not change the real Xwayland
  binary or other sessions' environment.

## Build and install

```sh
git switch gnome-wayland-compat
sudo apt install build-essential debhelper xwayland
dpkg-buildpackage -b -us -uc -d
sudo dpkg -i ../gxde-hisi-9000c-gpu-compat_*.deb
```

The package now installs both shims, the GNOME session entry, the launcher,
and the Xwayland wrapper.  `debian/preinst` diverts the distribution
`/usr/bin/Xwayland` to `/usr/bin/Xwayland.real`; `debian/postrm` restores it
on remove/purge.

Manual build of just the companion shim:

```sh
gcc -O2 -shared -fPIC \
    -o libgnome_hisi_shim.so \
    src/usr/local/lib/gxde-hisi-9000c-gnome-compat/gnome_hisi_shim.c -ldl
```

## Session launcher environment

`/usr/local/bin/startgnome_hisi_9000c` sets:

```sh
LD_LIBRARY_PATH=/usr/lib/gxde-hisi-9000c-gpu-compat/server-egl
LD_PRELOAD=/usr/local/lib/gxde-hisi-9000c-gnome-compat/libgnome_hisi_shim.so \
           /usr/lib/gxde-hisi-9000c-gpu-compat/libhisi_gbm_shim.so
XDG_SESSION_TYPE=wayland
XDG_SESSION_DESKTOP=gnome
XDG_CURRENT_DESKTOP=GNOME
GDK_BACKEND=wayland
MUTTER_DEBUG_USE_KMS_MODIFIERS=0
MUTTER_DEBUG_SEND_KMS_MODIFIERS=0
```

Optional debugging override: `GNOME_HISI_KEEP_PARTIAL_UPDATE=1` restores
the driver's partial-update/damage extensions.  It is logged by the launcher
as `GNOME_HISI_KEEP_PARTIAL_UPDATE=...`.

It also unsets `DISPLAY`, `WAYLAND_DISPLAY`, and `XAUTHORITY`, and logs to
`~/.local/state/gnome-hisi-9000c-session.log` by default.

`MUTTER_DEBUG_PAINT=disable-direct-scanout` is still exported by the launcher
for older/newer Mutter experiments, but Mutter 48.7 does not parse that
variable.  The extension filter in the companion shim is what actually fixes
the shaking.

## Verification

Observed on the real device:

```text
GL_RENDERER=Maleoon 910
EGL vendor=HUAWEI
Added device '/dev/dri/card0' (hisi-dpu) using atomic mode setting
Created gbm renderer
Obtained a high priority EGL context
```

The filtered extension list no longer contains
`EGL_KHR_partial_update`, `EGL_HUAWEI_partial_update`,
`EGL_KHR_swap_buffers_with_damage`, or
`EGL_EXT_swap_buffers_with_damage`.

The user confirmed after installing the new shim: **no picture shaking**.

Useful checks:

```sh
# Session log
tail -n 80 ~/.local/state/gnome-hisi-9000c-session.log

# Extension list seen by a GNOME client
EGL_PLATFORM=gbm gnometest

# Mutter / KMS state
sudo journalctl -b -u gdm -u gnome-shell --no-pager | tail -n 100
```

`eglQueryDisplayAttribEXT` may still produce the non-fatal warning:

```text
Failed to query EGL device from primary EGL display:
EGL proc 'egl->eglQueryDisplayAttribEXT' not resolved
```

That warning does not prevent the hardware renderer from starting.

## Known limitations

- Xwayland runs on system Mesa (`llvmpipe`), matching GXDE's partial-Xwayland
  model.  X11 applications do not use Maleoon 910.
- Hardware cursor is not used; the cursor is composited in software.
- `GNOME_HISI_FORCE_LEGACY=1` is not usable on this DPU because
  `drmModeSetCursor()` returns ENXIO and aborts the KMS update.
- `MUTTER_DEBUG_PAINT=disable-direct-scanout` is ineffective on Mutter 48.7.
- Direct scanout must not be re-enabled without re-testing the partial-update
  extension filter.
- `journalctl --user` is permission-blocked on this image; use `sudo journalctl`.
- Chrome must use `/dev/dri/renderD128`.  If `chrome://gpu` still shows
  software, check the `Command Line` line for
  `--render-node-override=/dev/dri/card0` and restart Chrome after changing it.
- Twitch/H.264 playback requires `--disable-accelerated-video-decode` on this
  driver stack.  The `vacore` VA-API decoder output cannot be imported by the
  hisi EGL path (`gbm format invalid`), so hardware video decode is currently
  not usable for Twitch.  GPU compositing and WebGL still use Maleoon 910.
- This shim does not change Mesa's Vulkan/Zink driver selection.  Minecraft
  using Vulkan/Zink and falling back to llvmpipe is a separate Mesa/Vulkan
  issue (`VK_ICD_FILENAMES`, `MESA_LOADER_DRIVER_OVERRIDE`, and the hisi
  `libhvgr` ICD), not the GNOME Cogl/EGL path.

## Rollback

```sh
sudo apt remove gxde-hisi-9000c-gpu-compat
```

`debian/postrm` removes the Xwayland diversion and restores the distribution
binary.  GXDE users who never select the GNOME session are unaffected even
while the branch package is installed.

To disable only the shaking fix for comparison:

```sh
GNOME_HISI_KEEP_PARTIAL_UPDATE=1 /usr/local/bin/startgnome_hisi_9000c
```

## Commit summary

This branch should be reviewed as:

```sh
git diff --stat master..gnome-wayland-compat
git diff master..gnome-wayland-compat -- \
    src/usr/lib/gxde-hisi-9000c-gpu-compat/hisi_gbm_shim.c
```

The second command must be empty.  That is the GXDE compatibility contract.
