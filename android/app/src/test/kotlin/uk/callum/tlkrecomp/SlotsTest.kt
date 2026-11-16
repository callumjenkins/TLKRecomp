package uk.callum.tlkrecomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class SlotsTest {
    @get:Rule val tmp = TemporaryFolder()
    private val build = "0123456789ab-Android-aarch64"

    private fun save(dir: File, base: String, build: String? = this.build, level: String = "Pride Rock", at: Long = 1000) {
        File(dir, "$base.state").writeText("state $base")
        File(dir, "$base.png").writeText("png")
        if (build != null)
            File(dir, "$base.json").writeText("""{"build": "$build", "frame": 6000, "saved_unix": $at, "level": 9, "level_name": "$level", "lives": 3}""")
    }

    @Test
    fun slotsListOrdersAutosaveFirstAndMarksOtherBuilds() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot3", build = "older")
        save(dir, "tlk_slot1", build = null)
        save(dir, Slots.AUTOSAVE, level = "Hakuna")
        File(dir, "tlk_slot2.json").writeText("""{"build": "$build"}""")   // a json without its state
        val slots = Slots.list(dir, build)
        assertEquals(listOf("Autosave", "Slot 1", "Slot 3"), slots.map { it.title })
        assertEquals(listOf(true, true, false), slots.map { it.usable })
        assertEquals("Hakuna", slots[0].levelName)
        assertEquals(3, slots[0].lives)
        assertNull("no sidecar: shown, and loads", slots[1].levelName)
        assertEquals("older", slots[2].build)
    }

    @Test
    fun slotDescriptionSaysWhereAndHowLongAgo() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", at = 1000)
        val slot = Slots.read(dir, "tlk_slot1", build)!!
        assertEquals("Pride Rock, 12 minutes ago", Slots.describe(slot, 1000 + 12 * 60 + 5))
        assertEquals("Pride Rock, just now", Slots.describe(slot, 1000 + 59))
        assertEquals("Pride Rock, 1 hour ago", Slots.describe(slot, 1000 + 3600))
        assertEquals("Pride Rock, 2 days ago", Slots.describe(slot, 1000 + 2 * 86400))
        assertEquals("Pride Rock, 1 minute ago", Slots.describe(slot, 1000 + 60))
        assertEquals("Pride Rock, 23 hours ago", Slots.describe(slot, 1000 + 86399))
        assertEquals("Pride Rock, 1 day ago", Slots.describe(slot, 1000 + 86400))
        assertEquals("a clock set back", "Pride Rock, just now", Slots.describe(slot, 500))
        assertTrue(Slots.describe(Slots.read(dir, "tlk_slot1", "other")!!, 2000).contains("another build"))
        save(dir, "tlk_slot2", build = null)
        assertEquals("Saved", Slots.describe(Slots.read(dir, "tlk_slot2", build)!!, 2000))
    }

    @Test
    fun copyToASlotBringsTheSidecarAndDeleteRemovesAllThree() {
        val dir = tmp.newFolder()
        save(dir, Slots.AUTOSAVE, level = "Exile")
        save(dir, "tlk_slot4", level = "Stampede")
        Slots.copy(Slots.read(dir, Slots.AUTOSAVE, build)!!, "tlk_slot4")
        val copied = Slots.read(dir, "tlk_slot4", build)!!
        assertEquals("Exile", copied.levelName)
        assertEquals("state ${Slots.AUTOSAVE}", copied.state.readText())
        Slots.delete(copied)
        assertNull(Slots.read(dir, "tlk_slot4", build))
        assertFalse(File(dir, "tlk_slot4.json").exists() || File(dir, "tlk_slot4.png").exists())
        assertTrue(File(dir, "${Slots.AUTOSAVE}.state").exists())
    }

    @Test
    fun copyOntoItselfIsRefusedAndKeepsTheState() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1")
        val slot = Slots.read(dir, "tlk_slot1", build)!!
        assertThrows(IllegalArgumentException::class.java) { Slots.copy(slot, "tlk_slot1") }
        assertEquals("Pride Rock", Slots.read(dir, "tlk_slot1", build)?.levelName)
    }

    @Test
    fun sidecarCutShortStillNamesItsBuildLikeTheGameReadsIt() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", build = null)
        File(dir, "tlk_slot1.json").writeText("""{"build": "another", "frame": 12""")
        assertFalse(Slots.read(dir, "tlk_slot1", build)!!.usable)
        File(dir, "tlk_slot1.json").writeText("""{"build": "$build", "fra""")
        assertTrue(Slots.read(dir, "tlk_slot1", build)!!.usable)
        File(dir, "tlk_slot1.json").writeText("""{"frame": 12""")
        assertTrue("no build key: as no sidecar", Slots.read(dir, "tlk_slot1", build)!!.usable)
    }

    @Test
    fun copyOfAStateWithoutSidecarLeavesNoStaleSidecarBehind() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", build = null)
        File(dir, "tlk_slot1.png").delete()
        save(dir, "tlk_slot2", build = "older")
        Slots.copy(Slots.read(dir, "tlk_slot1", build)!!, "tlk_slot2")
        val copied = Slots.read(dir, "tlk_slot2", build)!!
        assertTrue(copied.usable)
        assertNull(copied.build)
        assertNull(copied.picture)
    }
}
