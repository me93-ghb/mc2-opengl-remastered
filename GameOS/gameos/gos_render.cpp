#include "gos_render.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <GL/gl.h>
#include "utils/logging.h"
#ifdef MC2_VULKAN
#include "vulkan_backend_skeleton.h"
#endif

// macos-port: on-screen window path. Apple's Cocoa GL caps at 4.1, so the
// engine renders through Mesa Zink -> kosmickrisp -> Metal on a MANUAL EGL
// surfaceless/pbuffer context (created here, not by SDL), while a real SDL
// *cocoa* window supplies input + a Metal SDL_Renderer that presents each
// finished frame (glReadPixels of FBO 0 -> streaming texture). This keeps the
// full GL render path unchanged and sidesteps the two-GL-driver symbol clash a
// blit through Apple GL 4.1 would hit. Gated at runtime on the cocoa video
// driver, so the offscreen/headless path (smoke, framedump) is untouched.
// See macos-run.sh's MC2_MACOS_WINDOW mode for the env (curated Vulkan-only
// DYLD dir keeps Mesa's libGL off SDL's dlopen path, else NSOpenGLContext SIGBUS).
#if defined(__APPLE__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

// FIXME: think how to make it better when different parts need window
SDL_Window* g_sdl_window = NULL;
static SDL_GLContext g_sdl_glcontext = NULL;
static bool g_mouse_grabbed = false;

// GPU-VENDOR-DETECT-1: -1 = not yet probed, 0 = not NVIDIA, 1 = NVIDIA.
// Set once in init_render_context after the GL context is live. Read via
// gos_IsNvidiaGPU() from mclib (bdactor.cpp) to pick vendor-aware defaults.
int g_gosGpuIsNvidia = -1;
bool gos_IsNvidiaGPU() { return g_gosGpuIsNvidia == 1; }

namespace graphics {

static bool VERBOSE_VIDEO = true;   // unchanged (keep current behavior)
static bool VERBOSE_RENDER = false; // mode/driver/extensions dump silenced; GPU identity prints unconditionally
static bool VERBOSE_MODES = false;  // display-mode enumeration silenced
static bool ENABLE_VSYNC = false;  // default off; overridden by MC2_VSYNC env var in init_render_context

struct RenderWindow {
    SDL_Window* window_;
    int width_;
    int height_;
};

struct RenderContext {
   SDL_GLContext glcontext_;
   RenderWindow* render_window_;
};

#if defined(__APPLE__)
// macos-port: state for the cocoa-window present path (see header comment).
// active_ is set true in create_window when SDL selected the cocoa driver.
struct MacPresent {
    bool          active_   = false;
    EGLDisplay    dpy_      = EGL_NO_DISPLAY;
    EGLContext    ctx_      = EGL_NO_CONTEXT;
    EGLSurface    surf_     = EGL_NO_SURFACE;
    EGLConfig     cfg_      = nullptr;
    SDL_Renderer* renderer_ = nullptr;  // Metal-backed present
    SDL_Texture*  tex_      = nullptr;  // streaming; engine frame uploaded here
    int           w_        = 0;        // pbuffer / readback size (fixed)
    int           h_        = 0;
    unsigned char* rowbuf_  = nullptr;  // glReadPixels scratch (w*h*4)
};
static MacPresent g_mac;
static bool mac_resize(int w, int h);  // (re)size pbuffer + present texture
#endif

static void apply_mouse_grab(SDL_Window* window, bool grabbed)
{
#ifdef PLATFORM_WINDOWS
    if (!grabbed) {
        if (!ClipCursor(NULL)) {
            log_error("ClipCursor release failed: %lu\n", (unsigned long)GetLastError());
        }
    }
#endif

    if (!window) {
        return;
    }

    SDL_SetWindowMouseGrab(window, grabbed ? SDL_TRUE : SDL_FALSE);

#ifdef PLATFORM_WINDOWS
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE) {
        log_error("SDL_GetWindowWMInfo: %s\n", SDL_GetError());
        return;
    }

    if (wm_info.subsystem != SDL_SYSWM_WINDOWS || !wm_info.info.win.window) {
        return;
    }

    if (!grabbed) {
        return;
    }

    RECT client_rect;
    if (!GetClientRect(wm_info.info.win.window, &client_rect)) {
        log_error("GetClientRect failed: %lu\n", (unsigned long)GetLastError());
        return;
    }

    POINT top_left = { client_rect.left, client_rect.top };
    POINT bottom_right = { client_rect.right, client_rect.bottom };
    if (!ClientToScreen(wm_info.info.win.window, &top_left) ||
        !ClientToScreen(wm_info.info.win.window, &bottom_right)) {
        log_error("ClientToScreen failed: %lu\n", (unsigned long)GetLastError());
        return;
    }

    RECT clip_rect = {
        top_left.x,
        top_left.y,
        bottom_right.x,
        bottom_right.y,
    };

    if (!ClipCursor(&clip_rect)) {
        log_error("ClipCursor apply failed: %lu\n", (unsigned long)GetLastError());
    }
#endif
}

