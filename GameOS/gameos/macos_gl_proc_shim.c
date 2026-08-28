// macos-port: route GLEW's GL entry-point resolution through eglGetProcAddress.
//
// The macOS GL path runs on Mesa's Zink (see CMakeLists.txt "macos-port" and
// dev/macos-run.sh). GLEW resolves entry points with dlsym(), which only finds
// the symbols Mesa's libGL flat-exports; it misses functions Mesa exposes only
// via eglGetProcAddress (glClipControl, the direct-state-access calls,
// glShaderStorageBlockBinding, ...). Those come back NULL and the engine then
// aborts (e.g. the clip_control fail-closed gate) or crashes on first call.
//
// A dyld interpose on dlsym redirects gl* name lookups to eglGetProcAddress
// (the full context ABI) and passes everything else through to the real dlsym.
// Linked into the mc2 executable, so no DYLD_INSERT_LIBRARIES is needed. The
// interposing image's own dlsym references are not redirected by dyld, so the
// fallback call does not recurse.
#if defined(__APPLE__)
#include <dlfcn.h>
#include <stdlib.h>

typedef void *(*mc2_proc_t)(const char *);
static mc2_proc_t mc2_egl_get_proc_address;
static int mc2_resolved;

static int mc2_is_gl_name(const char *n) {
    return n && n[0] == 'g' && n[1] == 'l' && n[2] >= 'A' && n[2] <= 'Z';
}

static void mc2_resolve_egl(void) {
    mc2_resolved = 1;
    // SDL dlopen()s Mesa's libEGL RTLD_LOCAL, so it is not in the global
    // (RTLD_DEFAULT) scope. Try that first anyway, then dlopen the same libEGL
    // ourselves (SDL_EGL_LIBRARY names it; see dev/macos-run.sh) to reach the
    // full-ABI eglGetProcAddress.
    mc2_egl_get_proc_address = (mc2_proc_t)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    if (mc2_egl_get_proc_address)
        return;
    const char *paths[3];
    paths[0] = getenv("SDL_EGL_LIBRARY");
    paths[1] = "/opt/homebrew/opt/mesa/lib/libEGL.dylib";
    paths[2] = "libEGL.dylib";
    for (int i = 0; i < 3; i++) {
        if (!paths[i] || !paths[i][0])
            continue;
        void *h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            mc2_egl_get_proc_address = (mc2_proc_t)dlsym(h, "eglGetProcAddress");
            if (mc2_egl_get_proc_address)
                return;
        }
    }
}

static void *mc2_dlsym(void *handle, const char *name) {
    if (!mc2_resolved)
        mc2_resolve_egl();
    if (mc2_egl_get_proc_address && mc2_is_gl_name(name)) {
        void *p = mc2_egl_get_proc_address(name);
        if (p)
            return p;
    }
    return dlsym(handle, name);
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} _mc2_interpose_dlsym __attribute__((section("__DATA,__interpose"))) = {
    (const void *)mc2_dlsym, (const void *)dlsym};
#endif // __APPLE__
