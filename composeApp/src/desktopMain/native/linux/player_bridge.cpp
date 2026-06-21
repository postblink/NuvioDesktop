// Linux JNI player bridge for Nuvio Desktop.
//
// Linux renders mpv in *software* (MPV_RENDER_API_TYPE_SW) into a CPU buffer that
// Kotlin draws in a Compose Canvas (see LinuxComposePlayer.kt); the app's shared
// Compose PlayerControlsShell draws over it. This avoids embedding mpv into a
// native window and compositing a native controls overlay on top — a Windows/macOS
// pattern that X11 does not support (no alpha compositing of overlay windows over
// the embedded video). Earlier wid-embedding + WebKitGTK/GtkGLArea/floating-window
// attempts hit X11/GLX/occlusion walls and were removed.
//
// Built against system libmpv via pkg-config. Implements the NativePlayerBridge
// JNI contract; window/overlay-only methods (updateControls, applyWindowChrome,
// setWindowBorderlessFullscreen, warmup/shutdownWebView2) are no-ops here.

#include <jni.h>
#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct PlayerInstance {
    mpv_handle *mpv = nullptr;
    mpv_render_context *renderCtx = nullptr;
    std::thread eventThread;
    std::atomic<bool> running{false};
};

// Live-instance registry: a jlong handle is a PlayerInstance*. Every JNI call
// validates the handle under g_mutex so a stale handle (e.g. a call racing
// dispose) returns safely instead of touching freed memory.
std::mutex g_mutex;
std::set<PlayerInstance *> g_live;

PlayerInstance *liveLocked(jlong handle) {
    auto *player = reinterpret_cast<PlayerInstance *>(handle);
    return (player != nullptr && g_live.count(player) != 0) ? player : nullptr;
}

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
    if (mpv == nullptr) return;
    int flag = value ? 1 : 0;
    mpv_set_property(mpv, name, MPV_FORMAT_FLAG, &flag);
}

void setDouble(mpv_handle *mpv, const char *name, double value) {
    if (mpv == nullptr) return;
    mpv_set_property(mpv, name, MPV_FORMAT_DOUBLE, &value);
}

void command(mpv_handle *mpv, std::vector<const char *> args) {
    if (mpv == nullptr) return;
    args.push_back(nullptr);
    mpv_command(mpv, args.data());
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

// Drains the mpv event queue so the core stays responsive.
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
    jlong /* hostViewPtr */,
    jstring sourceUrl,
    jobjectArray headerLines,
    jboolean playWhenReady,
    jlong initialPositionMs,
    jstring /* controlsPageUrl */,
    jint decoderPriority,
    jboolean /* nvidiaRtxSuperResolutionEnabled */, // VSR is Windows-only; ignored here
    jobject /* eventSink */) {

    const std::string url = jstringToUtf8(env, sourceUrl);
    if (url.empty()) {
        return 0;
    }

    // libmpv aborts the process under a non-C numeric locale; force it. Java
    // formatting uses java.util.Locale, not the C library locale, so this is safe.
    std::setlocale(LC_NUMERIC, "C");

    auto *player = new PlayerInstance();
    player->mpv = mpv_create();
    if (player->mpv == nullptr) {
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
    mpv_set_option_string(mpv, "vo", "libmpv"); // render API drives output

    // Hardware decode preference (matches the Windows/macOS bridges).
    // decoderPriority: 0 = hardware-strict, 2 = CPU/software, else = auto+fallback.
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

    // HTTP headers: mpv expects a comma-separated list of "Field: value".
    if (headerLines != nullptr) {
        const jsize count = env->GetArrayLength(headerLines);
        std::string headerValue;
        for (jsize i = 0; i < count; ++i) {
            auto line = reinterpret_cast<jstring>(env->GetObjectArrayElement(headerLines, i));
            const std::string headerLine = jstringToUtf8(env, line);
            env->DeleteLocalRef(line);
            if (headerLine.empty()) continue;
            if (!headerValue.empty()) headerValue += ",";
            headerValue += headerLine;
        }
        if (!headerValue.empty()) {
            mpv_set_option_string(mpv, "http-header-fields", headerValue.c_str());
        }
    }

    // Initial playback state, applied to the first loaded file (positional
    // loadfile args vary across mpv versions, so use options).
    mpv_set_option_string(mpv, "pause", playWhenReady == JNI_TRUE ? "no" : "yes");
    if (initialPositionMs > 0) {
        char startBuf[64];
        std::snprintf(startBuf, sizeof(startBuf), "%.3f", initialPositionMs / 1000.0);
        mpv_set_option_string(mpv, "start", startBuf);
    }

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        delete player;
        return 0;
    }

    mpv_request_log_messages(mpv, "warn");

    // Software render context: Kotlin pulls frames via renderFrame().
    mpv_render_param swParams[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&player->renderCtx, mpv, swParams) < 0) {
        std::fprintf(stderr, "[nuvio-player] SW render context create failed\n");
        player->renderCtx = nullptr;
    }

    player->running.store(true);
    player->eventThread = std::thread(runEventLoop, player);

    command(mpv, {"loadfile", url.c_str(), "replace"});

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_live.insert(player);
    }
    return reinterpret_cast<jlong>(player);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_dispose(JNIEnv *, jobject, jlong handle) {
    PlayerInstance *player = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        player = liveLocked(handle);
        if (player == nullptr) {
            return;
        }
        // Deregister first so any concurrent JNI call (blocked on g_mutex) sees it
        // gone and returns instead of touching freed state.
        g_live.erase(player);
    }
    player->running.store(false);
    if (player->mpv != nullptr) {
        mpv_wakeup(player->mpv);
    }
    if (player->eventThread.joinable()) {
        player->eventThread.join();
    }
    if (player->renderCtx != nullptr) {
        mpv_render_context_free(player->renderCtx);
        player->renderCtx = nullptr;
    }
    if (player->mpv != nullptr) {
        mpv_terminate_destroy(player->mpv);
        player->mpv = nullptr;
    }
    delete player;
}

