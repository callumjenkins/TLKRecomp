package uk.callum.tlkrecomp

import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** The saves and settings as one zip, the only copy of them that survives an uninstall. */
object SaveArchive {
    const val INI = "tlk_sdl.ini"
    private val parts = listOf("state", "png", "json")

    /** Every name an archive may hold. Anything else in one is skipped, so an entry can't name a path. */
    val names: Set<String> = (Slots.bases.flatMap { base -> parts.map { "$base.$it" } } + INI).toSet()

    /** How many states an archive holds, and whether it holds the ini. */
    data class Count(val states: Int, val settings: Boolean)

    fun write(dir: File, out: OutputStream): Count {
        var states = 0
        ZipOutputStream(out).use { zip ->
            for (name in names.sorted()) {
                val f = File(dir, name).takeIf { it.isFile } ?: continue
                zip.putNextEntry(ZipEntry(name))
                f.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                if (name.endsWith(".state")) states++
            }
        }
        return Count(states, File(dir, INI).isFile)
    }

    /**
     * Puts an archive's saves into `dir`, each replacing the slot of the same name, and its ini over
     * the current one. A slot the archive has no state for is left alone. An archive that can't be read
     * to its end changes nothing.
     */
    fun read(input: InputStream, dir: File): Count {
        val got = mutableSetOf<String>()
        try {
            ZipInputStream(input).use { zip ->
                while (true) {
                    val e = zip.nextEntry ?: break
                    if (e.isDirectory || e.name !in names) continue
                    File(dir, "${e.name}.import").outputStream().use { zip.copyTo(it) }
                    got += e.name
                }
            }
            return put(dir, got)
        } finally {
            dir.listFiles { f -> f.name.endsWith(".import") }?.forEach { it.delete() }
        }
    }

    private fun put(dir: File, got: Set<String>): Count {
        fun take(name: String) = File(dir, "$name.import").renameTo(File(dir, name))
        var states = 0
        for (base in Slots.bases) {
            if ("$base.state" !in got) continue
            // As the game writes a state: its json goes first and comes back last, so a json always has its state.
            File(dir, "$base.json").delete()
            take("$base.state")
            if ("$base.png" in got) take("$base.png") else File(dir, "$base.png").delete()
            if ("$base.json" in got) take("$base.json")
            states++
        }
        if (INI in got) take(INI)
        return Count(states, INI in got)
    }
}