void set_mouse_grab(bool grabbed)
{
    g_mouse_grabbed = grabbed;
    apply_mouse_grab(g_sdl_window, grabbed);
}

void refresh_mouse_grab()
{
    apply_mouse_grab(g_sdl_window, g_mouse_grabbed);
}

static void PrintRenderer(SDL_RendererInfo * info);


//==============================================================================
void set_verbose(bool is_verbose)
{
    VERBOSE_VIDEO = is_verbose;
    VERBOSE_RENDER = is_verbose;
    VERBOSE_MODES = is_verbose;
}

//==============================================================================
RenderWindow* create_window(const char* pwinname, int width, int height)
{
	int i, j, m, n;
	SDL_DisplayMode fullscreen_mode;
    SDL_Window* window = NULL; 

    if (VERBOSE_VIDEO) {
        n = SDL_GetNumVideoDrivers();
        if (n == 0) {
            fprintf(stderr, "No built-in video drivers\n");
        } else {
            fprintf(stderr, "Built-in video drivers:");
            for (i = 0; i < n; ++i) {
                if (i > 0) {
                    fprintf(stderr, ",");
                }
                fprintf(stderr, " %s", SDL_GetVideoDriver(i));
            }
            fprintf(stderr, "\n");
        }
    }

    // initialize using 0 videodriver
    if (SDL_VideoInit(nullptr) < 0) {
        fprintf(stderr, "Couldn't initialize video driver: %s\n", SDL_GetError());
        return NULL;
    }
    if (VERBOSE_VIDEO) {
        fprintf(stderr, "Video driver: %s\n", SDL_GetCurrentVideoDriver());
    }

    //not really related to video, but let it be here
    if (VERBOSE_VIDEO) {
        printf("SDL revision: %s\n", SDL_GetRevision());

        SDL_version compiled;
        SDL_version linked;

        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        fprintf(stderr, "We compiled against SDL version %d.%d.%d \n",
            compiled.major, compiled.minor, compiled.patch);
        fprintf(stderr, "But we are linking against SDL version %d.%d.%d.\n",
           linked.major, linked.minor, linked.patch);
    }

    SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 16 );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    // 4x multisampling :P
    // disable, and add as setting later.
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);


    // select core profile if needed
    // COMPATIBILITY, ES,...
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    //SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    // 4.3 required for core SSBO (GL_SHADER_STORAGE_BUFFER) + std430 layout,
    // used by the GPU static-prop renderer. AMD RX 7900 XTX supports up to
    // 4.6 core; 4.3 is the minimum feature level we need.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    // MC2_GL_DEBUG=1 enables the OpenGL debug context AND the debug-message
    // callback (installed in gameosmain.cpp). Debug contexts run driver-side
    // validation on every GL call and can cost 10-30% perf, especially on
    // NVIDIA; the callback also floods stdout with harmless AMD-driver
    // warnings in our workload. Off by default in shipped builds;
    // env-gated rather than NDEBUG-gated so it can be flipped on a
    // deployed binary without rebuilding.
    const bool gl_debug = (getenv("MC2_GL_DEBUG") != nullptr);
    if (gl_debug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
        printf("[GL_DEBUG] MC2_GL_DEBUG=1 -- GL debug context active. This reduces performance.\n");
    }

    if (VERBOSE_MODES) {
        SDL_DisplayMode mode;
        int bpp;
        Uint32 Rmask, Gmask, Bmask, Amask;

        n = SDL_GetNumVideoDisplays();
        fprintf(stderr, "Number of displays: %d\n", n);
        for (i = 0; i < n; ++i) {
            fprintf(stderr, "Display %d:\n", i);

            SDL_GetDesktopDisplayMode(i, &mode);
            SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask, &Gmask,
                    &Bmask, &Amask);
            fprintf(stderr,
                    "  Current mode: %dx%d@%dHz, %d bits-per-pixel (%s)\n",
                    mode.w, mode.h, mode.refresh_rate, bpp,
                    SDL_GetPixelFormatName(mode.format));
            if (Rmask || Gmask || Bmask) {
                fprintf(stderr, "      Red Mask   = 0x%.8x\n", Rmask);
                fprintf(stderr, "      Green Mask = 0x%.8x\n", Gmask);
                fprintf(stderr, "      Blue Mask  = 0x%.8x\n", Bmask);
                if (Amask)
                    fprintf(stderr, "      Alpha Mask = 0x%.8x\n", Amask);
            }

            /* Print available fullscreen video modes */
            m = SDL_GetNumDisplayModes(i);
            if (m == 0) {
                fprintf(stderr, "No available fullscreen video modes\n");
            } else {
                fprintf(stderr, "  Fullscreen video modes:\n");
                for (j = 0; j < m; ++j) {
                    SDL_GetDisplayMode(i, j, &mode);
                    SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask,
                            &Gmask, &Bmask, &Amask);
                    fprintf(stderr,
                            "    Mode %d: %dx%d@%dHz, %d bits-per-pixel (%s)\n",
                            j, mode.w, mode.h, mode.refresh_rate, bpp,
                            SDL_GetPixelFormatName(mode.format));
                    if (Rmask || Gmask || Bmask) {
                        fprintf(stderr, "        Red Mask   = 0x%.8x\n",
                                Rmask);
                        fprintf(stderr, "        Green Mask = 0x%.8x\n",
                                Gmask);
                        fprintf(stderr, "        Blue Mask  = 0x%.8x\n",
                                Bmask);
                        if (Amask)
                            fprintf(stderr,
                                    "        Alpha Mask = 0x%.8x\n",
                                    Amask);
                    }
                }
            }
        }
    }

    if (VERBOSE_RENDER) {
        SDL_RendererInfo info;

        n = SDL_GetNumRenderDrivers();
        if (n == 0) {
            fprintf(stderr, "No built-in render drivers\n");
        } else {
            fprintf(stderr, "Built-in render drivers:\n");
            for (i = 0; i < n; ++i) {
                SDL_GetRenderDriverInfo(i, &info);
                PrintRenderer(&info);
            }
        }
    }

    SDL_zero(fullscreen_mode);
    switch (/*state->depth*/0) {
        case 8:
            fullscreen_mode.format = SDL_PIXELFORMAT_INDEX8;
            break;
        case 15:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB555;
            break;
        case 16:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB565;
            break;
        case 24:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB24;
            break;
        default:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB888;
            break;
    }
    //fullscreen_mode.refresh_rate = state->refresh_rate;

    {
        // FREE-RESIZE-1 (UI-ASPECT-ANCHOR meta-fix): in windowed mode the
        // window is user-resizable — every consumer (postprocess FBOs, UI
        // canvas box, mouse normalize, camera aspect) re-derives from
        // Environment.drawableWidth/Height each frame, so a live resize just
        // works once the SIZE_CHANGED handler refreshes those values.
        // MC2_WINDOW_RESIZABLE=0 restores a fixed window.
        Uint32 winFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
        // macos-port: under the cocoa driver the window presents via Metal, not
        // an Apple-GL view (that view's updateLayer touches NSOpenGLContext and
        // SIGBUSes once Mesa is loaded). A Metal window + fixed backbuffer size;
        // HiDPI/resize deferred so the pbuffer + readback stay one constant size.
        const bool macWindowed = (SDL_GetCurrentVideoDriver() &&
                                  strcmp(SDL_GetCurrentVideoDriver(), "cocoa") == 0);
        if (macWindowed) {
            g_mac.active_ = true;
            winFlags = SDL_WINDOW_METAL;
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
        }
#endif
        {
            const char* wenv = getenv("MC2_WINDOWED");
            const char* renv = getenv("MC2_WINDOW_RESIZABLE");
            const bool windowed = (wenv && wenv[0] && wenv[0] != '0');
            const bool resizable = !(renv && renv[0] == '0');
            if (windowed && resizable)
                winFlags |= SDL_WINDOW_RESIZABLE;
        }
#if defined(__APPLE__)
        if (g_mac.active_) winFlags &= ~(Uint32)SDL_WINDOW_RESIZABLE;  // fixed size for now
#endif
        window = SDL_CreateWindow(pwinname ? pwinname : "--",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, winFlags);

        if (!window) {
            fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
            return NULL;
        }
        SDL_GetWindowSize(window, &width, &height);

        // NULL to use window width and height and display refresh rate
        // only need to set mode if wanted fullscreen
        if (SDL_SetWindowDisplayMode(window, NULL) < 0) {
            fprintf(stderr, "Can't set up display mode: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            return NULL;
        }

        SDL_ShowWindow(window);

        // Hide the OS cursor. MC2 renders its own in-game cursor sprite, so
        // the default arrow would otherwise double up on top of it.
        // MC2_OS_CURSOR=1: keep the native OS cursor visible instead. Use when the
        // software cursor doesn't render (e.g. old-era MC2X/MCO campaigns at the
        // 800-logical GUI tier, where the cursorsa sprite draws invisibly). Paired
        // with a software-cursor-draw skip in UserInput::render so there's no double.
        if (getenv("MC2_OS_CURSOR"))
            SDL_ShowCursor(SDL_ENABLE);
        else
            SDL_ShowCursor(SDL_DISABLE);

#if defined(__APPLE__)
        // macos-port: Metal-backed present surface for the cocoa window. The
        // engine's finished frame (FBO 0) is read back and uploaded into a
        // streaming texture each swap_window. The pbuffer/texture are sized to
        // the live drawable and re-created on change (see mac_resize) so a
        // resolution switch or fullscreen toggle stays 1:1 with the window.
        if (g_mac.active_) {
            Uint32 rflags = SDL_RENDERER_ACCELERATED;
            if (getenv("MC2_VSYNC") && getenv("MC2_VSYNC")[0] == '1')
                rflags |= SDL_RENDERER_PRESENTVSYNC;
            g_mac.renderer_ = SDL_CreateRenderer(window, -1, rflags);
            if (!g_mac.renderer_) {
                fprintf(stderr, "[macos-port] SDL_CreateRenderer(metal) failed: %s\n", SDL_GetError());
                SDL_DestroyWindow(window);
                return NULL;
            }
            SDL_RendererInfo ri;
            SDL_GetRendererInfo(g_mac.renderer_, &ri);
            printf("[macos-port] window present via SDL_Renderer '%s'\n", ri.name ? ri.name : "?");
            // texture/pbuffer created in init_render_context->mac_resize once the
            // EGL context exists.
        }
#endif
    }

    RenderWindow* rw = new RenderWindow();
    rw->window_ = window;
    rw->width_ = width;
    rw->height_ = height;

    g_sdl_window = window;
    set_mouse_grab(true);

    return rw;
}

