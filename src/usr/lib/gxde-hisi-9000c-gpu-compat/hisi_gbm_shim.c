/*
 * hisi_gbm_shim.c — LD_PRELOAD shim filling the gaps in hisi libGFX_hisi
 * for wlcom/gxde-wlcom + Xwayland + Qt/GLES clients on the live desktop:
 *
 *   gbm_bo_create_with_modifiers2  (Xwayland/glamor needs)
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
#include <stdio.h>
#include <stdarg.h>

struct gbm_device;
struct gbm_bo;

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
    /* flags (GBM_BO_CREATE_*) unused by hisi path; forward the rest */
    return real_fn(dev, width, height, format, modifiers, count);
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