// ---- Software-render frame pull (Compose) ---------------------------------
// Renders the current mpv frame into the caller's direct ByteBuffer as RGBA8888.
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

// ---- transport ------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setPaused(JNIEnv *, jobject, jlong handle, jboolean paused) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) setFlag(player->mpv, "pause", paused == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekTo(JNIEnv *, jobject, jlong handle, jlong positionMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    char seconds[64];
    std::snprintf(seconds, sizeof(seconds), "%.3f", positionMs / 1000.0);
    command(player->mpv, {"seek", seconds, "absolute+keyframes"});
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekBy(JNIEnv *, jobject, jlong handle, jlong offsetMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    char seconds[64];
    std::snprintf(seconds, sizeof(seconds), "%.3f", offsetMs / 1000.0);
    command(player->mpv, {"seek", seconds, "relative+keyframes"});
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSpeed(JNIEnv *, jobject, jlong handle, jfloat speed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) setDouble(player->mpv, "speed", static_cast<double>(speed));
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_adjustVolume(JNIEnv *, jobject, jlong handle, jfloat delta) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    double volume = getDouble(player->mpv, "volume", 100.0) + static_cast<double>(delta);
    if (volume < 0.0) volume = 0.0;
    if (volume > 130.0) volume = 130.0;
    setDouble(player->mpv, "volume", volume);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setVolume(JNIEnv *, jobject, jlong handle, jfloat level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    double volume = static_cast<double>(level);
    if (volume < 0.0) volume = 0.0;
    if (volume > 130.0) volume = 130.0;
    setDouble(player->mpv, "volume", volume);
}

JNIEXPORT jfloat JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_volume(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return player == nullptr ? 100.0f : static_cast<jfloat>(getDouble(player->mpv, "volume", 100.0));
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setResizeMode(JNIEnv *, jobject, jlong handle, jint mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    // 0 = Fit (letterbox), 1 = Fill, 2 = Zoom, 3 = Stretch (ignore aspect ratio).
    if (mode == 3) {
        mpv_set_property_string(player->mpv, "keepaspect", "no");
        mpv_set_property_string(player->mpv, "panscan", "0.0");
    } else {
        mpv_set_property_string(player->mpv, "keepaspect", "yes");
        mpv_set_property_string(player->mpv, "panscan", mode == 0 ? "0.0" : "1.0");
    }
}

// ---- state queries --------------------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_durationMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return player == nullptr ? 0 : static_cast<jlong>(getDouble(player->mpv, "duration", 0.0) * 1000.0);
}

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_positionMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return player == nullptr ? 0 : static_cast<jlong>(getDouble(player->mpv, "time-pos", 0.0) * 1000.0);
}

JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_bufferedPositionMs(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return player == nullptr ? 0 : static_cast<jlong>(getDouble(player->mpv, "demuxer-cache-time", 0.0) * 1000.0);
}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isLoading(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return JNI_TRUE;
    const bool loading = getFlag(player->mpv, "paused-for-cache", false) || getFlag(player->mpv, "seeking", false);
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
    return player == nullptr ? 1.0f : static_cast<jfloat>(getDouble(player->mpv, "speed", 1.0));
}

// ---- tracks / subtitles ---------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_audioTracksJson(JNIEnv *env, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return env->NewStringUTF(player == nullptr ? "[]" : buildTracksJson(player->mpv, "audio").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_subtitleTracksJson(JNIEnv *env, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    return env->NewStringUTF(player == nullptr ? "[]" : buildTracksJson(player->mpv, "sub").c_str());
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectAudioTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    mpv_set_property_string(player->mpv, "aid", trackId < 0 ? "no" : std::to_string(trackId).c_str());
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectSubtitleTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    mpv_set_property_string(player->mpv, "sid", trackId < 0 ? "no" : std::to_string(trackId).c_str());
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_addSubtitleUrl(JNIEnv *env, jobject, jlong handle, jstring url) {
    const std::string subUrl = jstringToUtf8(env, url);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr && !subUrl.empty()) {
        command(player->mpv, {"sub-add", subUrl.c_str(), "select"});
    }
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitles(JNIEnv *, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) mpv_set_property_string(player->mpv, "sid", "no");
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitlesAndSelect(JNIEnv *, jobject, jlong handle, jint trackId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    mpv_set_property_string(player->mpv, "sid", trackId < 0 ? "no" : std::to_string(trackId).c_str());
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSubtitleDelayMs(JNIEnv *, jobject, jlong handle, jint delayMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player != nullptr) setDouble(player->mpv, "sub-delay", delayMs / 1000.0);
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applySubtitleStyle(
    JNIEnv *env, jobject, jlong handle,
    jstring textColor, jstring backgroundColor, jstring outlineColor,
    jfloat outlineSize, jboolean bold, jfloat fontSize, jint subPos) {
    const std::string text = jstringToUtf8(env, textColor);
    const std::string back = jstringToUtf8(env, backgroundColor);
    const std::string outline = jstringToUtf8(env, outlineColor);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto *player = liveLocked(handle);
    if (player == nullptr) return;
    mpv_handle *mpv = player->mpv;
    if (!text.empty()) mpv_set_property_string(mpv, "sub-color", text.c_str());
    if (!back.empty()) mpv_set_property_string(mpv, "sub-back-color", back.c_str());
    if (!outline.empty()) mpv_set_property_string(mpv, "sub-border-color", outline.c_str());
    setDouble(mpv, "sub-border-size", static_cast<double>(outlineSize));
    mpv_set_property_string(mpv, "sub-bold", bold == JNI_TRUE ? "yes" : "no");
    if (fontSize > 0.0f) setDouble(mpv, "sub-font-size", static_cast<double>(fontSize));
    mpv_set_property_string(mpv, "sub-pos", std::to_string(subPos).c_str());
}

// ---- window/overlay-only methods: no-ops on Linux (Compose controls) ------

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateControls(JNIEnv *, jobject, jlong, jstring) {}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applyWindowChrome(JNIEnv *, jobject, jlong, jboolean, jint, jint, jint) {}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setWindowBorderlessFullscreen(JNIEnv *, jobject, jlong, jboolean, jint, jint, jint, jint) {}

JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_warmupWebView2(JNIEnv *, jobject, jstring) {
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_shutdownWebView2Warmup(JNIEnv *, jobject) {}

} // extern "C"