//==============================================================================
void swap_window(RenderWindowHandle h)
{
    RenderWindow* rw = (RenderWindow*)h;
    assert(rw && rw->window_);
#if defined(__APPLE__)
    if (g_mac.active_) {
        // macos-port: present the engine's finished frame (FBO 0, rendered by
        // Mesa/Zink) into the cocoa window via the Metal SDL_Renderer. One
        // glReadPixels + one texture upload per frame -- a CPU round-trip, but
        // it puts a real interactive window on screen. Zero-copy (shared
        // IOSurface) is the later optimization if this bottlenecks.
        // Keep FBO 0 sized to the live drawable: a resolution/fullscreen change
        // grows the window, and the engine renders at the new size. If it grew,
        // skip this (mismatched) frame's present -- the next renders 1:1.
        int cw = 0, ch = 0;
        SDL_GL_GetDrawableSize(rw->window_, &cw, &ch);
        if (cw > 0 && ch > 0 && (cw != g_mac.w_ || ch != g_mac.h_)) {
            mac_resize(cw, ch);
            return;
        }
        const int w = g_mac.w_, h2 = g_mac.h_;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h2, GL_RGBA, GL_UNSIGNED_BYTE, g_mac.rowbuf_);
        void* dst = nullptr; int pitch = 0;
        if (SDL_LockTexture(g_mac.tex_, nullptr, &dst, &pitch) == 0) {
            // glReadPixels is bottom-up; the texture is top-down -> flip rows.
            for (int y = 0; y < h2; ++y) {
                memcpy((unsigned char*)dst + (size_t)y * pitch,
                       g_mac.rowbuf_ + (size_t)(h2 - 1 - y) * w * 4, (size_t)w * 4);
            }
            SDL_UnlockTexture(g_mac.tex_);
        }
        SDL_RenderClear(g_mac.renderer_);
        SDL_RenderCopy(g_mac.renderer_, g_mac.tex_, nullptr, nullptr);
        SDL_RenderPresent(g_mac.renderer_);
        return;
    }
