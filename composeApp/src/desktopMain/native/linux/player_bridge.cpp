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
    mpv_render_context *renderCtx = nullptr;
    GtkWidget *glArea = nullptr;
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
        dispatchEvent(player, typeStr, value);
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
    int advanced = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
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

void onGlAreaUnrealize(GtkGLArea *area, gpointer data) {
    auto *player = static_cast<PlayerInstance *>(data);
    gtk_gl_area_make_current(area);
    if (player->renderCtx != nullptr) {
        mpv_render_context_free(player->renderCtx);
        player->renderCtx = nullptr;
    }
}

// Runs on the GTK thread (caller holds g_mutex and verified the player is live).
void createRenderWindowOnGtk(PlayerInstance *player) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

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
    gtk_gl_area_set_auto_render(GTK_GL_AREA(glArea), TRUE);
    g_signal_connect(glArea, "realize", G_CALLBACK(onGlAreaRealize), player);
    g_signal_connect(glArea, "unrealize", G_CALLBACK(onGlAreaUnrealize), player);
    g_signal_connect(glArea, "render", G_CALLBACK(onGlAreaRender), player);
    gtk_container_add(GTK_CONTAINER(window), glArea);
    player->gtkWindow = window;
    player->glArea = glArea;

    // Show realizes the GLArea -> GL context is created for the reparented window.
    gtk_widget_show_all(window);
    XMapWindow(display, overlayXid);
    XFlush(display);
    std::fprintf(stderr, "[nuvio-player] render window embedded over host wid=%lu (%dx%d)\n",
                 player->hostWid, w, h);
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
    player->renderMode = (std::getenv("NUVIO_LINUX_RENDER") != nullptr);
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
    mpv_set_option_string(mpv, "hwdec", "auto-safe");
    mpv_set_option_string(mpv, "force-window", "no");
    if (player->renderMode) {
        // Render API: mpv renders into our GtkGLArea's GL context (no own window).
        mpv_set_option_string(mpv, "vo", "libmpv");
    } else {
        mpv_set_option_string(mpv, "vo", "gpu-next");
        // Embedding via "wid" is X11-only. On a Wayland session mpv's auto context
        // would create its own wl_surface and ignore wid; force an X11 GLX context
        // (EGL fails to make its context current on the foreign AWT window).
        mpv_set_option_string(mpv, "gpu-context", "x11");
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
    if (!player->renderMode) {
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
    if (player->renderMode) {
        // Render-API path: mpv renders into a GtkGLArea embedded in the AWT window.
        runOnGtk([player]() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_live.count(player) != 0) {
                createRenderWindowOnGtk(player);
            }
        });
    } else {
        // Legacy child-reparent overlay (broken; see NUVIO_LINUX_OVERLAY notes).
        static const bool overlayEnabled = std::getenv("NUVIO_LINUX_OVERLAY") != nullptr;
        if (overlayEnabled && !player->controlsUrl.empty()) {
            runOnGtk([player]() {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_live.count(player) != 0) {
                    createOverlayOnGtk(player);
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

    if (player->renderMode) {
        // Render context + mpv must be torn down on the GTK thread (GL owner):
        // render_context_free before terminate_destroy, and the window destroyed
        // last so no further render callback touches the instance. The GTK task
        // owns the instance and frees it.
        runOnGtk([player]() {
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

    // Legacy wid / child-overlay path: destroy the overlay window (if any) on the
    // GTK thread, then terminate mpv here.
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        PlayerInstance *player = liveLocked(handle);
        if (player == nullptr || player->webView == nullptr) {
            return;
        }
    }
    auto *player = reinterpret_cast<PlayerInstance *>(handle);
    const std::string script = "window.playerUpdate(" + jstringToUtf8(env, controlsJson) + ");";
    runOnGtk([player, script]() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_live.count(player) == 0 || player->webView == nullptr) {
            return;
        }
        webkit_web_view_evaluate_javascript(player->webView, script.c_str(), -1,
                                            nullptr, nullptr, nullptr, nullptr, nullptr);
    });
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
