package uk.callum.tlkrecomp

import org.json.JSONObject
import java.io.File

/**
 * A savestate the game wrote into the files folder (tlk_autosave.state or tlk_slotN.state), with what its
 * sidecar says. `usable` is false for a state from another build, which the game refuses.
 */
data class Slot(
    val base: String,
    val state: File,
    val picture: File?,
    val levelName: String?,
    val savedUnix: Long?,
    val lives: Int?,
    val build: String?,
    val usable: Boolean,
) {
    val title get() = Slots.title(base)
}

object Slots {
    const val AUTOSAVE = "tlk_autosave"
    val bases = listOf(AUTOSAVE) + (1..4).map { "tlk_slot$it" }
    private val BUILD_KEY = Regex(""""build"\s*:\s*"([^"]*)"""")

    fun title(base: String) = if (base == AUTOSAVE) "Autosave" else "Slot ${base.removePrefix("tlk_slot")}"

    /** The state named `base` in `dir`, or null when there is none. A json without its state means nothing. */
    fun read(dir: File, base: String, build: String): Slot? {
        val state = File(dir, "$base.state")
        if (!state.isFile) return null
        val text = File(dir, "$base.json").takeIf { it.isFile }?.readText()
        val json = try {
            text?.let { JSONObject(it) }
        } catch (_: Exception) {
            null
        }
        // The game reads the build key from a json that doesn't parse, and refuses the state when it's another's.
        val theirs = json?.optString("build")?.takeIf { it.isNotEmpty() }
            ?: text?.let { BUILD_KEY.find(it)?.groupValues?.get(1) }
        return Slot(
            base, state, File(dir, "$base.png").takeIf { it.isFile },
            json?.optString("level_name")?.takeIf { it.isNotEmpty() },
            json?.takeIf { it.has("saved_unix") }?.getLong("saved_unix"),
            json?.takeIf { it.has("lives") }?.getInt("lives"),
            theirs, theirs == null || theirs == build,
        )
    }

    /** The autosave, then slots 1 to 4, each that has a state. */
    fun list(dir: File, build: String): List<Slot> = bases.mapNotNull { read(dir, it, build) }

    /** "Pride Rock, 12 minutes ago"; for another build's state, that it can't load. */
    fun describe(slot: Slot, nowUnix: Long): String {
        if (!slot.usable) return "Saved by another build of the game, so this one can't load it"
        val level = slot.levelName ?: "Saved"
        return slot.savedUnix?.let { "$level, ${ago(it, nowUnix)}" } ?: level
    }

    fun ago(savedUnix: Long, nowUnix: Long): String {
        val s = (nowUnix - savedUnix).coerceAtLeast(0)
        fun n(v: Long, unit: String) = if (v == 1L) "1 $unit ago" else "$v ${unit}s ago"
        return when {
            s < 60 -> "just now"
            s < 3600 -> n(s / 60, "minute")
            s < 86400 -> n(s / 3600, "hour")
            else -> n(s / 86400, "day")
        }
    }

    /** Copies a state and its sidecar to `base`, the json last, so a json always has its state beside it. */
    fun copy(slot: Slot, base: String) {
        require(base != slot.base) { "a slot can't be copied onto itself" }
        val dir = slot.state.parentFile
        File(dir, "$base.json").delete()
        slot.state.copyTo(File(dir, "$base.state"), overwrite = true)
        val png = File(dir, "$base.png")
        slot.picture?.copyTo(png, overwrite = true) ?: png.delete()
        File(dir, "${slot.base}.json").takeIf { it.isFile }?.copyTo(File(dir, "$base.json"), overwrite = true)
    }

    fun delete(slot: Slot) {
        val dir = slot.state.parentFile
        File(dir, "${slot.base}.json").delete()
        slot.state.delete()
        File(dir, "${slot.base}.png").delete()
    }
}