#endif
    SDL_GL_SwapWindow(rw->window_);
}

#if defined(__APPLE__)
//==============================================================================
// macos-port: (re)create the pbuffer surface (= FBO 0 the engine renders into)
// and the matching present texture at size w x h, then make the context
// current on the new surface. Called at init and whenever the drawable size
// changes (resolution switch / fullscreen toggle) so the readback + present
// stay 1:1 with what the engine renders. Cheap no-op when the size is unchanged.
static bool mac_resize(int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    if (w == g_mac.w_ && h == g_mac.h_ && g_mac.surf_ != EGL_NO_SURFACE) return true;

    const EGLint pbAttr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface ns = eglCreatePbufferSurface(g_mac.dpy_, g_mac.cfg_, pbAttr);
    if (ns == EGL_NO_SURFACE) {
        fprintf(stderr, "[macos-port] eglCreatePbufferSurface(%dx%d) failed: 0x%x\n", w, h, eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_mac.dpy_, ns, ns, g_mac.ctx_)) {
        fprintf(stderr, "[macos-port] eglMakeCurrent(resize) failed: 0x%x\n", eglGetError());
        eglDestroySurface(g_mac.dpy_, ns);
        return false;
    }
    EGLSurface old = g_mac.surf_;
    g_mac.surf_ = ns;
    if (old != EGL_NO_SURFACE) eglDestroySurface(g_mac.dpy_, old);

    if (g_mac.tex_) SDL_DestroyTexture(g_mac.tex_);
    g_mac.tex_ = SDL_CreateTexture(g_mac.renderer_, SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    g_mac.rowbuf_ = (unsigned char*)realloc(g_mac.rowbuf_, (size_t)w * h * 4);
    g_mac.w_ = w; g_mac.h_ = h;
    printf("[macos-port] present surface sized to %dx%d\n", w, h);
    return true;
}

//==============================================================================
// macos-port: create the engine's real GL context on Mesa (Zink/kosmickrisp)
// via a MANUAL EGL surfaceless display + a pbuffer surface, so that FBO 0 exists
// and the engine's final composite lands there for readback. SDL is on the
// cocoa driver here and would only hand us Apple GL 4.1.
static bool mac_egl_init(int w, int h)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatDpy)
        dpy = getPlatDpy(EGL_PLATFORM_SURFACELESS_MESA, (void*)EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "[macos-port] no EGL display\n"); return false; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "[macos-port] eglInitialize failed: 0x%x\n", eglGetError()); return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[macos-port] eglBindAPI(GL) failed\n"); return false;
    }

    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = nullptr; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "[macos-port] eglChooseConfig failed (n=%d err=0x%x)\n", ncfg, eglGetError());
        return false;
    }

    EGLint ctxAttr[16]; int ai = 0;
    ctxAttr[ai++] = EGL_CONTEXT_MAJOR_VERSION; ctxAttr[ai++] = 4;
    ctxAttr[ai++] = EGL_CONTEXT_MINOR_VERSION; ctxAttr[ai++] = 3;
    ctxAttr[ai++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
    ctxAttr[ai++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
    if (getenv("MC2_GL_DEBUG")) { ctxAttr[ai++] = EGL_CONTEXT_OPENGL_DEBUG; ctxAttr[ai++] = EGL_TRUE; }
    ctxAttr[ai++] = EGL_NONE;
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[macos-port] eglCreateContext(4.3 core) failed: 0x%x\n", eglGetError());
        return false;
    }
    g_mac.dpy_ = dpy; g_mac.ctx_ = ctx; g_mac.cfg_ = cfg;
    if (!mac_resize(w, h)) return false;  // creates surface + makes current + texture
    printf("[macos-port] EGL %d.%d GL context current (pbuffer %dx%d, vendor=%s)\n",
           major, minor, w, h, eglQueryString(dpy, EGL_VENDOR));
    return true;
}
#endif

