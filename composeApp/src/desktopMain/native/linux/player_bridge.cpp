// Linux JNI player bridge for Nuvio Desktop.
//
// Mirrors the Windows (player_bridge.cpp) and macOS (player_bridge.mm) bridges,
// implementing the JNI contract declared in
//   composeApp/src/desktopMain/kotlin/.../desktop/NativePlayerBridge.kt
//
// This is the Phase 3 "video-first" implementation:
//   * libmpv embedded into the host AWT Canvas X11 window via mpv's "wid" option
//   * full transport + state queries (the app polls these via snapshot())
//   * subtitle delay/style + external subtitle add
//
// Deferred (stubbed here, see Docs/LINUX_PORT_PLAN.md):
//   * Phase 4 — WebKitGTK controls overlay (updateControls / warmupWebView2 /
//     the event-sink callbacks for scrub/fullscreen/control actions)
//   * Phase 4 — audio/subtitle track enumeration JSON (returns "[]" for now)
//   * Phase 5 — applyWindowChrome / setWindowBorderlessFullscreen (X11/WM hints)
//
// Built against system libmpv via pkg-config (see composeApp/build.gradle.kts,
// buildLinuxPlayerBridge). Direct-linked (-lmpv); for the AppImage the .so is
// located next to a bundled libmpv via an $ORIGIN rpath.

#include <jni.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <epoxy/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <atomic>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// One live player. The jlong handle returned by create() is a pointer to this.
struct PlayerInstance {
    mpv_handle *mpv = nullptr;
    JavaVM *jvm = nullptr;
    jobject eventSink = nullptr;      // global ref; receives overlay control events
    jmethodID onEventMethod = nullptr; // NativePlayerEventSink.onPlayerEvent(String,double)
    std::thread eventThread;
    std::atomic<bool> running{false};

    // WebKitGTK controls overlay (created/destroyed on the GTK thread).
    unsigned long hostWid = 0;   // host AWT Canvas X11 window we overlay into
    std::string controlsUrl;     // file:// URL of the HTML controls page
    GtkWidget *gtkWindow = nullptr;
    WebKitWebView *webView = nullptr;

    // Render-API path (NUVIO_LINUX_RENDER): mpv renders into a GtkGLArea.
    bool renderMode = false;
    // Software-render path (Compose): mpv renders frames into a CPU buffer that
    // Kotlin draws in a Compose Canvas via Skia. No window, no overlay.
    bool swMode = false;
    mpv_render_context *renderCtx = nullptr;
    GtkWidget *glArea = nullptr;

    // Controls protocol state (webview).
    std::string pendingControlsJson; // latest structural state (window.playerControls)
    bool controlsReady = false;      // webview sent "controlsReady"
    guint syncTimerId = 0;           // periodic playback-state push (window.playerUpdate)
};

// Guards PlayerInstance *lifetime*. The app polls state (positionMs, isPaused,
// ...) from a thread that can differ from the one calling dispose(), so without
// this a query can dereference an instance dispose() just freed. libmpv itself
// is internally thread-safe; this lock only protects create/destroy/registry.
// (Mirrors the locking the Windows/macOS bridges use.)
std::mutex g_mutex;
std::set<PlayerInstance *> g_live;

// ---- small helpers --------------------------------------------------------

std::string jstringToUtf8(JNIEnv *env, jstring value) {
    if (value == nullptr) {
        return std::string();
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return std::string();
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

// Caller MUST hold g_mutex. Returns the instance only if it is still registered
// (not yet disposed) and has a live mpv handle; otherwise nullptr. The membership
// check short-circuits before any dereference, so a stale handle is never
// dereferenced.
PlayerInstance *liveLocked(jlong handle) {
    auto *player = reinterpret_cast<PlayerInstance *>(handle);
    if (player != nullptr && g_live.count(player) != 0 && player->mpv != nullptr) {
        return player;
    }
    return nullptr;
}

double getDouble(mpv_handle *mpv, const char *name, double fallback = 0.0) {
    double out = fallback;
    if (mpv == nullptr || mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &out) < 0) {
        return fallback;
    }
    return out;
}

bool getFlag(mpv_handle *mpv, const char *name, bool fallback = false) {
    int out = fallback ? 1 : 0;
    if (mpv == nullptr || mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &out) < 0) {
        return fallback;
    }
    return out != 0;
}

void setFlag(mpv_handle *mpv, const char *name, bool value) {
    if (mpv == nullptr) {
        return;
    }
    int flag = value ? 1 : 0;
    mpv_set_property(mpv, name, MPV_FORMAT_FLAG, &flag);
}

void setDouble(mpv_handle *mpv, const char *name, double value) {
    if (mpv == nullptr) {
        return;
    }
    mpv_set_property(mpv, name, MPV_FORMAT_DOUBLE, &value);
}

void command(mpv_handle *mpv, std::vector<const char *> args) {
    if (mpv == nullptr) {
        return;
    }
    const char *name = args.empty() ? "?" : args[0];
    args.push_back(nullptr);
    int rc = mpv_command(mpv, args.data());
    if (rc < 0) {
        std::fprintf(stderr, "[nuvio-player] mpv_command '%s' failed: %s\n", name, mpv_error_string(rc));
    }
}

// ---- WebKitGTK controls overlay ------------------------------------------
// The HTML controls (composeApp/.../resources/player-ui/controls.*) already speak
// the WebKit message API (window.webkit.messageHandlers.player + window.playerUpdate),
// shared with the macOS WKWebView path, so the JS is reused unchanged. Here we host
// a transparent WebKitWebView reparented over the mpv video window, push state via
// evaluate_javascript, and forward web messages to the Kotlin event sink.
//
// All GTK/WebKit calls must run on the GTK thread; use runOnGtk() to marshal.

std::once_flag g_gtkOnce;

void ensureGtkThread() {
    std::call_once(g_gtkOnce, []() {
        std::thread([]() {
            gtk_init(nullptr, nullptr);
            // Make X errors non-fatal. Our overlay window is a child of the AWT
            // Canvas, so when the player view is disposed the parent can be
            // destroyed first, making teardown unmap/destroy hit BadWindow — and
            // the default Xlib handler aborts the whole process. Log and continue.
            XSetErrorHandler([](Display *d, XErrorEvent *e) -> int {
                char buf[256];
                XGetErrorText(d, e->error_code, buf, sizeof(buf));
                std::fprintf(stderr, "[nuvio-player] X error ignored: %s (code %d, request %d)\n",
                             buf, e->error_code, e->request_code);
                return 0;
            });
            gtk_main();
        }).detach();
    });
}

