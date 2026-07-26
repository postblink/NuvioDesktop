package com.nuvio.app.features.player

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asComposeImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntSize
import com.nuvio.app.features.player.desktop.NativePlayerBridge
import com.nuvio.app.features.player.desktop.NativePlayerEventSink
import com.nuvio.app.features.player.desktop.isDesktopAppFullscreen
import com.nuvio.app.features.player.desktop.toggleDesktopAppFullscreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import org.jetbrains.skia.Bitmap
import org.jetbrains.skia.ColorAlphaType
import org.jetbrains.skia.ColorType
import org.jetbrains.skia.ImageInfo
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.Volatile

/**
 * Linux desktop player surface: mpv renders frames in software into a CPU buffer
 * (NativePlayerBridge.renderFrame) which we draw in a Compose Canvas. The shared
 * Compose PlayerControlsShell (composed by the player screen) draws over it, so no
 * native window / webview overlay is needed — sidestepping the X11/GLX/compositing
 * issues of embedding mpv + a native overlay into the JVM/AWT window.
 *
 * Requires the native bridge to be in software mode (NUVIO_LINUX_SW).
 */
@Composable
internal fun LinuxComposePlayerSurface(
    sourceUrl: String,
    sourceHeaders: Map<String, String>,
    modifier: Modifier,
    playWhenReady: Boolean,
    resizeMode: PlayerResizeMode,
    initialPositionMs: Long,
    initialPositionRequestKey: String?,
    decoderPriority: Int,
    onInitialPositionHandled: (key: String, handled: Boolean) -> Unit,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
    onError: (String?) -> Unit,
) {
    val controller = remember { LinuxComposePlayerController() }
    var frame by remember { mutableStateOf<ImageBitmap?>(null) }
    var surfaceSize by remember { mutableStateOf(IntSize.Zero) }

    LaunchedEffect(controller) { onControllerReady(controller) }

    // Create the player off the main thread. Each request gets a generation so a
    // cancelled native create can never overwrite a newer source when it returns.
    LaunchedEffect(sourceUrl, sourceHeaders, initialPositionMs, initialPositionRequestKey) {
        val request = controller.beginOpen(playWhenReady)
        val openResult = withContext(Dispatchers.IO) {
            runCatching {
                controller.completeOpen(
                    request = request,
                    sourceUrl = sourceUrl,
                    headerLines = sourceHeaders.toHeaderLines().toTypedArray(),
                    playWhenReady = playWhenReady,
                    initialPositionMs = initialPositionMs.coerceAtLeast(0L),
                    decoderPriority = decoderPriority,
                )
            }
        }
        val installed = openResult.getOrElse { error ->
            onError(error.message ?: "Unable to initialize the Linux player.")
            false
        }
        if (!installed) return@LaunchedEffect
        initialPositionRequestKey?.let { key ->
            onInitialPositionHandled(key, initialPositionMs > 0L)
        }
    }

    LaunchedEffect(controller, playWhenReady) {
        controller.setPlayWhenReady(playWhenReady)
    }

    LaunchedEffect(controller, resizeMode) { controller.setResizeMode(resizeMode) }

    DisposableEffect(controller) {
        onDispose { controller.dispose() }
    }

    // Poll a playback snapshot for the controls.
    LaunchedEffect(controller) {
        while (isActive) {
            controller.consumeError()?.let(onError)
            onSnapshot(controller.snapshot())
            kotlinx.coroutines.delay(500L)
        }
    }

    // Render loop: pull a frame each vsync into a reused buffer and publish it.
    LaunchedEffect(controller) {
        var buffer: ByteBuffer? = null
        var bytes: ByteArray? = null
        var bufW = 0
        var bufH = 0
        while (isActive) {
            androidx.compose.runtime.withFrameNanos { }
            val w = surfaceSize.width
            val h = surfaceSize.height
            if (w <= 0 || h <= 0 || !controller.isReady()) continue
            // Native software rendering and the full-frame copy are expensive;
            // keep both off the Compose dispatcher so controls remain responsive.
            val renderedFrame = withContext(Dispatchers.Default) {
                runCatching {
                    if (buffer == null || bufW != w || bufH != h) {
                        val pixelBytes = Math.multiplyExact(Math.multiplyExact(w, h), 4)
                        buffer = ByteBuffer.allocateDirect(pixelBytes).order(ByteOrder.nativeOrder())
                        bytes = ByteArray(pixelBytes)
                        bufW = w
                        bufH = h
                    }
                    val buf = buffer ?: return@runCatching null
                    val arr = bytes ?: return@runCatching null
                    if (!controller.renderFrame(w, h, buf)) return@runCatching null
                    buf.rewind()
                    buf.get(arr)
                    Bitmap().apply {
                        setImageInfo(ImageInfo(w, h, ColorType.RGBA_8888, ColorAlphaType.OPAQUE))
                        installPixels(arr)
                    }.asComposeImageBitmap()
                }.getOrNull()
            }
            if (renderedFrame != null) {
                frame = renderedFrame
            }
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black)
            .onSizeChanged { surfaceSize = it },
    ) {
        frame?.let { bitmap ->
            Image(
                bitmap = bitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.FillBounds,
            )
        }
    }
}