//==============================================================================
RenderContextHandle init_render_context(RenderWindowHandle render_window)
{
    RenderWindow* rw = (RenderWindow*)render_window;
    assert(rw && rw->window_);

    SDL_GLContext glcontext = NULL;
#if defined(__APPLE__)
    if (g_mac.active_) {
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(rw->window_, &dw, &dh);
        if (dw <= 0 || dh <= 0) { dw = rw->width_; dh = rw->height_; }
        if (!mac_egl_init(dw, dh)) return NULL;
        glcontext = (SDL_GLContext)g_mac.ctx_;  // non-null sentinel for downstream
    } else
#endif
    {
    glcontext = SDL_GL_CreateContext(rw->window_);
    if (!glcontext ) {
        fprintf(stderr, "SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return NULL;
    }

    if (SDL_GL_MakeCurrent(rw->window_, glcontext) < 0) {
        SDL_GL_DeleteContext(glcontext);
        return NULL;
    }
    }

    // MC2_VSYNC: "1" forces vsync on, "0" or unset leaves it off.
    // Off by default so a GPU that misses 60 Hz is not rounded down
    // to 30/20/15 FPS.
    const char* vsync_env = getenv("MC2_VSYNC");
    const bool vsync_on = (vsync_env && vsync_env[0] == '1');
#if defined(__APPLE__)
    // macos-port: present-side vsync is owned by the SDL_Renderer flags (set in
    // create_window); there's no SDL GL swap interval on the manual EGL path.
    if (!g_mac.active_)
#endif
    SDL_GL_SetSwapInterval(vsync_on ? 1 : 0);
    printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
           vsync_env ? vsync_env : "(unset, default 0)",
           vsync_on ? "ON" : "OFF");

    // Print GPU identity unconditionally with a distinctive prefix so it is
    // impossible to miss when a user pastes their console log for triage.
    // On hybrid-graphics laptops this line is the single most valuable
    // diagnostic: it says which GPU OpenGL actually selected.
    printf("[GPU] Vendor   : %s\n", glGetString(GL_VENDOR));
    printf("[GPU] Renderer : %s\n", glGetString(GL_RENDERER));
    printf("[GPU] Version  : %s\n", glGetString(GL_VERSION));

    // GPU-VENDOR-DETECT-1: cache whether this is an NVIDIA GPU. The whole
    // GPU-driven static-prop stack (compute cull -> indirect multidraw ->
    // persistent-mapped readback) was only ever validated on AMD RDNA3; on
    // NVIDIA registered static props (trees/buildings) can draw nothing. Used
    // by mclib to default MC2_FORCE_DYNAMIC_{TREES,BUILDINGS} ON for NVIDIA as a
    // stopgap until the GPU path is fixed on that hardware (see bdactor.cpp).
    {
        const char* vend = (const char*)glGetString(GL_VENDOR);
        g_gosGpuIsNvidia = 0;
        if (vend) {
            // Vendor string is e.g. "NVIDIA Corporation".
            if (strstr(vend, "NVIDIA") || strstr(vend, "nvidia") ||
                strstr(vend, "NVidia"))
                g_gosGpuIsNvidia = 1;
        }
        printf("[GPU] IsNvidia : %d\n", g_gosGpuIsNvidia);
    }

    // GL capability limits -- useful to rule out SSBO / texture-size / unit
    // ceilings when a user reports rendering issues on unusual hardware.
    {
        GLint maxTex = 0, maxSSBO = 0, maxTexUnits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSSBO);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTexUnits);
        printf("[GL] max_texture_size=%d max_ssbo_block=%d max_combined_texture_units=%d\n",
               maxTex, maxSSBO, maxTexUnits);
    }