struct GtkTask {
    std::function<void()> fn;
};

gboolean gtkTaskTrampoline(gpointer data) {
    auto *task = static_cast<GtkTask *>(data);
    task->fn();
    delete task;
    return G_SOURCE_REMOVE;
}

void runOnGtk(std::function<void()> fn) {
    ensureGtkThread();
    // g_idle_add (not g_main_context_invoke): the latter runs the callback inline
    // on the calling thread if it can acquire the default context, which races the
    // GTK thread's gtk_init and would run GTK code on the AWT thread (crash). An
    // idle source always runs on the thread iterating the context (our gtk_main).
    g_idle_add_full(G_PRIORITY_DEFAULT, gtkTaskTrampoline, new GtkTask{std::move(fn)}, nullptr);
}

// Calls NativePlayerEventSink.onPlayerEvent(type, value). Runs on the GTK thread,
// which is attached to the JVM on first use and left attached (daemon thread).
void dispatchEvent(PlayerInstance *player, const char *type, double value) {
    if (player->eventSink == nullptr || player->onEventMethod == nullptr || player->jvm == nullptr) {
        return;
    }
    JNIEnv *env = nullptr;
    jint rc = player->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (player->jvm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK) {
            return;
        }
    } else if (rc != JNI_OK) {
        return;
    }
    jstring jtype = env->NewStringUTF(type);
    env->CallVoidMethod(player->eventSink, player->onEventMethod, jtype, value);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jtype);
}

// Forward declaration (defined in the track-list section below).
std::string buildTracksJson(mpv_handle *mpv, const char *wantType);

// Pushes live playback state to the controls via window.playerUpdate(). This is
// what moves the controls off their loading skeleton and drives the scrubber.
// Runs on the GTK thread.
void syncControlsOnGtk(PlayerInstance *player) {
    if (player->webView == nullptr || player->mpv == nullptr) {
        return;
    }
    const double duration = getDouble(player->mpv, "duration", 0.0);
    const double position = getDouble(player->mpv, "time-pos", 0.0);
    const bool paused = getFlag(player->mpv, "pause", false);
    const bool loading = getFlag(player->mpv, "paused-for-cache", false) ||
                         getFlag(player->mpv, "seeking", false);
    char head[256];
    std::snprintf(head, sizeof(head),
                  "window.playerUpdate({duration:%.3f,position:%.3f,paused:%s,loading:%s,audioTracks:",
                  duration, position, paused ? "true" : "false", loading ? "true" : "false");
    std::string script = head;
    script += buildTracksJson(player->mpv, "audio");
    script += ",subtitleTracks:";
    script += buildTracksJson(player->mpv, "sub");
    script += "});";
    webkit_web_view_evaluate_javascript(player->webView, script.c_str(), -1,
                                        nullptr, nullptr, nullptr, nullptr, nullptr);
}

// Applies the cached structural controls state via window.playerControls().
void flushControlsOnGtk(PlayerInstance *player) {
    if (player->webView == nullptr || !player->controlsReady || player->pendingControlsJson.empty()) {
        return;
    }
    const std::string script = "window.playerControls(" + player->pendingControlsJson + ");";
    webkit_web_view_evaluate_javascript(player->webView, script.c_str(), -1,
                                        nullptr, nullptr, nullptr, nullptr, nullptr);
}

// Periodic timer (GTK thread): pushes live playback state until the player ends.
gboolean onSyncTimer(gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_live.count(player) == 0) {
        return G_SOURCE_REMOVE;
    }
    syncControlsOnGtk(player);
    return G_SOURCE_CONTINUE;
}

// window.webkit.messageHandlers.player.postMessage({type, value})
void onScriptMessage(WebKitUserContentManager *, WebKitJavascriptResult *result, gpointer userData) {
    auto *player = static_cast<PlayerInstance *>(userData);
    JSCValue *message = webkit_javascript_result_get_js_value(result);
    if (message == nullptr || !jsc_value_is_object(message)) {
        return;
    }
    JSCValue *typeVal = jsc_value_object_get_property(message, "type");
    JSCValue *valueVal = jsc_value_object_get_property(message, "value");
    char *typeStr = (typeVal != nullptr && jsc_value_is_string(typeVal)) ? jsc_value_to_string(typeVal) : nullptr;
    double value = (valueVal != nullptr && jsc_value_is_number(valueVal)) ? jsc_value_to_double(valueVal) : 0.0;
    if (typeStr != nullptr) {
        if (std::strcmp(typeStr, "controlsReady") == 0) {
            // The page is ready: flush the cached structural state and push an
            // immediate playback snapshot so the controls leave their skeleton.
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_live.count(player) != 0) {
                player->controlsReady = true;
                flushControlsOnGtk(player);
                syncControlsOnGtk(player);
            }
        } else {
            dispatchEvent(player, typeStr, value);
        }
        g_free(typeStr);
    }
    if (typeVal != nullptr) g_object_unref(typeVal);
    if (valueVal != nullptr) g_object_unref(valueVal);
}

// Runs on the GTK thread (caller holds g_mutex and has verified the player is live).
void createOverlayOnGtk(PlayerInstance *player) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_widget_set_app_paintable(window, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(window);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
    if (rgba != nullptr) {
        gtk_widget_set_visual(window, rgba);
    }

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "player");
    g_signal_connect(ucm, "script-message-received::player", G_CALLBACK(onScriptMessage), player);

    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(web, &transparent);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(web));

    player->gtkWindow = window;
    player->webView = web;

    gtk_widget_realize(window);
    gtk_widget_show_all(window);

    GdkWindow *gdkWindow = gtk_widget_get_window(window);
    Window overlayXid = GDK_WINDOW_XID(gdkWindow);
    Display *display = GDK_WINDOW_XDISPLAY(gdkWindow);

    XReparentWindow(display, overlayXid, player->hostWid, 0, 0);
    XWindowAttributes attrs;
    if (XGetWindowAttributes(display, player->hostWid, &attrs) != 0) {
        gtk_window_resize(GTK_WINDOW(window), attrs.width, attrs.height);
        XResizeWindow(display, overlayXid, attrs.width, attrs.height);
    }
    XMapWindow(display, overlayXid);
    XRaiseWindow(display, overlayXid);
    XFlush(display);

    webkit_web_view_load_uri(web, player->controlsUrl.c_str());
    std::fprintf(stderr, "[nuvio-player] overlay created over host wid=%lu (rgba_visual=%d)\n",
                 player->hostWid, rgba != nullptr ? 1 : 0);
}