private fun Map<String, String>.toHeaderLines(): List<String> =
    entries.mapNotNull { (key, value) ->
        val cleanKey = key.trim()
        val cleanValue = value.trim()
        if (cleanKey.isBlank() || cleanValue.isBlank()) null else "$cleanKey: $cleanValue"
    }

@Serializable
private data class NativeMpvTrack(
    val index: Int = 0,
    val id: String = "",
    val label: String = "",
    val language: String = "",
    val selected: Boolean = false,
    val forced: Boolean = false,
)

private fun resolveTrackId(index: Int, tracks: List<NativeMpvTrack>): Int? =
    tracks.firstNotNullOfOrNull { track ->
        if (track.index == index) track.id.toIntOrNull() else null
    } ?: tracks.getOrNull(index)?.id?.toIntOrNull()

private fun Color.toMpvColorString(): String = buildString {
    append('#')
    append((alpha * 255f).toInt().toHexByte())
    append((red * 255f).toInt().toHexByte())
    append((green * 255f).toInt().toHexByte())
    append((blue * 255f).toInt().toHexByte())
}

private fun SubtitleStyleState.toMpvSubtitlePosition(): Int =
    (100 - (bottomOffset / 2)).coerceIn(0, 150)

private fun SubtitleStyleState.toMpvSubtitleFontSize(): Float =
    (fontSizeSp * 3f).coerceIn(18f, 96f)

private fun Int.toHexByte(): String {
    val digits = "0123456789ABCDEF"
    val value = coerceIn(0, 255)
    return buildString {
        append(digits[value / 16])
        append(digits[value % 16])
    }
}

private class LinuxComposePlayerController : PlayerEngineController {
    private val handles = LinuxPlayerHandleState()
    private val noOpSink = NativePlayerEventSink { _, _ -> }
    @Volatile
    private var desiredPlayWhenReady = true
    @Volatile
    private var desiredPlaybackSpeed = 1f
    @Volatile
    private var pendingResizeMode = PlayerResizeMode.Fit
    @Volatile
    private var pendingSubtitleDelayMs = 0
    @Volatile
    private var pendingSubtitleStyle: SubtitleStyleState? = null

    fun beginOpen(playWhenReady: Boolean): LinuxPlayerOpenRequest {
        desiredPlayWhenReady = playWhenReady
        return handles.beginOpen()
    }

    fun completeOpen(
        request: LinuxPlayerOpenRequest,
        sourceUrl: String,
        headerLines: Array<String>,
        playWhenReady: Boolean,
        initialPositionMs: Long,
        decoderPriority: Int,
    ): Boolean {
        disposeHandle(request.previousHandle)
        val created = NativePlayerBridge.create(
            hostViewPtr = 0L,
            sourceUrl = sourceUrl,
            headerLines = headerLines,
            playWhenReady = playWhenReady,
            initialPositionMs = initialPositionMs,
            controlsPageUrl = "",
            decoderPriority = decoderPriority,
            // VSR is a Windows/DirectX feature; the Linux software path ignores it.
            nvidiaRtxSuperResolutionEnabled = false,
            eventSink = noOpSink,
        )
        check(created != 0L) { "Native player did not return a handle." }
        if (!handles.install(request.generation, created)) {
            disposeHandle(created)
            return false
        }
        NativePlayerBridge.setPaused(created, !desiredPlayWhenReady)
        applyResizeMode(created, pendingResizeMode)
        NativePlayerBridge.setSpeed(created, desiredPlaybackSpeed)
        NativePlayerBridge.setSubtitleDelayMs(created, pendingSubtitleDelayMs)
        pendingSubtitleStyle?.let { applySubtitleStyle(created, it) }
        return true
    }