#ifdef MC2_VULKAN
    // VULKAN-BACKEND-SKELETON-1: one-shot, opt-in Vulkan capability probe.
    // Runs ONLY if the MC2_VULKAN_PROBE env var is set (default unset -> never
    // runs). Fail-soft: logs a reason + continues on any Vulkan error. This is
    // NOT wired into the frame loop and does NOT touch the GL render path.
    mc2_vulkan_probe_if_env();
#endif

    // Drawable (physical, post-HiDPI) vs logical (window coords) size.
    // Divergence indicates the backbuffer is larger than the window, which
    // multiplies fragment cost.
    {
        int draw_w = 0, draw_h = 0, logical_w = 0, logical_h = 0;
        SDL_GL_GetDrawableSize(rw->window_, &draw_w, &draw_h);
        SDL_GetWindowSize(rw->window_, &logical_w, &logical_h);
        printf("[WINDOW] drawable=%dx%d logical=%dx%d%s\n",
               draw_w, draw_h, logical_w, logical_h,
               (draw_w != logical_w || draw_h != logical_h) ? " (HiDPI)" : "");
    }

    // Single-line summary of effective runtime mode. Anchor for log pastes:
    // grep [MODE] and you see every toggle state at a glance.
    // NB: gl_debug is set on the attribute in create_window()'s scope; we
    // re-read the env here because that local doesn't carry across functions.
    const bool gl_debug_mode = (getenv("MC2_GL_DEBUG") != nullptr);
    printf("[MODE] gl_debug=%d vsync=%d tracy=on-demand\n",
           gl_debug_mode ? 1 : 0,
           vsync_on ? 1 : 0);
    printf("[TRACY] on-demand mode -- profiler listening on TCP 8086, no capture until a GUI attaches.\n");

    if(VERBOSE_RENDER) {
        SDL_DisplayMode mode;
        SDL_GetCurrentDisplayMode(0, &mode);
        printf("Current Display Mode:\n");
        printf("Screen BPP: %d\n", SDL_BITSPERPIXEL(mode.format));
        printf("\n");
        printf("Vendor     : %s\n", glGetString(GL_VENDOR));
        printf("Renderer   : %s\n", glGetString(GL_RENDERER));
        printf("Version    : %s\n", glGetString(GL_VERSION));
        const GLubyte* exts = glGetString(GL_EXTENSIONS);
        printf("Extensions : %s\n", exts);
        printf("\n");

        int value;
        int status = 0;

        /*
           status = SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &value);
           if (!status) {
           printf("SDL_GL_RED_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_RED_SIZE: %s\n", SDL_GetError());
           }
           status = SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &value);
           if (!status) {
           printf("SDL_GL_GREEN_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_GREEN_SIZE: %s\n", SDL_GetError());
           }
           status = SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &value);
           if (!status) {
           printf("SDL_GL_BLUE_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_BLUE_SIZE: %s\n", SDL_GetError());
           }
           */
        //status = SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &value);
        //if (!status) {
        //    printf("SDL_GL_DEPTH_SIZE: requested %d, got %d\n", 16, value);
        //} else {
        //    printf("Failed to get SDL_GL_DEPTH_SIZE: %s\n", SDL_GetError());
        //}

		/*
        status = SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &value);
        if (!status) {
            printf("SDL_GL_MULTISAMPLEBUFFERS: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_MULTISAMPLEBUFFERS: %s\n",
                    SDL_GetError());
        }

        status = SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &value);
        if (!status) {
            printf("SDL_GL_MULTISAMPLESAMPLES: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_MULTISAMPLESAMPLES: %s\n",
                    SDL_GetError());
        }
		*/
        status = SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &value);
        if (!status) {
            printf("SDL_GL_ACCELERATED_VISUAL: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_ACCELERATED_VISUAL: %s\n",
                    SDL_GetError());
        }
		
        status = SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &value);
        if (!status) {
            printf("SDL_GL_CONTEXT_MAJOR_VERSION: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_CONTEXT_MAJOR_VERSION: %s\n", SDL_GetError());
        }

        status = SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &value);
        if (!status) {
            printf("SDL_GL_CONTEXT_MINOR_VERSION: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_CONTEXT_MINOR_VERSION: %s\n", SDL_GetError());
        }
    }

    RenderContext* rc = new RenderContext();
    rc->glcontext_ = glcontext;
    rc->render_window_ = render_window;

    g_sdl_glcontext = glcontext;

	return rc;
}

