package uk.callum.tlkrecomp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class SaveArchiveTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun save(dir: File, base: String, tag: String, png: Boolean = true) {
        File(dir, "$base.state").writeText("state $tag")
        if (png) File(dir, "$base.png").writeText("png $tag")
        File(dir, "$base.json").writeText("""{"build": "b", "level_name": "$tag"}""")
    }

    private fun zip(vararg entries: Pair<String, String>): ByteArray {
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { z ->
            for ((name, text) in entries) {
                z.putNextEntry(ZipEntry(name))
                z.write(text.toByteArray())
                z.closeEntry()
            }
        }
        return bytes.toByteArray()
    }

    @Test
    fun exportThenImportRestoresEverySaveAndTheIni() {
        val from = tmp.newFolder()
        save(from, Slots.AUTOSAVE, "Hakuna")
        save(from, "tlk_slot2", "Pride Rock")
        File(from, SaveArchive.INI).writeText("volume = 50\n")
        File(from, GameActivity.ROM).writeText("rom")
        val bytes = ByteArrayOutputStream()
        assertEquals(SaveArchive.Count(2, settings = true), SaveArchive.write(from, bytes))

        val to = tmp.newFolder()
        assertEquals(SaveArchive.Count(2, settings = true), SaveArchive.read(ByteArrayInputStream(bytes.toByteArray()), to))
        assertEquals("state Hakuna", File(to, "${Slots.AUTOSAVE}.state").readText())
        assertEquals("png Pride Rock", File(to, "tlk_slot2.png").readText())
        assertEquals("volume = 50\n", File(to, SaveArchive.INI).readText())
        assertFalse("the ROM stays out of the archive", File(to, GameActivity.ROM).exists())
        assertEquals(emptyList<String>(), to.list()!!.filter { it.endsWith(".import") })
    }

    @Test
    fun exportWithoutAnIniSaysItHasNoSettings() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", "one")
        assertEquals(SaveArchive.Count(1, settings = false), SaveArchive.write(dir, ByteArrayOutputStream()))
    }

    @Test
    fun everyNameComesBackByteForByte() {
        val from = tmp.newFolder()
        val want = SaveArchive.names.associateWith { name -> ByteArray(300) { (it * 7 + name.length).toByte() } }
        for ((name, bytes) in want) File(from, name).writeBytes(bytes)
        val zip = ByteArrayOutputStream()
        assertEquals(SaveArchive.Count(5, settings = true), SaveArchive.write(from, zip))
        val to = tmp.newFolder()
        SaveArchive.read(ByteArrayInputStream(zip.toByteArray()), to)
        assertEquals(want.keys, to.list()!!.toSet())
        for ((name, bytes) in want) assertArrayEquals(name, bytes, File(to, name).readBytes())
    }

    @Test
    fun anIniOnlyArchiveImportsTheSettingsAndNoSaves() {
        val dir = tmp.newFolder()
        val c = SaveArchive.read(ByteArrayInputStream(zip(SaveArchive.INI to "volume = 20\n")), dir)
        assertEquals(SaveArchive.Count(0, settings = true), c)
        assertEquals("volume = 20\n", File(dir, SaveArchive.INI).readText())
    }

    @Test
    fun aStateImportedWithoutItsJsonLeavesNoOldJson() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot2", "old")
        SaveArchive.read(ByteArrayInputStream(zip("tlk_slot2.state" to "state new")), dir)
        assertEquals("state new", File(dir, "tlk_slot2.state").readText())
        assertFalse("the old json would vouch for the new state", File(dir, "tlk_slot2.json").exists())
    }

    @Test
    fun anArchiveCutShortChangesNothingAndLeavesNoImportFiles() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", "mine")
        val noise = java.util.Random(1).let { r -> List(2) { ByteArray(5000).also(r::nextBytes).toString(Charsets.ISO_8859_1) } }
        val whole = zip("tlk_slot1.png" to noise[0], "tlk_slot1.state" to noise[1])   // too random to compress
        assertThrows(Exception::class.java) {
            SaveArchive.read(ByteArrayInputStream(whole.copyOf(whole.size / 3)), dir)
        }
        assertEquals("state mine", File(dir, "tlk_slot1.state").readText())
        assertEquals("png mine", File(dir, "tlk_slot1.png").readText())
        assertEquals(emptyList<String>(), dir.list()!!.filter { it.endsWith(".import") })
    }

    @Test
    fun importReplacesOnlyTheSlotsTheArchiveHas() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot1", "kept")
        save(dir, "tlk_slot3", "old")
        SaveArchive.read(ByteArrayInputStream(zip("tlk_slot3.state" to "state new", "tlk_slot3.json" to "{}")), dir)
        assertEquals("state kept", File(dir, "tlk_slot1.state").readText())
        assertEquals("state new", File(dir, "tlk_slot3.state").readText())
        assertFalse("the old picture goes with the old state", File(dir, "tlk_slot3.png").exists())
    }

    @Test
    fun importSkipsNamesOutsideTheSavesAndAJsonWithoutItsState() {
        val dir = tmp.newFolder()
        save(dir, "tlk_slot4", "mine")
        val before = dir.parentFile.list()!!.toSet()
        val c = SaveArchive.read(ByteArrayInputStream(zip(
            "../escaped" to "x", "lib/libmain.so" to "x", "tlk_slot9.state" to "x", "tlk_slot4.json" to "{}")), dir)
        assertEquals(SaveArchive.Count(0, settings = false), c)
        assertEquals("nothing written beside the folder", before, dir.parentFile.list()!!.toSet())
        assertEquals(setOf("tlk_slot4.state", "tlk_slot4.png", "tlk_slot4.json"), dir.list()!!.toSet())
        assertEquals("""{"build": "b", "level_name": "mine"}""", File(dir, "tlk_slot4.json").readText())
        assertTrue(File(dir, "tlk_slot4.state").isFile)
    }
}