    fun isReady(): Boolean = handles.current() != 0L

    fun renderFrame(width: Int, height: Int, buffer: ByteBuffer): Boolean {
        val current = handles.current()
        return current != 0L && NativePlayerBridge.renderFrame(current, width, height, buffer)
    }

    fun consumeError(): String? {
        val current = handles.current()
        return if (current == 0L) null else NativePlayerBridge.consumeError(current)
    }

    fun snapshot(): PlayerPlaybackSnapshot {
        val current = handles.current()
        if (current == 0L) return PlayerPlaybackSnapshot(isLoading = true)
        return runCatching {
            val isLoading = NativePlayerBridge.isLoading(current)
            val isEnded = NativePlayerBridge.isEnded(current)
            PlayerPlaybackSnapshot(
                isLoading = isLoading,
                isPlaying = !NativePlayerBridge.isPaused(current) && !isLoading && !isEnded,
                isEnded = isEnded,
                durationMs = NativePlayerBridge.durationMs(current),
                positionMs = NativePlayerBridge.positionMs(current),
                bufferedPositionMs = NativePlayerBridge.bufferedPositionMs(current),
                playbackSpeed = NativePlayerBridge.speed(current),
            )
        }.getOrDefault(PlayerPlaybackSnapshot(isLoading = true))
    }

    fun setResizeMode(mode: PlayerResizeMode) {
        pendingResizeMode = mode
        handles.current().takeIf { it != 0L }?.let { applyResizeMode(it, mode) }
    }

    private fun applyResizeMode(handle: Long, mode: PlayerResizeMode) {
        NativePlayerBridge.setResizeMode(
            handle,
            when (mode) {
                PlayerResizeMode.Fit -> 0
                PlayerResizeMode.Fill -> 1
                PlayerResizeMode.Zoom -> 2
                PlayerResizeMode.Stretch -> 3
            },
        )
    }

    fun dispose() {
        disposeHandle(handles.clear())
    }

    private fun disposeHandle(handle: Long) {
        if (handle != 0L) runCatching { NativePlayerBridge.dispose(handle) }
    }

    fun setPlayWhenReady(playWhenReady: Boolean) {
        desiredPlayWhenReady = playWhenReady
        handles.current().takeIf { it != 0L }?.let {
            NativePlayerBridge.setPaused(it, !playWhenReady)
        }
    }

    override fun play() {
        setPlayWhenReady(true)
    }

    override fun pause() {
        setPlayWhenReady(false)
    }

    override fun seekTo(positionMs: Long) {
        handles.current().takeIf { it != 0L }?.let { NativePlayerBridge.seekTo(it, positionMs) }
    }

    override fun seekBy(offsetMs: Long) {
        handles.current().takeIf { it != 0L }?.let { NativePlayerBridge.seekBy(it, offsetMs) }
    }

    override fun retry() = Unit

    override fun toggleFullscreen() {
        toggleDesktopAppFullscreen()
    }

    override fun isHostFullscreen(): Boolean = isDesktopAppFullscreen()

    override fun setPlaybackSpeed(speed: Float) {
        desiredPlaybackSpeed = speed
        handles.current().takeIf { it != 0L }?.let { NativePlayerBridge.setSpeed(it, speed) }
    }

    private val json = Json { ignoreUnknownKeys = true }

    private fun decodeTracks(jsonText: String): List<NativeMpvTrack> =
        runCatching { json.decodeFromString<List<NativeMpvTrack>>(jsonText) }.getOrDefault(emptyList())