//==============================================================================
void destroy_render_context(RenderContextHandle rc_handle)
{
    RenderContext* rc = (RenderContext*)rc_handle;
    assert(rc);

#if defined(__APPLE__)
    if (g_mac.active_) {
        if (g_mac.dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(g_mac.dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (g_mac.ctx_  != EGL_NO_CONTEXT) eglDestroyContext(g_mac.dpy_, g_mac.ctx_);
            if (g_mac.surf_ != EGL_NO_SURFACE) eglDestroySurface(g_mac.dpy_, g_mac.surf_);
            eglTerminate(g_mac.dpy_);
        }
        if (g_mac.tex_)      SDL_DestroyTexture(g_mac.tex_);
        if (g_mac.renderer_) SDL_DestroyRenderer(g_mac.renderer_);
        free(g_mac.rowbuf_);
        g_mac = MacPresent();
        g_sdl_glcontext = NULL;
        delete rc;
        return;
    }
#endif
    SDL_GL_DeleteContext(rc->glcontext_);
    g_sdl_glcontext = NULL;
    delete rc;
}

//==============================================================================
void make_current_context(RenderContextHandle ctx_h)
{
    RenderContext* rc = (RenderContext*)ctx_h;
    assert(rc && rc->render_window_ && rc->glcontext_);

    RenderWindow* rw = rc->render_window_;
    assert(rw && rw->window_);

#if defined(__APPLE__)
    if (g_mac.active_) {
        eglMakeCurrent(g_mac.dpy_, g_mac.surf_, g_mac.surf_, g_mac.ctx_);
        return;
    }
#endif
    SDL_GL_MakeCurrent(rw->window_, rc->glcontext_);
}

//==============================================================================
bool resize_window(RenderWindowHandle rw_handle, int width, int height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);

    SDL_SetWindowSize(rw->window_, width, height);
    rw->width_ = width;
    rw->height_ = height;
    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
    }

    return true;
}

//==============================================================================
bool set_window_fullscreen(RenderWindowHandle rw_handle, bool fullscreen)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);

    Uint32 flags = fullscreen ? /*SDL_WINDOW_FULLSCREEN*/ SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    
    if(0 != SDL_SetWindowFullscreen(rw->window_, flags)) {
        log_error("SDL_SetWindowFullscreen: %s\n", SDL_GetError());
        return false;
    }

    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
    }

    return true;
}

//==============================================================================
bool is_mode_supported(int width, int height, int bpp) {

    int displayIndex = 0;
    //displayIndex = SDL_GetWindowDisplayIndex(win_h);

    SDL_DisplayMode desired;
	desired.format = bpp==16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888;
	desired.w = width;
	desired.h = height;
	desired.refresh_rate = 0;
	desired.driverdata = 0;

    SDL_DisplayMode returned;
    
    if(NULL == SDL_GetClosestDisplayMode(displayIndex, &desired, &returned)) {
        log_error("resize_window: %s\n", SDL_GetError());
        return false;
    }

    //const char* df = SDL_GetPixelFormatName(desired.format);
    //const char* rf = SDL_GetPixelFormatName(returned.format);

    if(returned.w == desired.w && returned.h == desired.h && returned.format == desired.format)
        return true;

    return false;
}

//==============================================================================
int get_window_display_index(RenderContextHandle ctx_h)
{
    RenderContext* rc = (RenderContext*)ctx_h;
    assert(rc);

    RenderWindow* rw = rc->render_window_;
    assert(rw && rw->window_);

    return SDL_GetWindowDisplayIndex(rw->window_);
}

//==============================================================================
int get_num_display_modes(int display_index)
{
    return SDL_GetNumDisplayModes(display_index);
}

//==============================================================================
bool get_desktop_display_mode(int display_index, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(display_index, &dm) != 0) {
        log_error("SDL_GetDesktopDisplayMode failed: %s", SDL_GetError());
        return false;
    }

    *width = dm.w;
    *height = dm.h;
    *bpp = SDL_BITSPERPIXEL(dm.format);
    return true;
}

//==============================================================================
bool get_display_mode_by_index(int display_index, int mode_index, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDisplayMode(display_index, mode_index, &dm) != 0) {
        log_error("SDL_GetDisplayMode failed: %s", SDL_GetError());
        return false;
    }

    *width = dm.w;
    *height = dm.h;
    *bpp = SDL_BITSPERPIXEL(dm.format);
    return true;
}

//==============================================================================
void get_window_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
    *width = rw->width_;
    *height = rw->height_;
}

//==============================================================================
void get_drawable_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
	// TOD: does it make sense to cahce this value? probably not
	SDL_GL_GetDrawableSize(rw->window_, width, height);
}

//==============================================================================
void destroy_window(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    if (rw->window_ == g_sdl_window) {
        set_mouse_grab(false);
    }
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(rw->window_);
    delete rw;

    g_sdl_window = NULL;
}

//==============================================================================
static void PrintRendererFlag(Uint32 flag)
{
	switch (flag) {
	case SDL_RENDERER_PRESENTVSYNC:
		fprintf(stderr, "PresentVSync");
		break;
	case SDL_RENDERER_ACCELERATED:
		fprintf(stderr, "Accelerated");
		break;
	default:
		fprintf(stderr, "0x%8.8x", flag);
		break;
	}
}