// ---- mpv render-API into a GtkGLArea -------------------------------------
// Alternative to wid embedding: mpv (vo=libmpv) renders into a GtkGLArea via the
// OpenGL render API. The GLArea lives in the same GTK window as the (transparent)
// WebKitWebView under a GtkOverlay, so GTK composites video + controls in-process
// — avoiding the X11 sibling-window conflict of the child-reparent overlay.

void *renderGetProcAddress(void *, const char *name) {
    return reinterpret_cast<void *>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>(name)));
}

// Called by mpv (possibly off the GTK thread) when a new frame is ready.
void onMpvRedraw(void *ctx) {
    static std::atomic<int> redrawCount{0};
    const int c = ++redrawCount;
    if (c % 120 == 1) {
        std::fprintf(stderr, "[nuvio-player] mpv redraw callbacks: %d\n", c);
    }
    auto *player = static_cast<PlayerInstance *>(ctx);
    runOnGtk([player]() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_live.count(player) != 0 && player->glArea != nullptr) {
            gtk_gl_area_queue_render(GTK_GL_AREA(player->glArea));
        }
    });
}

void onGlAreaRealize(GtkGLArea *area, gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) {
        std::fprintf(stderr, "[nuvio-player] GtkGLArea GL init error\n");
        return;
    }
    mpv_opengl_init_params glInit;
    glInit.get_proc_address = renderGetProcAddress;
    glInit.get_proc_address_ctx = nullptr;
    // No ADVANCED_CONTROL: that mode requires manual frame/swap management
    // (mpv_render_context_update + report_swap). Without it, use mpv's simpler
    // self-timed model where the update callback drives a queue_render per frame.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&player->renderCtx, player->mpv, params) < 0) {
        std::fprintf(stderr, "[nuvio-player] mpv_render_context_create failed\n");
        player->renderCtx = nullptr;
        return;
    }
    mpv_render_context_set_update_callback(player->renderCtx, onMpvRedraw, player);
    std::fprintf(stderr, "[nuvio-player] render context created\n");
}

gboolean onGlAreaRender(GtkGLArea *area, GdkGLContext *, gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    if (player->renderCtx == nullptr) {
        return FALSE;
    }
    static std::atomic<int> renderCount{0};
    const int c = ++renderCount;
    if (c % 120 == 1) {
        std::fprintf(stderr, "[nuvio-player] gl renders: %d\n", c);
    }
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    const int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    mpv_opengl_fbo mpfbo;
    mpfbo.fbo = static_cast<int>(fbo);
    mpfbo.w = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale;
    mpfbo.h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale;
    mpfbo.internal_format = 0;
    int flipY = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(player->renderCtx, params);
    return TRUE;
}

