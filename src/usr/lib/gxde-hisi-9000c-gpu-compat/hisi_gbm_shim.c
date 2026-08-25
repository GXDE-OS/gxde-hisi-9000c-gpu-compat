/*
 * hisi_gbm_shim.c — LD_PRELOAD shim filling the gaps in hisi libGFX_hisi
 * for wlcom/gxde-wlcom + Xwayland + Qt/GLES clients on the live desktop:
 *
 *   gbm_bo_create_with_modifiers2  (Xwayland/glamor needs)
 *   gbm_bo_create_with_modifiers   (hisi modifier-list quirk workaround)
 *   gbm_bo_get_fd_for_plane        (Xwayland/glamor needs)
 *   eglGetPlatformDisplay          (Qt5 wayland-egl plugin links this;
 *                                   hisi only exposes eglGetPlatformDisplayEXT
 *                                   via eglGetProcAddress)
 *
 * Build: gcc -O2 -shared -fPIC -o libhisi_gbm_shim.so hisi_gbm_shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

struct gbm_device;
struct gbm_bo;

/* ------------------------------------------------------------------ */
/* Kirin 9000c (Maleoon 910) gbm quirk workaround.                     */
/*                                                                    */
/* libGFX_hisi 的 gbm_bo_create_with_modifiers() 在 modifier 列表中   */
/* DRM_FORMAT_MOD_INVALID 排在 DRM_FORMAT_MOD_LINEAR 之前时会"成功"    */
/* 返回一个损坏的双平面 BO（modifier=INVALID、stride=0，驱动同时打印  */
/* "Multiplane buffers not supported"），随后按平面导出 dmabuf 必然    */
/* 失败。LINEAR 排在最前则得到正常的单平面线性 BO。                    */
/* 只含 {INVALID} 的列表会被驱动直接拒绝；改写为 {LINEAR}，让调用方   */
/* （如 wlroots 的 GBM 分配器）直接拿到可用的 BO。                     */
/* 含真实 modifier 的列表保持原样，不干扰驱动的正常选择。             */
/* ------------------------------------------------------------------ */
#define SHIM_DRM_FORMAT_MOD_LINEAR  0ULL
#define SHIM_DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)

static _Thread_local uint64_t hisi_fixed_mods[32];

static const uint64_t *hisi_fixup_modifier_list(const uint64_t *modifiers,
        uint32_t count, uint32_t width, uint32_t height, uint32_t *out_count)
{
    *out_count = count;
    if (!modifiers || count == 0 || count > 32) {
        return modifiers;
    }

    bool has_invalid = false, has_linear = false, has_real = false;
    for (uint32_t i = 0; i < count; i++) {
        if (modifiers[i] == SHIM_DRM_FORMAT_MOD_INVALID) {
            has_invalid = true;
        } else if (modifiers[i] == SHIM_DRM_FORMAT_MOD_LINEAR) {
            has_linear = true;
        } else {
            has_real = true;
        }
    }

    if (has_real) {
        /* 有真实 modifier，保持原样 */
        return modifiers;
    }
    if (!has_linear) {
        if (count == 1 && has_invalid && !(width == 1 && height == 1)) {
            /* 仅 {INVALID}：hisi 会拒绝，改写为 {LINEAR}。
             * 例外：Chromium/Electron 的 GPU 进程用 1x1 缓冲做
             * dmabuf 探针。若把探针改写成 {LINEAR}，驱动会造出一个
             * modifier=0 的 BO，随后 gbm_bo_import(FD_MODIFIER,
             * modifier=0) 永远失败，GPU 进程无限重试并刷屏
             * "modifier is 0"（clash-party 不加 --disable-gpu 打不开
             * 的根因）。1x1 探针必须保持失败，让 Chromium 走软件/
             * 回退路径。 */
            hisi_fixed_mods[0] = SHIM_DRM_FORMAT_MOD_LINEAR;
            *out_count = 1;
            return hisi_fixed_mods;
        }
        return modifiers;
    }

    /* LINEAR 提前、INVALID 殿后 */
    uint32_t n = 0;
    if (has_linear) {
        hisi_fixed_mods[n++] = SHIM_DRM_FORMAT_MOD_LINEAR;
    }
    if (has_invalid) {
        hisi_fixed_mods[n++] = SHIM_DRM_FORMAT_MOD_INVALID;
    }
    *out_count = n;
    return hisi_fixed_mods;
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *dev,
        uint32_t width, uint32_t height, uint32_t format,
        const uint64_t *modifiers, uint32_t count)
{
    static struct gbm_bo *(*real_fn)(struct gbm_device *, uint32_t, uint32_t,
        uint32_t, const uint64_t *, uint32_t);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "gbm_bo_create_with_modifiers");
    if (!real_fn) {
        errno = ENOSYS;
        return NULL;
    }

    uint32_t fixed_count = 0;
    const uint64_t *fixed =
        hisi_fixup_modifier_list(modifiers, count, width, height, &fixed_count);
    return real_fn(dev, width, height, format, fixed, fixed_count);
}

struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *dev,
        uint32_t width, uint32_t height, uint32_t format, uint32_t flags,
        const uint64_t *modifiers, uint32_t count)
{
    static struct gbm_bo *(*real_fn)(struct gbm_device *, uint32_t, uint32_t,
        uint32_t, const uint64_t *, uint32_t);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "gbm_bo_create_with_modifiers");
    if (!real_fn) {
        errno = ENOSYS;
        return NULL;
    }

    uint32_t fixed_count = 0;
    const uint64_t *fixed =
        hisi_fixup_modifier_list(modifiers, count, width, height, &fixed_count);
    /* flags (GBM_BO_CREATE_*) unused by hisi path; forward the rest */
    return real_fn(dev, width, height, format, fixed, fixed_count);
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    static int (*real_fn)(struct gbm_bo *);
    if (!real_fn)
        real_fn = (void *)dlsym(RTLD_NEXT, "gbm_bo_get_fd");
    if (!real_fn) {
        errno = ENOSYS;
        return -1;
    }
    if (plane != 0) {
        errno = EINVAL;
        return -1;
    }
    return real_fn(bo);
}

/* ------------------------------------------------------------------ */
/* EGL: provide the standard eglGetPlatformDisplay entry point that   */
/* Qt5's wayland-graphics-integration-client plugin links directly.   */
/* hisi libGFX_hisi only ships eglGetPlatformDisplayEXT (resolvable   */
/* through eglGetProcAddress), so forward the standard name to it.    */
/* ------------------------------------------------------------------ */
typedef intptr_t EGLAttrib;
typedef void *EGLDisplay;
typedef int EGLenum;

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
                                 const EGLAttrib *attrib_list)
{
    static EGLDisplay (*real_fn)(EGLenum, void *, const EGLAttrib *);
    if (!real_fn) {
        void *(*gpa)(const char *) = (void *)dlsym(RTLD_NEXT, "eglGetProcAddress");
        if (gpa)
            real_fn = (EGLDisplay (*)(EGLenum, void *, const EGLAttrib *))
                      gpa("eglGetPlatformDisplayEXT");
    }
    if (!real_fn) {
        errno = ENOSYS;
        return NULL;
    }
    return real_fn(platform, native_display, attrib_list);
}