//==============================================================================
static void PrintPixelFormat(Uint32 format)
{
	switch (format) {
	case SDL_PIXELFORMAT_UNKNOWN:
		fprintf(stderr, "Unknwon");
		break;
	case SDL_PIXELFORMAT_INDEX1LSB:
		fprintf(stderr, "Index1LSB");
		break;
	case SDL_PIXELFORMAT_INDEX1MSB:
		fprintf(stderr, "Index1MSB");
		break;
	case SDL_PIXELFORMAT_INDEX4LSB:
		fprintf(stderr, "Index4LSB");
		break;
	case SDL_PIXELFORMAT_INDEX4MSB:
		fprintf(stderr, "Index4MSB");
		break;
	case SDL_PIXELFORMAT_INDEX8:
		fprintf(stderr, "Index8");
		break;
	case SDL_PIXELFORMAT_RGB332:
		fprintf(stderr, "RGB332");
		break;
	case SDL_PIXELFORMAT_RGB444:
		fprintf(stderr, "RGB444");
		break;
	case SDL_PIXELFORMAT_RGB555:
		fprintf(stderr, "RGB555");
		break;
	case SDL_PIXELFORMAT_BGR555:
		fprintf(stderr, "BGR555");
		break;
	case SDL_PIXELFORMAT_ARGB4444:
		fprintf(stderr, "ARGB4444");
		break;
	case SDL_PIXELFORMAT_ABGR4444:
		fprintf(stderr, "ABGR4444");
		break;
	case SDL_PIXELFORMAT_ARGB1555:
		fprintf(stderr, "ARGB1555");
		break;
	case SDL_PIXELFORMAT_ABGR1555:
		fprintf(stderr, "ABGR1555");
		break;
	case SDL_PIXELFORMAT_RGB565:
		fprintf(stderr, "RGB565");
		break;
	case SDL_PIXELFORMAT_BGR565:
		fprintf(stderr, "BGR565");
		break;
	case SDL_PIXELFORMAT_RGB24:
		fprintf(stderr, "RGB24");
		break;
	case SDL_PIXELFORMAT_BGR24:
		fprintf(stderr, "BGR24");
		break;
	case SDL_PIXELFORMAT_RGB888:
		fprintf(stderr, "RGB888");
		break;
	case SDL_PIXELFORMAT_BGR888:
		fprintf(stderr, "BGR888");
		break;
	case SDL_PIXELFORMAT_ARGB8888:
		fprintf(stderr, "ARGB8888");
		break;
	case SDL_PIXELFORMAT_RGBA8888:
		fprintf(stderr, "RGBA8888");
		break;
	case SDL_PIXELFORMAT_ABGR8888:
		fprintf(stderr, "ABGR8888");
		break;
	case SDL_PIXELFORMAT_BGRA8888:
		fprintf(stderr, "BGRA8888");
		break;
	case SDL_PIXELFORMAT_ARGB2101010:
		fprintf(stderr, "ARGB2101010");
		break;
	case SDL_PIXELFORMAT_YV12:
		fprintf(stderr, "YV12");
		break;
	case SDL_PIXELFORMAT_IYUV:
		fprintf(stderr, "IYUV");
		break;
	case SDL_PIXELFORMAT_YUY2:
		fprintf(stderr, "YUY2");
		break;
	case SDL_PIXELFORMAT_UYVY:
		fprintf(stderr, "UYVY");
		break;
	case SDL_PIXELFORMAT_YVYU:
		fprintf(stderr, "YVYU");
		break;
	default:
		fprintf(stderr, "0x%8.8x", format);
		break;
	}
}

//==============================================================================
static void PrintRenderer(SDL_RendererInfo * info)
{
    size_t i, count;

    fprintf(stderr, "  Renderer %s:\n", info->name);

    fprintf(stderr, "    Flags: 0x%8.8X", info->flags);
    fprintf(stderr, " (");
    count = 0;
    for (i = 0; i < sizeof(info->flags) * 8; ++i) {
        Uint32 flag = (1 << i);
        if (info->flags & flag) {
            if (count > 0) {
                fprintf(stderr, " | ");
            }
            PrintRendererFlag(flag);
            ++count;
        }
    }
    fprintf(stderr, ")\n");

    fprintf(stderr, "    Texture formats (%d): ", info->num_texture_formats);
    for (i = 0; i < info->num_texture_formats; ++i) {
        if (i > 0) {
			fprintf(stderr, ", ");
		}
		PrintPixelFormat(info->texture_formats[i]);
	}
	fprintf(stderr, "\n");

	if (info->max_texture_width || info->max_texture_height) {
		fprintf(stderr, "    Max Texture Size: %dx%d\n",
				info->max_texture_width, info->max_texture_height);
	}
}



//==============================================================================
SDL_Window* getSDLWindow() noexcept
{
    return g_sdl_window;
}

//==============================================================================
SDL_GLContext getSDLGLContext() noexcept
{
    return g_sdl_glcontext;
}

}; // namespace graphics
