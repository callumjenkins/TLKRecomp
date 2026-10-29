package uk.callum.tlkrecomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class IniTest {
    private val ini = """
        ; tlk_sdl settings (rewritten by the in-game settings menu)
        scale = 3
        aspect = 8:7
        ; touchscreen pad
        touch = auto
        Volume=80
    """.trimIndent() + "\n"

    @Test
    fun iniEditKeepsOtherKeysAndCommentsInOrder() {
        val out = Ini.set(ini, "aspect", "4:3")
        assertEquals(ini.replace("aspect = 8:7", "aspect = 4:3"), out)
        assertEquals(ini.replace("Volume=80", "Volume = 60"), Ini.set(ini, "volume", "60"))
    }

    @Test
    fun iniEditAddsAMissingKeyAtTheEnd() {
        assertEquals(ini + "autosave_seconds = 30\n", Ini.set(ini, "autosave_seconds", "30"))
        assertEquals("a = 1\nb = 2\n", Ini.set("a = 1", "b", "2"))
        assertEquals("b = 2\n", Ini.set("", "b", "2"))
    }

    @Test
    fun iniReadsTheLastLineForAKeyAsTheGameDoes() {
        val twice = "touch = on\n; touch = off\ntouch = off\n"
        assertEquals("off", Ini.get(twice, "touch"))
        assertEquals("touch = on\n; touch = off\ntouch = auto\n", Ini.set(twice, "touch", "auto"))
        assertEquals("80", Ini.get(ini, "volume"))
        assertNull(Ini.get(ini, "widescreen"))
    }
}