// Per-frame-clock poll: render when mpv reports a new frame. Reliable across the
// initial frame (the edge-triggered update callback can be missed before the GL
// context exists) without re-rendering every vsync.
gboolean onGlAreaTick(GtkWidget *widget, GdkFrameClock *, gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    if (player->renderCtx != nullptr) {
        const uint64_t flags = mpv_render_context_update(player->renderCtx);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            gtk_gl_area_queue_render(GTK_GL_AREA(widget));
            // Force a window-level redraw so the composited GL result is presented
            // to screen each frame (the embedded GLArea's GL swap alone stalls).
            if (player->gtkWindow != nullptr) {
                gtk_widget_queue_draw(player->gtkWindow);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

void onGlAreaUnrealize(GtkGLArea *area, gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    gtk_gl_area_make_current(area);
    if (player->renderCtx != nullptr) {
        mpv_render_context_free(player->renderCtx);
        player->renderCtx = nullptr;
    }
}

void onWebLoadChanged(WebKitWebView *, WebKitLoadEvent event, gpointer) {
    const char *name =
        event == WEBKIT_LOAD_STARTED ? "started" :
        event == WEBKIT_LOAD_REDIRECTED ? "redirected" :
        event == WEBKIT_LOAD_COMMITTED ? "committed" :
        event == WEBKIT_LOAD_FINISHED ? "finished" : "other";
    std::fprintf(stderr, "[nuvio-player] webview load: %s\n", name);
}

gboolean onWebLoadFailed(WebKitWebView *, WebKitLoadEvent, gchar *uri, GError *error, gpointer) {
    std::fprintf(stderr, "[nuvio-player] webview load FAILED: %s (%s)\n",
                 uri, error != nullptr ? error->message : "unknown");
    return FALSE;
}

// Runs on the GTK thread (caller holds g_mutex and verified the player is live).
void createRenderWindowOnGtk(PlayerInstance *player) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    // NOTE: do NOT set an RGBA visual here. It makes the GtkGLArea's GLX drawable
    // invalid (GLXBadWindow) so rendered frames never reach the screen until a
    // pause forces a different present path. The controls webview's transparency
    // composites over the GLArea via cairo and needs no window-level alpha.

    // Realize the (empty) window to get its X11 window, then reparent it into the
    // AWT host BEFORE the GtkGLArea creates its GL context. Creating the GL
    // context first and reparenting after invalidates the GLX drawable
    // (GLXBadDrawable, fatal). So: realize -> reparent -> add GLArea -> show.
    gtk_widget_realize(window);
    GdkWindow *gdkWindow = gtk_widget_get_window(window);
    Window overlayXid = GDK_WINDOW_XID(gdkWindow);
    Display *display = GDK_WINDOW_XDISPLAY(gdkWindow);

    int w = 1280;
    int h = 720;
    XWindowAttributes attrs;
    if (XGetWindowAttributes(display, player->hostWid, &attrs) != 0) {
        w = attrs.width;
        h = attrs.height;
    }
    XReparentWindow(display, overlayXid, player->hostWid, 0, 0);
    gtk_window_resize(GTK_WINDOW(window), w, h);
    XResizeWindow(display, overlayXid, w, h);

    GtkWidget *glArea = gtk_gl_area_new();
    gtk_gl_area_set_has_alpha(GTK_GL_AREA(glArea), FALSE);
    // auto_render FALSE: render only when mpv has a new frame (our queue_render),
    // and keep the last frame so the overlay can recomposite without forcing a
    // ~60fps GL re-render that contends with the webview overlay (flicker).
    gtk_gl_area_set_auto_render(GTK_GL_AREA(glArea), FALSE);
    g_signal_connect(glArea, "realize", G_CALLBACK(onGlAreaRealize), player);
    g_signal_connect(glArea, "unrealize", G_CALLBACK(onGlAreaUnrealize), player);
    g_signal_connect(glArea, "render", G_CALLBACK(onGlAreaRender), player);
    gtk_widget_add_tick_callback(glArea, onGlAreaTick, player, nullptr);

    // GtkOverlay composites a transparent controls webview over the video GLArea.
    // Both are GTK widgets in one window, so GTK handles the alpha compositing —
    // no X11 sibling-window conflict (the failure mode of the child-reparent path).
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), glArea);

    if (!player->controlsUrl.empty()) {
        WebKitUserContentManager *ucm = webkit_user_content_manager_new();
        webkit_user_content_manager_register_script_message_handler(ucm, "player");
        g_signal_connect(ucm, "script-message-received::player", G_CALLBACK(onScriptMessage), player);
        WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
        GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
        webkit_web_view_set_background_color(web, &transparent);
        WebKitSettings *wkSettings = webkit_web_view_get_settings(web);
        webkit_settings_set_enable_write_console_messages_to_stdout(wkSettings, TRUE);
        // Render via cairo into GTK's draw cycle (not WebKit's own accelerated
        // native surface), so GtkOverlay composites the controls OVER the GLArea
        // video. With HW acceleration the webview's GL surface occludes/escapes
        // the overlay and the controls never appear.
        webkit_settings_set_hardware_acceleration_policy(wkSettings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        g_signal_connect(web, "load-changed", G_CALLBACK(onWebLoadChanged), nullptr);
        g_signal_connect(web, "load-failed", G_CALLBACK(onWebLoadFailed), nullptr);
        // Fill the whole overlay; without explicit FILL alignment a GtkOverlay
        // child is sized to its (zero) natural size and stays invisible.
        gtk_widget_set_halign(GTK_WIDGET(web), GTK_ALIGN_FILL);
        gtk_widget_set_valign(GTK_WIDGET(web), GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(GTK_WIDGET(web), TRUE);
        gtk_widget_set_vexpand(GTK_WIDGET(web), TRUE);
        player->webView = web;
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(web));
    }

    gtk_container_add(GTK_CONTAINER(window), overlay);
    player->gtkWindow = window;
    player->glArea = glArea;

    // Show realizes the GLArea -> GL context is created for the reparented window.
    gtk_widget_show_all(window);
    XMapWindow(display, overlayXid);
    XFlush(display);

    if (player->webView != nullptr) {
        if (std::getenv("NUVIO_LINUX_OVERLAY_TEST") != nullptr) {
            // Diagnostic: a bright bar to confirm the overlay composites over video,
            // independent of the real controls page / JS state.
            webkit_web_view_load_html(player->webView,
                "<html><body style='margin:0;font-family:sans-serif'>"
                "<div style='position:fixed;left:0;right:0;bottom:0;height:90px;"
                "background:rgba(220,30,30,0.7);color:#fff;display:flex;"
                "align-items:center;justify-content:center;font-size:28px'>"
                "OVERLAY TEST &mdash; if you see this, compositing works</div></body></html>",
                nullptr);
        } else {
            webkit_web_view_load_uri(player->webView, player->controlsUrl.c_str());
        }
        // Push live playback state ~4x/sec so the controls track position/state.
        player->syncTimerId = g_timeout_add(250, onSyncTimer, player);
    }
    std::fprintf(stderr, "[nuvio-player] render window embedded over host wid=%lu (%dx%d) controls=%d\n",
                 player->hostWid, w, h, player->webView != nullptr ? 1 : 0);
}

// ---- Floating top-level controls overlay (wid-embedded video path) --------
// Video stays embedded directly in the host AWT window via mpv's "wid" (which
// presents flawlessly). The controls live in a SEPARATE transparent top-level
// window glued over the player: top-level windows are composited with alpha by
// the compositor, unlike sibling child windows or a GtkGLArea-in-GtkOverlay.

// Periodic (GTK thread): keep the overlay positioned/sized over the host window,
// hide it when the host isn't viewable, and push live playback state.
gboolean onFloatingTimer(gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_live.count(player) == 0) {
        return G_SOURCE_REMOVE;
    }
    if (player->gtkWindow != nullptr) {
        GdkWindow *gdkWin = gtk_widget_get_window(player->gtkWindow);
        if (gdkWin != nullptr) {
            Display *dpy = GDK_WINDOW_XDISPLAY(gdkWin);
            XWindowAttributes attrs;
            Window childRet = 0;
            int rx = 0;
            int ry = 0;
            if (XGetWindowAttributes(dpy, player->hostWid, &attrs) != 0 &&
                attrs.map_state == IsViewable &&
                XTranslateCoordinates(dpy, player->hostWid, attrs.root, 0, 0, &rx, &ry, &childRet) != 0) {
                gtk_window_move(GTK_WINDOW(player->gtkWindow), rx, ry);
                gtk_window_resize(GTK_WINDOW(player->gtkWindow), attrs.width, attrs.height);
                if (!gtk_widget_get_visible(player->gtkWindow)) {
                    gtk_widget_show(player->gtkWindow);
                }
                XRaiseWindow(dpy, GDK_WINDOW_XID(gdkWin));
            } else if (gtk_widget_get_visible(player->gtkWindow)) {
                gtk_widget_hide(player->gtkWindow);
            }
        }
    }
    syncControlsOnGtk(player);
    return G_SOURCE_CONTINUE;
}