    override fun getAudioTracks(): List<AudioTrack> {
        val current = handles.current().takeIf { it != 0L } ?: return emptyList()
        return decodeTracks(NativePlayerBridge.audioTracksJson(current)).map { track ->
            AudioTrack(
                index = track.index,
                id = track.id,
                label = track.label,
                language = track.language.takeUnless(String::isBlank),
                isSelected = track.selected,
            )
        }
    }

    override fun getSubtitleTracks(): List<SubtitleTrack> {
        val current = handles.current().takeIf { it != 0L } ?: return emptyList()
        return decodeTracks(NativePlayerBridge.subtitleTracksJson(current)).map { track ->
            SubtitleTrack(
                index = track.index,
                id = track.id,
                label = track.label,
                language = track.language.takeUnless(String::isBlank),
                isSelected = track.selected,
                isForced = track.forced ||
                    inferForcedSubtitleTrack(track.label, track.language, track.id),
            )
        }
    }

    override fun selectAudioTrack(index: Int) {
        val current = handles.current().takeIf { it != 0L } ?: return
        val trackId = resolveTrackId(index, decodeTracks(NativePlayerBridge.audioTracksJson(current))) ?: return
        NativePlayerBridge.selectAudioTrack(current, trackId)
    }

    override fun selectSubtitleTrack(index: Int) {
        val current = handles.current().takeIf { it != 0L } ?: return
        if (index < 0) {
            NativePlayerBridge.selectSubtitleTrack(current, -1)
            return
        }
        val trackId = resolveTrackId(index, decodeTracks(NativePlayerBridge.subtitleTracksJson(current))) ?: return
        NativePlayerBridge.selectSubtitleTrack(current, trackId)
    }

    override fun setSubtitleUri(url: String) {
        handles.current().takeIf { it != 0L }?.let { NativePlayerBridge.addSubtitleUrl(it, url) }
    }

    override fun clearExternalSubtitle() {
        handles.current().takeIf { it != 0L }?.let(NativePlayerBridge::clearExternalSubtitles)
    }

    override fun clearExternalSubtitleAndSelect(trackIndex: Int) {
        val current = handles.current().takeIf { it != 0L } ?: return
        val trackId = if (trackIndex < 0) {
            -1
        } else {
            resolveTrackId(trackIndex, decodeTracks(NativePlayerBridge.subtitleTracksJson(current))) ?: return
        }
        NativePlayerBridge.clearExternalSubtitlesAndSelect(current, trackId)
    }

    override fun setSubtitleDelayMs(delayMs: Int) {
        pendingSubtitleDelayMs = delayMs
        handles.current().takeIf { it != 0L }?.let { NativePlayerBridge.setSubtitleDelayMs(it, delayMs) }
    }

    override fun applySubtitleStyle(style: SubtitleStyleState) {
        pendingSubtitleStyle = style
        val current = handles.current().takeIf { it != 0L } ?: return
        applySubtitleStyle(current, style)
    }

    private fun applySubtitleStyle(current: Long, style: SubtitleStyleState) {
        NativePlayerBridge.applySubtitleStyle(
            handle = current,
            textColor = style.textColor.toMpvColorString(),
            backgroundColor = style.backgroundColor.toMpvColorString(),
            outlineColor = style.outlineColor.toMpvColorString(),
            outlineSize = if (style.outlineEnabled) style.outlineWidth.toFloat() else 0f,
            bold = style.bold,
            fontSize = style.toMpvSubtitleFontSize(),
            subPos = style.toMpvSubtitlePosition(),
        )
    }
}

internal data class LinuxPlayerOpenRequest(
    val generation: Long,
    val previousHandle: Long,
)

internal class LinuxPlayerHandleState {
    private var generation = 0L

    @Volatile
    private var handle = 0L

    @Synchronized
    fun beginOpen(): LinuxPlayerOpenRequest {
        generation += 1
        val previous = handle
        handle = 0L
        return LinuxPlayerOpenRequest(generation, previous)
    }

    @Synchronized
    fun install(requestGeneration: Long, createdHandle: Long): Boolean {
        check(createdHandle != 0L)
        if (requestGeneration != generation) return false
        handle = createdHandle
        return true
    }

    @Synchronized
    fun clear(): Long {
        generation += 1
        val previous = handle
        handle = 0L
        return previous
    }

    fun current(): Long = handle
}
