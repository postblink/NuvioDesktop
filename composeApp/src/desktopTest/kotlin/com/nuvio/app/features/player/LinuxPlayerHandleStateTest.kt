package com.nuvio.app.features.player

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class LinuxPlayerHandleStateTest {
    @Test
    fun `superseded create cannot replace newer player`() {
        val state = LinuxPlayerHandleState()
        val first = state.beginOpen()
        val second = state.beginOpen()

        assertTrue(state.install(second.generation, 22L))
        assertFalse(state.install(first.generation, 11L))
        assertEquals(22L, state.current())
    }

    @Test
    fun `begin open detaches current handle for disposal`() {
        val state = LinuxPlayerHandleState()
        val first = state.beginOpen()
        assertTrue(state.install(first.generation, 11L))

        val second = state.beginOpen()

        assertEquals(11L, second.previousHandle)
        assertEquals(0L, state.current())
    }

    @Test
    fun `clear invalidates in flight create`() {
        val state = LinuxPlayerHandleState()
        val request = state.beginOpen()

        assertEquals(0L, state.clear())
        assertFalse(state.install(request.generation, 11L))
        assertEquals(0L, state.current())
    }
}