// Runs on the GTK thread (caller holds g_mutex and verified the player is live).
void createFloatingOverlayOnGtk(PlayerInstance *player) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
    gtk_widget_set_app_paintable(window, TRUE);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window));
    if (rgba != nullptr) {
        gtk_widget_set_visual(window, rgba);
    }

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "player");
    g_signal_connect(ucm, "script-message-received::player", G_CALLBACK(onScriptMessage), player);
    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(web, &transparent);
    WebKitSettings *wkSettings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_write_console_messages_to_stdout(wkSettings, TRUE);
    // Cairo rendering for reliable transparency and no GLX surface in the overlay.
    webkit_settings_set_hardware_acceleration_policy(wkSettings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    g_signal_connect(web, "load-changed", G_CALLBACK(onWebLoadChanged), nullptr);
    g_signal_connect(web, "load-failed", G_CALLBACK(onWebLoadFailed), nullptr);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(web));
    player->gtkWindow = window;
    player->webView = web;

    // Initial geometry from the host window, then realize as override-redirect so
    // the WM doesn't decorate/manage/restack it; the timer keeps it glued.
    gtk_widget_realize(window);
    GdkWindow *gdkWin = gtk_widget_get_window(window);
    gdk_window_set_override_redirect(gdkWin, TRUE);
    Display *dpy = GDK_WINDOW_XDISPLAY(gdkWin);
    XWindowAttributes attrs;
    Window childRet = 0;
    int rx = 0;
    int ry = 0;
    if (XGetWindowAttributes(dpy, player->hostWid, &attrs) != 0) {
        XTranslateCoordinates(dpy, player->hostWid, attrs.root, 0, 0, &rx, &ry, &childRet);
        gtk_window_move(GTK_WINDOW(window), rx, ry);
        gtk_window_resize(GTK_WINDOW(window), attrs.width, attrs.height);
    }
    gtk_widget_show_all(window);
    XRaiseWindow(dpy, GDK_WINDOW_XID(gdkWin));
    XFlush(dpy);

    webkit_web_view_load_uri(web, player->controlsUrl.c_str());
    player->syncTimerId = g_timeout_add(100, onFloatingTimer, player);
    std::fprintf(stderr, "[nuvio-player] floating overlay at %d,%d %dx%d over host wid=%lu\n",
                 rx, ry, attrs.width, attrs.height, player->hostWid);
}

// ---- track-list (audio/subtitle) JSON ------------------------------------
// Produces the JSON shape NativePlayerController deserializes (NativeMpvTrack):
//   [{"index":0,"id":"1","label":"...","language":"eng","selected":true,"forced":false}, ...]

std::string jsonEscape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

const mpv_node *nodeMapGet(const mpv_node *map, const char *key) {
    if (map == nullptr || map->format != MPV_FORMAT_NODE_MAP) {
        return nullptr;
    }
    const mpv_node_list *list = map->u.list;
    for (int i = 0; i < list->num; ++i) {
        if (list->keys[i] != nullptr && std::strcmp(list->keys[i], key) == 0) {
            return &list->values[i];
        }
    }
    return nullptr;
}

std::string nodeString(const mpv_node *node) {
    if (node != nullptr && node->format == MPV_FORMAT_STRING && node->u.string != nullptr) {
        return node->u.string;
    }
    return std::string();
}

int64_t nodeInt(const mpv_node *node) {
    if (node == nullptr) return 0;
    if (node->format == MPV_FORMAT_INT64) return node->u.int64;
    if (node->format == MPV_FORMAT_FLAG) return node->u.flag;
    return 0;
}

bool nodeFlag(const mpv_node *node) {
    if (node == nullptr) return false;
    if (node->format == MPV_FORMAT_FLAG) return node->u.flag != 0;
    if (node->format == MPV_FORMAT_INT64) return node->u.int64 != 0;
    return false;
}

// wantType is mpv's track type: "audio" or "sub".
std::string buildTracksJson(mpv_handle *mpv, const char *wantType) {
    std::string out = "[";
    if (mpv == nullptr) {
        out += "]";
        return out;
    }
    mpv_node node;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_ARRAY) {
            const mpv_node_list *tracks = node.u.list;
            int ordinal = 0;
            for (int i = 0; i < tracks->num; ++i) {
                const mpv_node *track = &tracks->values[i];
                if (track->format != MPV_FORMAT_NODE_MAP) continue;
                if (nodeString(nodeMapGet(track, "type")) != wantType) continue;

                const int64_t id = nodeInt(nodeMapGet(track, "id"));
                const std::string title = nodeString(nodeMapGet(track, "title"));
                const std::string lang = nodeString(nodeMapGet(track, "lang"));
                const bool selected = nodeFlag(nodeMapGet(track, "selected"));
                const bool forced = nodeFlag(nodeMapGet(track, "forced"));

                char buf[64];
                if (ordinal > 0) out += ",";
                out += "{\"index\":";
                std::snprintf(buf, sizeof(buf), "%d", ordinal);
                out += buf;
                out += ",\"id\":\"";
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(id));
                out += buf;
                out += "\",\"label\":\"";
                out += jsonEscape(title);
                out += "\",\"language\":\"";
                out += jsonEscape(lang);
                out += "\",\"selected\":";
                out += selected ? "true" : "false";
                out += ",\"forced\":";
                out += forced ? "true" : "false";
                out += "}";
                ++ordinal;
            }
        }
        mpv_free_node_contents(&node);
    }
    out += "]";
    return out;
}

