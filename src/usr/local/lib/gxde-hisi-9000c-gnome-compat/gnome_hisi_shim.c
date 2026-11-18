/*
 * gnome_hisi_shim.c — extra compatibility for GNOME/Mutter Wayland on
 * HiSilicon Kirin 9000c (Maleoon 910) with the GXDE hisi GPU compat layer.
 *
 * Used together with the package-provided libhisi_gbm_shim.so:
 *   LD_PRELOAD="/usr/local/lib/gxde-hisi-9000c-gnome-compat/libgnome_hisi_shim.so \
 *               /usr/lib/gxde-hisi-9000c-gpu-compat/libhisi_gbm_shim.so"
 *
 * This is an additive companion shim.  It does NOT replace or modify the
 * GXDE shim: hisi_gbm_shim.c stays byte-for-byte identical, and GXDE
 * sessions keep loading only libhisi_gbm_shim.so.  The two shims export
 * disjoint symbols, so the GNOME launcher preloads this one first.
 *
 * Additions over the GXDE shim:
 *   - gbm_bo_get_bpp / gbm_device_get_format_modifier_plane_count:
 *     libmutter-16.so.0 is linked with BIND_NOW and needs both symbols,
 *     but the vendor libGFX_hisi gbm implementation does not export them.
 *   - eglQueryString: advertise EGL_EXT_platform_base / EGL_KHR_platform_gbm
 *     (Mutter refuses the GBM renderer without EGL_KHR_platform_gbm, even
 *     though the vendor eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR) works).
 *   - eglGetProcAddress: make libepoxy's cached eglQueryString pointer go
 *     through the interposed eglQueryString above.
 *   - gbm_surface_create_with_modifiers: apply the same {INVALID}/{LINEAR}
 *     modifier-list workaround as the GXDE shim does for gbm_bo_create_*.
 *   - hide EGL_KHR_partial_update / EGL_HUAWEI_partial_update and the
 *     swap-with-damage extensions, because hisi does not implement
 *     EGL_EXT_buffer_age.  Cogl then does full redraws + plain
 *     eglSwapBuffers, which fixes shaking/tearing on window and cursor
 *     movement.  GNOME_HISI_KEEP_PARTIAL_UPDATE=1 restores the old list.
 *
 * Build:
 *   gcc -O2 -shared -fPIC -o libgnome_hisi_shim.so gnome_hisi_shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gbm_device;
struct gbm_bo;
struct gbm_surface;

typedef void *EGLDisplay;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void (*EGLProc)(void);

#define EGL_EXTENSIONS 0x3055
#define EGL_PLATFORM_GBM_KHR 0x31D7

#define SHIM_DRM_FORMAT_MOD_LINEAR  0ULL
#define SHIM_DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)

/* ------------------------------------------------------------------ */
/* Modifier-list workaround (same logic as the GXDE shim).            */
/* ------------------------------------------------------------------ */
static _Thread_local uint64_t hisi_fixed_mods[32];

static const uint64_t *hisi_fixup_modifier_list(const uint64_t *modifiers,
        uint32_t count, uint32_t width, uint32_t height, uint32_t *out_count)
{
    *out_count = count;
    if (!modifiers || count == 0 || count > 32)
        return modifiers;

    bool has_invalid = false, has_linear = false, has_real = false;
    for (uint32_t i = 0; i < count; i++) {
        if (modifiers[i] == SHIM_DRM_FORMAT_MOD_INVALID)
            has_invalid = true;
        else if (modifiers[i] == SHIM_DRM_FORMAT_MOD_LINEAR)
            has_linear = true;
        else
            has_real = true;
    }

    if (has_real)
        return modifiers;

    if (!has_linear) {
        if (count == 1 && has_invalid && !(width == 1 && height == 1)) {
            hisi_fixed_mods[0] = SHIM_DRM_FORMAT_MOD_LINEAR;
            *out_count = 1;
            return hisi_fixed_mods;
        }
        return modifiers;
    }

    uint32_t n = 0;
    hisi_fixed_mods[n++] = SHIM_DRM_FORMAT_MOD_LINEAR;
    if (has_invalid)
        hisi_fixed_mods[n++] = SHIM_DRM_FORMAT_MOD_INVALID;
    *out_count = n;
    return hisi_fixed_mods;
}

struct gbm_surface *
gbm_surface_create_with_modifiers(struct gbm_device *dev,
        uint32_t width, uint32_t height, uint32_t format,
        const uint64_t *modifiers, uint32_t count)
{
    static struct gbm_surface *(*real_fn)(struct gbm_device *, uint32_t,
        uint32_t, uint32_t, const uint64_t *, uint32_t);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "gbm_surface_create_with_modifiers");
    if (!real_fn) {
        /* Some vendor builds only provide gbm_surface_create(). */
        struct gbm_surface *(*plain_fn)(struct gbm_device *, uint32_t,
            uint32_t, uint32_t);
        if (!plain_fn)
            plain_fn = (void *)dlsym(RTLD_NEXT, "gbm_surface_create");
        if (!plain_fn)
            return NULL;
        return plain_fn(dev, width, height, format);
    }

    uint32_t fixed_count = 0;
    const uint64_t *fixed =
        hisi_fixup_modifier_list(modifiers, count, width, height, &fixed_count);
    return real_fn(dev, width, height, format, fixed, fixed_count);
}