/* ------------------------------------------------------------------ */
/* Wayland 1.23 API 缺失补丁: wl_display_create_queue_with_name.      */
/* Mesa 的 libEGL_mesa / libvulkan_lvp（软件 Vulkan）按 wayland 1.23  */
/* 编译，引用此符号；UOS 1.21 没有。真实实现：转发到                  */
/* wl_display_create_queue（name 参数仅用于调试，可忽略）。           */
/* ------------------------------------------------------------------ */
struct wl_event_queue *wl_display_create_queue_with_name(void *display,
                                                         const char *name)
{
    struct wl_event_queue *(*real_fn)(void *);
    real_fn = (struct wl_event_queue *(*)(void *))
              dlsym(RTLD_NEXT, "wl_display_create_queue");
    if (real_fn)
        return real_fn(display);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Wayland: fix wl_surface_interface missing events.                  */
/*                                                                     */
/* UOS 1.21's generated interface table is a *v5* wl_surface (only    */
/* enter/leave events). Qt6 (built against newer headers) binds        */
/* wl_compositor at v6, so the compositor sends preferred_buffer_scale */
/* (event 2) on v6 surfaces -> libwayland dies with                    */
/* "interface 'wl_surface' has no event 2".                           */
/*                                                                     */
/* Fix via DATA-symbol interposition: LD_PRELOAD re-exports            */
/* wl_surface_interface with event_count=4 and the two v6 events       */
/* added. Apps reference &wl_surface_interface when creating surfaces, */
/* so the proxy gets our corrected table.                              */
/*                                                                     */
/* IMPORTANT (2026-08-09): Firefox bundles its own wayland             */
/* implementation (libmozwayland.so, complete v6 wl_surface). The data */
/* interposition above preempts *its* wl_surface_interface too, which  */
/* made firefox crash in wl_proxy_marshal_constructor (NULL/garbage    */
/* methods table). So we prefer copying libmozwayland's own (complete) */
/* table verbatim when present, and only apply the v5->v6 upgrade to   */
/* the libwayland-client (UOS 1.21) fallback path. Also force-load the */
/* source lib with dlopen() so the constructor can never leave the     */
/* interposed table zero-initialized.                                  */
/* ------------------------------------------------------------------ */

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;
    int event_count;
    const struct wl_message *events;
};

static struct wl_message compat_surface_events[4];
struct wl_interface wl_surface_interface;

/* dlopen a lib if it exists (already-loaded libs are reused). */
static void *shim_dlopen_or_null(const char *soname)
{
    void *h = dlopen(soname, RTLD_LAZY | RTLD_NOLOAD);
    if (!h)
        h = dlopen(soname, RTLD_LAZY | RTLD_GLOBAL);
    return h;
}

__attribute__((constructor)) static void compat_surface_init(void)
{
    const struct wl_interface *real = NULL;
    int from_mozwayland = 0;

    /* 1) Firefox: 优先使用其自带 libmozwayland 的完整 v6 表（原样使用，
          避免数据符号拦截破坏 firefox 的 marshal）。 */
    void *h = shim_dlopen_or_null("libmozwayland.so");
    if (h && (real = dlsym(h, "wl_surface_interface")))
        from_mozwayland = 1;

    /* 2) 其他应用: libwayland-client (UOS 1.21, v5)，稍后升到 v6。 */
    if (!real) {
        h = shim_dlopen_or_null("libwayland-client.so.0");
        if (h)
            real = dlsym(h, "wl_surface_interface");
    }

    /* 3) 兜底: 正常 scope 查找。 */
    if (!real)
        real = dlsym(RTLD_NEXT, "wl_surface_interface");
    if (!real)
        return;

    wl_surface_interface = *real;

    /* firefox 的表已是完整 v6，直接原样使用。 */
    if (from_mozwayland)
        return;

    /* UOS 1.21 v5 -> v6: 补上两个 v6 事件。 */
    wl_surface_interface.version = 6;
    wl_surface_interface.event_count = 4;
    compat_surface_events[0] = real->events[0];   /* enter  */
    compat_surface_events[1] = real->events[1];   /* leave  */
    compat_surface_events[2].name = "preferred_buffer_scale";
    compat_surface_events[2].signature = "i";
    compat_surface_events[2].types = NULL;
    compat_surface_events[3].name = "preferred_buffer_transform";
    compat_surface_events[3].signature = "i";
    compat_surface_events[3].types = NULL;
    wl_surface_interface.events = compat_surface_events;
}