// Drains the mpv event queue so the core stays responsive. Phase 4 will route
// selected events (and overlay messages) back to Kotlin via eventSink here.
void runEventLoop(PlayerInstance *player) {
    while (player->running.load()) {
        mpv_event *event = mpv_wait_event(player->mpv, 0.1);
        if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
            continue;
        }
        switch (event->event_id) {
            case MPV_EVENT_SHUTDOWN:
                return;
            case MPV_EVENT_LOG_MESSAGE: {
                auto *msg = static_cast<mpv_event_log_message *>(event->data);
                std::fprintf(stderr, "[nuvio-player][mpv:%s] %s: %s", msg->level, msg->prefix, msg->text);
                break;
            }
            case MPV_EVENT_START_FILE:
                std::fprintf(stderr, "[nuvio-player] start-file\n");
                break;
            case MPV_EVENT_FILE_LOADED:
                std::fprintf(stderr, "[nuvio-player] file-loaded\n");
                break;
            case MPV_EVENT_END_FILE: {
                auto *ef = static_cast<mpv_event_end_file *>(event->data);
                std::fprintf(stderr, "[nuvio-player] end-file reason=%d error=%s\n",
                             ef->reason, mpv_error_string(ef->error));
                break;
            }
            default:
                break;
        }
    }
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_create(
    JNIEnv *env,
    jobject,
    jlong hostViewPtr,
    jstring sourceUrl,
    jobjectArray headerLines,
    jboolean playWhenReady,
    jlong initialPositionMs,
    jstring controlsPageUrl,
    jint decoderPriority,
    jobject eventSink) {

    const std::string url = jstringToUtf8(env, sourceUrl);
    if (url.empty()) {
        return 0;
    }

    // libmpv refuses to run (and abort()s the process) under a non-C numeric
    // locale. The JVM sets the process locale from the environment, so force
    // LC_NUMERIC=C before touching mpv. Java number formatting uses
    // java.util.Locale, not the C library locale, so this is safe.
    std::setlocale(LC_NUMERIC, "C");

    auto *player = new PlayerInstance();
    env->GetJavaVM(&player->jvm);
    player->hostWid = static_cast<unsigned long>(hostViewPtr);
    player->controlsUrl = jstringToUtf8(env, controlsPageUrl);
    // Linux renders in software into a Compose Canvas (the only path that avoids
    // the native-window + overlay compositing problems). It is the default; the
    // env vars below remain only for comparing the abandoned approaches.
    player->renderMode = (std::getenv("NUVIO_LINUX_RENDER") != nullptr);
    player->swMode = !player->renderMode && (std::getenv("NUVIO_LINUX_WID") == nullptr);
    if (eventSink != nullptr) {
        player->eventSink = env->NewGlobalRef(eventSink);
        jclass sinkClass = env->GetObjectClass(eventSink);
        player->onEventMethod = env->GetMethodID(sinkClass, "onPlayerEvent", "(Ljava/lang/String;D)V");
        env->DeleteLocalRef(sinkClass);
    }

    player->mpv = mpv_create();
    if (player->mpv == nullptr) {
        if (player->eventSink != nullptr) {
            env->DeleteGlobalRef(player->eventSink);
        }
        delete player;
        return 0;
    }

    mpv_handle *mpv = player->mpv;

    // Options that must be set before mpv_initialize.
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "input-default-bindings", "no");
    mpv_set_option_string(mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv, "keep-open", "yes");
    // Hardware decode preference, matching the Windows/macOS bridges.
    // decoderPriority: 0 = hardware-strict (no software fallback),
    // 2 = CPU/software (hwdec off), else (1) = auto with software fallback.
    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "hwdec-codecs", "all");
    if (decoderPriority == 0) {
        mpv_set_option_string(mpv, "vd-lavc-software-fallback", "no");
    } else if (decoderPriority == 2) {
        mpv_set_option_string(mpv, "hwdec", "no");
        mpv_set_option_string(mpv, "vd-lavc-software-fallback", "yes");
    } else {
        mpv_set_option_string(mpv, "vd-lavc-software-fallback", "yes");
    }
    mpv_set_option_string(mpv, "force-window", "no");
    if (player->swMode || player->renderMode) {
        // libmpv VO: we drive rendering via the render API (SW buffer or GL FBO).
        mpv_set_option_string(mpv, "vo", "libmpv");
    } else {
        mpv_set_option_string(mpv, "vo", "gpu-next");
        // Embedding via "wid" is X11-only. On a Wayland session mpv's auto context
        // would create its own wl_surface and ignore wid; force an X11 GLX context
        // (EGL fails to make its context current on the foreign AWT window).
        mpv_set_option_string(mpv, "gpu-context", "x11");
        // Don't block on vsync: when the controls overlay occludes the mpv window,
        // the compositor stops frame callbacks and a vsync-locked swap stalls.
        mpv_set_option_string(mpv, "opengl-swapinterval", "0");
        mpv_set_option_string(mpv, "video-sync", "audio");
    }

    // Initial playback state, applied to the first loaded file. Set as options
    // before mpv_initialize rather than as positional loadfile arguments (the
    // loadfile option/index positions differ across mpv versions).
    mpv_set_option_string(mpv, "pause", playWhenReady ? "no" : "yes");
    if (initialPositionMs > 0) {
        char startBuf[64];
        std::snprintf(startBuf, sizeof(startBuf), "%.3f", initialPositionMs / 1000.0);
        mpv_set_option_string(mpv, "start", startBuf);
    }

    // HTTP headers: mpv expects a comma-separated list of "Field: value".
    if (headerLines != nullptr) {
        const jsize count = env->GetArrayLength(headerLines);
        std::string headerValue;
        for (jsize i = 0; i < count; ++i) {
            auto line = reinterpret_cast<jstring>(env->GetObjectArrayElement(headerLines, i));
            const std::string headerLine = jstringToUtf8(env, line);
            env->DeleteLocalRef(line);
            if (headerLine.empty()) {
                continue;
            }
            if (!headerValue.empty()) {
                headerValue += ",";
            }
            headerValue += headerLine;
        }
        if (!headerValue.empty()) {
            mpv_set_option_string(mpv, "http-header-fields", headerValue.c_str());
        }
    }

    // Embed mpv into the host AWT Canvas X11 window (its XID, from
    // LinuxAwtViewResolver.getWindow()). Render mode embeds via the GtkGLArea
    // instead, so wid is only set for the direct-embedding path.
    int64_t wid = static_cast<int64_t>(hostViewPtr);
    if (!player->renderMode && !player->swMode) {
        mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);
    }

    if (mpv_initialize(mpv) < 0) {
        std::fprintf(stderr, "[nuvio-player] mpv_initialize failed (wid=%lld)\n",
                     static_cast<long long>(wid));
        mpv_terminate_destroy(mpv);
        if (player->eventSink != nullptr) {
            env->DeleteGlobalRef(player->eventSink);
        }
        delete player;
        return 0;
    }

    // Surface mpv warnings/errors to the run console for diagnostics.
    mpv_request_log_messages(mpv, "warn");

    if (player->swMode) {
        mpv_render_param swParams[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&player->renderCtx, mpv, swParams) < 0) {
            std::fprintf(stderr, "[nuvio-player] SW render context create failed\n");
            player->renderCtx = nullptr;
        } else {
            std::fprintf(stderr, "[nuvio-player] SW render context created\n");
        }
    }

    // Start draining events before we load.
    player->running.store(true);
    player->eventThread = std::thread(runEventLoop, player);

    std::fprintf(stderr, "[nuvio-player] create: wid=%lld url=%s\n",
                 static_cast<long long>(wid), url.c_str());
    command(mpv, {"loadfile", url.c_str(), "replace"});

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_live.insert(player);
    }

    // Set up the GTK-side window. Re-check liveness on the GTK thread in case
    // dispose() raced ahead before the task ran.
    if (player->swMode) {
        // No GTK window: Kotlin pulls frames via renderFrame() and draws them in a
        // Compose Canvas; the shared Compose PlayerControlsShell draws over them.
    } else if (player->renderMode) {
        // Render-API path: mpv renders into a GtkGLArea embedded in the AWT window.
        runOnGtk([player]() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_live.count(player) != 0) {
                createRenderWindowOnGtk(player);
            }
        });
    } else {
        // wid-embedded video + a floating top-level controls overlay glued over it.
        static const bool overlayEnabled = std::getenv("NUVIO_LINUX_OVERLAY") != nullptr;
        if (overlayEnabled && !player->controlsUrl.empty()) {
            runOnGtk([player]() {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_live.count(player) != 0) {
                    createFloatingOverlayOnGtk(player);
                }
            });
        }
    }

    return reinterpret_cast<jlong>(player);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_dispose(JNIEnv *env, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    // Deregister first so any concurrent query/transport call (blocked on
    // g_mutex) sees it gone and returns instead of touching freed state.
    g_live.erase(player);

    // Stop the event thread before tearing anything down.
    player->running.store(false);
    if (player->mpv != nullptr) {
        mpv_wakeup(player->mpv);
    }
    if (player->eventThread.joinable()) {
        player->eventThread.join();
    }
    if (player->eventSink != nullptr) {
        env->DeleteGlobalRef(player->eventSink);
        player->eventSink = nullptr;
    }

    if (player->swMode) {
        // SW render context has no GL resources; free it here, then mpv.
        if (player->renderCtx != nullptr) {
            mpv_render_context_free(player->renderCtx);
            player->renderCtx = nullptr;
        }
        if (player->mpv != nullptr) {
            mpv_terminate_destroy(player->mpv);
            player->mpv = nullptr;
        }
        delete player;
        return;
    }

    if (player->renderMode) {
        // Render context + mpv must be torn down on the GTK thread (GL owner):
        // render_context_free before terminate_destroy, and the window destroyed
        // last so no further render callback touches the instance. The GTK task
        // owns the instance and frees it.
        runOnGtk([player]() {
            if (player->syncTimerId != 0) {
                g_source_remove(player->syncTimerId);
                player->syncTimerId = 0;
            }
            if (player->glArea != nullptr) {
                gtk_gl_area_make_current(GTK_GL_AREA(player->glArea));
            }
            if (player->renderCtx != nullptr) {
                mpv_render_context_free(player->renderCtx);
                player->renderCtx = nullptr;
            }
            GtkWidget *window = player->gtkWindow;
            player->gtkWindow = nullptr;
            player->glArea = nullptr;
            if (window != nullptr) {
                gtk_widget_destroy(window);
            }
            if (player->mpv != nullptr) {
                mpv_terminate_destroy(player->mpv);
                player->mpv = nullptr;
            }
            delete player;
        });
        return;
    }

    // wid path: stop the floating overlay timer and destroy its window (if any)
    // on the GTK thread, then terminate mpv here.
    if (player->syncTimerId != 0) {
        g_source_remove(player->syncTimerId);
        player->syncTimerId = 0;
    }
    GtkWidget *overlay = player->gtkWindow;
    player->gtkWindow = nullptr;
    player->webView = nullptr;
    if (overlay != nullptr) {
        runOnGtk([overlay]() { gtk_widget_destroy(overlay); });
    }
    if (player->mpv != nullptr) {
        mpv_terminate_destroy(player->mpv);
        player->mpv = nullptr;
    }
    delete player;
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setPaused(JNIEnv *, jobject, jlong handle, jboolean paused) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) {
        setFlag(player->mpv, "pause", paused == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekTo(JNIEnv *, jobject, jlong handle, jlong positionMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    char seconds[64];
    std::snprintf(seconds, sizeof(seconds), "%.3f", positionMs / 1000.0);
    command(player->mpv, {"seek", seconds, "absolute+keyframes"});
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekBy(JNIEnv *, jobject, jlong handle, jlong offsetMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    char seconds[64];
    std::snprintf(seconds, sizeof(seconds), "%.3f", offsetMs / 1000.0);
    command(player->mpv, {"seek", seconds, "relative+keyframes"});
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSpeed(JNIEnv *, jobject, jlong handle, jfloat speed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) {
        setDouble(player->mpv, "speed", static_cast<double>(speed));
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_adjustVolume(JNIEnv *, jobject, jlong handle, jfloat delta) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    double volume = getDouble(player->mpv, "volume", 100.0) + static_cast<double>(delta);
    if (volume < 0.0) volume = 0.0;
    if (volume > 130.0) volume = 130.0;
    setDouble(player->mpv, "volume", volume);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setResizeMode(JNIEnv *, jobject, jlong handle, jint mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    // 0 = Fit (keep aspect, letterbox), 1 = Fill (crop to fill), 2 = Zoom.
    switch (mode) {
        case 1:
            mpv_set_property_string(player->mpv, "panscan", "1.0");
            break;
        case 2:
            mpv_set_property_string(player->mpv, "panscan", "1.0");
            break;
        default:
            mpv_set_property_string(player->mpv, "panscan", "0.0");
            break;
    }
}

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_durationMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return 0;
    return static_cast<jlong>(getDouble(player->mpv, "duration", 0.0) * 1000.0);
}

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_positionMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return 0;
    return static_cast<jlong>(getDouble(player->mpv, "time-pos", 0.0) * 1000.0);
}

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_bufferedPositionMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return 0;
    // demuxer-cache-time is the absolute timestamp the cache currently reaches.
    return static_cast<jlong>(getDouble(player->mpv, "demuxer-cache-time", 0.0) * 1000.0);
}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isLoading(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return JNI_TRUE;
    const bool loading = getFlag(player->mpv, "paused-for-cache", false) ||
                         getFlag(player->mpv, "seeking", false);
    return loading ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isEnded(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return JNI_FALSE;
    return getFlag(player->mpv, "eof-reached", false) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isPaused(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return JNI_TRUE;
    return getFlag(player->mpv, "pause", false) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_speed(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return 1.0f;
    return static_cast<jfloat>(getDouble(player->mpv, "speed", 1.0));
}

// ---- Software-render frame pull (Compose) ---------------------------------
// Renders the current mpv frame into the caller's direct ByteBuffer as RGBA8888.
// Returns true if a frame was rendered. Kotlin draws it in a Compose Canvas.

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_renderFrame(
    JNIEnv *env, jobject, jlong handle, jint width, jint height, jobject buffer) {
    if (width <= 0 || height <= 0 || buffer == nullptr) {
        return JNI_FALSE;
    }
    void *dst = env->GetDirectBufferAddress(buffer);
    if (dst == nullptr) {
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr || player->renderCtx == nullptr) {
        return JNI_FALSE;
    }
    int size[2] = {static_cast<int>(width), static_cast<int>(height)};
    size_t stride = static_cast<size_t>(width) * 4;
    char swFormat[] = "rgba"; // bytes R,G,B,A -> Skia ColorType.RGBA_8888
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, swFormat},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, dst},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return mpv_render_context_render(player->renderCtx, params) >= 0 ? JNI_TRUE : JNI_FALSE;
}

// ---- Track enumeration ----------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_audioTracksJson(JNIEnv *env, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return env->NewStringUTF("[]");
    }
    return env->NewStringUTF(buildTracksJson(player->mpv, "audio").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_subtitleTracksJson(JNIEnv *env, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return env->NewStringUTF("[]");
    }
    return env->NewStringUTF(buildTracksJson(player->mpv, "sub").c_str());
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectAudioTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    if (trackId < 0) {
        mpv_set_property_string(player->mpv, "aid", "no");
    } else {
        mpv_set_property_string(player->mpv, "aid", std::to_string(trackId).c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectSubtitleTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    if (trackId < 0) {
        mpv_set_property_string(player->mpv, "sid", "no");
    } else {
        mpv_set_property_string(player->mpv, "sid", std::to_string(trackId).c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_addSubtitleUrl(JNIEnv *env, jobject, jlong handle, jstring url) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    const std::string subUrl = jstringToUtf8(env, url);
    if (!subUrl.empty()) {
        command(player->mpv, {"sub-add", subUrl.c_str(), "select"});
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitles(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) {
        mpv_set_property_string(player->mpv, "sid", "no");
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitlesAndSelect(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    if (trackId < 0) {
        mpv_set_property_string(player->mpv, "sid", "no");
    } else {
        mpv_set_property_string(player->mpv, "sid", std::to_string(trackId).c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSubtitleDelayMs(JNIEnv *, jobject, jlong handle, jint delayMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) {
        setDouble(player->mpv, "sub-delay", delayMs / 1000.0);
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applySubtitleStyle(
    JNIEnv *env,
    jobject,
    jlong handle,
    jstring textColor,
    jstring backgroundColor,
    jstring outlineColor,
    jfloat outlineSize,
    jboolean bold,
    jfloat fontSize,
    jint subPos) {

    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    mpv_handle *mpv = player->mpv;

    const std::string text = jstringToUtf8(env, textColor);
    const std::string back = jstringToUtf8(env, backgroundColor);
    const std::string outline = jstringToUtf8(env, outlineColor);

    if (!text.empty()) mpv_set_property_string(mpv, "sub-color", text.c_str());
    if (!back.empty()) mpv_set_property_string(mpv, "sub-back-color", back.c_str());
    if (!outline.empty()) mpv_set_property_string(mpv, "sub-border-color", outline.c_str());
    setDouble(mpv, "sub-border-size", static_cast<double>(outlineSize));
    mpv_set_property_string(mpv, "sub-bold", bold == JNI_TRUE ? "yes" : "no");
    if (fontSize > 0.0f) setDouble(mpv, "sub-font-size", static_cast<double>(fontSize));
    mpv_set_property_string(mpv, "sub-pos", std::to_string(subPos).c_str());
}

// ---- Controls overlay + window chrome: Phase 4/5 (stubs) ------------------

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateControls(JNIEnv *env, jobject, jlong handle, jstring controlsJson) {
    const std::string json = jstringToUtf8(env, controlsJson);
    std::lock_guard<std::mutex> lock(g_mutex);
    PlayerInstance *player = liveLocked(handle);
    if (player == nullptr) {
        return;
    }
    // Cache the latest structural state. If the page is ready, flush now; if not
    // (it loads asynchronously), controlsReady will flush it later.
    player->pendingControlsJson = json;
    if (player->webView != nullptr && player->controlsReady) {
        runOnGtk([player]() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_live.count(player) != 0) {
                flushControlsOnGtk(player);
            }
        });
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applyWindowChrome(
    JNIEnv *, jobject, jlong, jboolean, jint, jint, jint) {
    // No-op: Windows DWM chrome has no portable X11 equivalent (Phase 5).
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setWindowBorderlessFullscreen(
    JNIEnv *, jobject, jlong, jboolean, jint, jint, jint, jint) {
    // No-op: handled at the Compose/AWT layer for now (Phase 5 may add X11 hints).
}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_warmupWebView2(JNIEnv *, jobject, jstring) {
    return JNI_FALSE; // No WebView2 on Linux; WebKitGTK overlay is Phase 4.
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_shutdownWebView2Warmup(JNIEnv *, jobject) {
    // No-op.
}

} // extern "C"