/* ------------------------------------------------------------------ */
/* Missing Mesa/GBM helpers needed by libmutter.                      */
/* ------------------------------------------------------------------ */
int gbm_bo_get_bpp(struct gbm_bo *bo)
{
    static int (*real_fn)(struct gbm_bo *);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "gbm_bo_get_bpp");
    if (real_fn)
        return real_fn(bo);

    static uint32_t (*get_format_fn)(struct gbm_bo *);
    if (!get_format_fn)
        get_format_fn = (void *)dlsym(RTLD_NEXT, "gbm_bo_get_format");
    uint32_t format = get_format_fn ? get_format_fn(bo) : 0;

    switch (format) {
    /* 32bpp packed RGB(A) */
    case 0x34325241: /* ARGB8888 */
    case 0x34325258: /* XRGB8888 */
    case 0x34324241: /* ABGR8888 */
    case 0x34324258: /* XBGR8888 */
    case 0x34324152: /* RGBA8888 */
    case 0x34324142: /* BGRA8888 */
    case 0x34325852: /* RGBX8888 */
    case 0x34325842: /* BGRX8888 */
    case 0x30335241: /* ARGB2101010 */
    case 0x30335258: /* XRGB2101010 */
    case 0x30334241: /* ABGR2101010 */
    case 0x30334258: /* XBGR2101010 */
        return 32;
    /* 24bpp */
    case 0x34324752: /* BGR888 */
    case 0x34324742: /* RGB888 */
        return 24;
    /* 16bpp */
    case 0x36314752: /* RGB565 */
    case 0x36314742: /* BGR565 */
        return 16;
    /* 48bpp */
    case 0x38345241: /* ARGB16161616 */
    case 0x38345258: /* XRGB16161616 */
    case 0x38344241: /* ABGR16161616 */
    case 0x38344258: /* XBGR16161616 */
        return 48;
    /* YUV 4:2:0: 12 bits per pixel on average */
    case 0x3231564e: /* NV12 */
    case 0x32315659: /* YV12 */
    case 0x31315649: /* I420 */
        return 12;
    default:
        return 32;
    }
}

int gbm_device_get_format_modifier_plane_count(struct gbm_device *dev,
        uint32_t format, uint64_t modifier)
{
    static int (*real_fn)(struct gbm_device *, uint32_t, uint64_t);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT,
                                "gbm_device_get_format_modifier_plane_count");
    if (real_fn)
        return real_fn(dev, format, modifier);

    /* libGFX_hisi only exposes single-plane buffers for its supported
     * linear/AFBC-ish formats. */
    (void)dev; (void)format; (void)modifier;
    return 1;
}

/* ------------------------------------------------------------------ */
/* EGL extension advertisement.                                       */
/*                                                                    */
/* hisi advertises EGL_KHR_partial_update but NOT EGL_EXT_buffer_age.  */
/* Cogl treats KHR_partial_update as if buffer-age support existed,    */
/* then queries EGL_BUFFER_AGE_EXT, which the hisi driver does not     */
/* implement.  The resulting damage/damage-history handling produces   */
/* shaking/tearing while moving windows or the cursor.  Hide the       */
/* partial-update and swap-with-damage extensions so Mutter/Cogl do a  */
/* full redraw + plain eglSwapBuffers every frame.  This is the same   */
/* kind of extension-level workaround as the GXDE GBM shim.            */
/*                                                                    */
/* Set GNOME_HISI_KEEP_PARTIAL_UPDATE=1 to restore the driver's        */
/* advertised extensions for comparison.                              */
/* ------------------------------------------------------------------ */
static bool
keep_partial_update (void)
{
    const char *env = getenv ("GNOME_HISI_KEEP_PARTIAL_UPDATE");
    return env && env[0] != '\0' && strcmp (env, "0") != 0;
}

static void
remove_extension (char *buf, const char *ext)
{
    size_t ext_len = strlen (ext);
    char *p = buf;

    while ((p = strstr (p, ext)) != NULL) {
        bool at_start = (p == buf || p[-1] == ' ');
        bool at_end = (p[ext_len] == '\0' || p[ext_len] == ' ');
        if (at_start && at_end) {
            char *src = p + ext_len;
            if (*src == ' ')
                src++;
            memmove (p, src, strlen (src) + 1);
        } else {
            p += ext_len;
        }
    }
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    static const char *(*real_fn)(EGLDisplay, EGLint);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "eglQueryString");

    const char *s = real_fn ? real_fn(dpy, name) : NULL;
    if (name != EGL_EXTENSIONS || !s)
        return s;

    static _Thread_local char client_buf[16384];
    static _Thread_local char display_buf[16384];
    char *buf = dpy ? display_buf : client_buf;
    snprintf(buf, sizeof(display_buf), "%s", s);

    if (!strstr(buf, "EGL_KHR_platform_gbm")) {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len,
                 " EGL_EXT_platform_base EGL_KHR_platform_base "
                 "EGL_KHR_platform_gbm EGL_MESA_platform_gbm");
    }

    if (!keep_partial_update()) {
        remove_extension(buf, "EGL_KHR_partial_update");
        remove_extension(buf, "EGL_HUAWEI_partial_update");
        remove_extension(buf, "EGL_KHR_swap_buffers_with_damage");
        remove_extension(buf, "EGL_EXT_swap_buffers_with_damage");
    }

    return buf;
}

EGLProc eglGetProcAddress(const char *name)
{
    static EGLProc (*real_fn)(const char *);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "eglGetProcAddress");

    if (name) {
        if (strcmp(name, "eglQueryString") == 0)
            return (EGLProc)eglQueryString;
        if (strcmp(name, "eglGetPlatformDisplay") == 0) {
            EGLProc p = real_fn ? real_fn(name) : NULL;
            if (p)
                return p;
            return (EGLProc)dlsym(RTLD_NEXT, "eglGetPlatformDisplay");
        }
    }
    return real_fn ? real_fn(name) : NULL;
}
