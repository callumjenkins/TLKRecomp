package uk.callum.tlkrecomp

import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** Where synced saves are kept: one zip per save name, as [SaveZip] makes. [HttpSaveStore] is the save server. */
interface SaveStore {
    /** Every save's sidecar, by name. */
    fun list(): Map<String, JSONObject>

    /** The zip, or [NoSuchElementException]. */
    fun get(name: String): ByteArray

    /** Stores the zip, or [SaveConflict] when the stored copy is as new or newer. */
    fun put(name: String, zip: ByteArray)
}

class SaveConflict(message: String) : Exception(message)

/** The store couldn't be reached at all, as when the phone is off the tailnet. */
class StoreUnreachable(message: String, cause: Throwable? = null) : IOException(message, cause)

/** sync/tlk_save_server.py: GET /v1/saves, and GET and PUT /v1/saves/NAME. */
class HttpSaveStore(url: String, private val connectMs: Int = 3000, private val readMs: Int = 15000) : SaveStore {
    private val base = url.trimEnd('/')

    val host: String get() = URL(base).host.let { h -> if (h.all { it.isDigit() || it == '.' }) h else h.substringBefore('.') }

    private fun call(method: String, path: String, body: ByteArray? = null): ByteArray {
        val c = try {
            (URL(base + path).openConnection() as HttpURLConnection).apply {
                requestMethod = method
                connectTimeout = connectMs
                readTimeout = readMs
                if (body != null) {
                    doOutput = true
                    setFixedLengthStreamingMode(body.size)
                    setRequestProperty("Content-Type", "application/zip")
                    outputStream.use { it.write(body) }
                }
            }
        } catch (e: IOException) {
            throw StoreUnreachable("$base: ${e.message}", e)
        }
        try {
            val code = try {
                c.responseCode
            } catch (e: IOException) {
                throw StoreUnreachable("$base: ${e.message}", e)
            }
            if (code in 200..299) return c.inputStream.use { it.readBytes() }
            val why = c.errorStream?.use { it.readBytes().decodeToString().trim() } ?: ""
            throw when (code) {
                404 -> NoSuchElementException(path)
                409 -> SaveConflict(why)
                else -> IOException("$method $path: $code $why")
            }
        } finally {
            c.disconnect()
        }
    }

    override fun list(): Map<String, JSONObject> {
        val saves = JSONObject(call("GET", "/v1/saves").decodeToString()).getJSONObject("saves")
        return saves.keys().asSequence().associateWith { saves.getJSONObject(it) }
    }

    override fun get(name: String) = call("GET", "/v1/saves/$name")

    override fun put(name: String, zip: ByteArray) {
        call("PUT", "/v1/saves/$name", zip)
    }
}

/** A save as one zip: save.state, save.png when there is one, and save.json, the sidecar. */
object SaveZip {
    class Save(val state: ByteArray, val info: JSONObject, val png: ByteArray?)

    fun sha256(data: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }

    fun pack(state: ByteArray, info: JSONObject, png: ByteArray?): ByteArray {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { z ->
            fun entry(name: String, data: ByteArray) {
                z.putNextEntry(ZipEntry(name))
                z.write(data)
                z.closeEntry()
            }
            entry("save.state", state)
            png?.let { entry("save.png", it) }
            entry("save.json", info.toString().toByteArray())
        }
        return out.toByteArray()
    }

    /** The zip's parts, or [IOException] when one is missing or the state doesn't match the sidecar's state_sha256. */
    fun unpack(zip: ByteArray): Save {
        val parts = mutableMapOf<String, ByteArray>()
        ZipInputStream(ByteArrayInputStream(zip)).use { z ->
            while (true) {
                val e = z.nextEntry ?: break
                if (e.name in setOf("save.state", "save.png", "save.json")) parts[e.name] = z.readBytes()
            }
        }
        val state = parts["save.state"] ?: throw IOException("no save.state in the zip")
        val info = parts["save.json"]?.let { JSONObject(it.decodeToString()) } ?: throw IOException("no save.json in the zip")
        if (info.optString("state_sha256") != sha256(state)) throw IOException("save.state doesn't match its sidecar")
        return Save(state, info, parts["save.png"])
    }
}

/** The merge rule, the same as sync/tlk_saves.py's; sync/merge_cases.json holds the cases both run. */
object SyncRule {
    enum class Action { PULL, PUSH, KEEP }

    /** `local` carries `sha_ok`: whether the state file still hashes to its `state_sha256`. */
    fun decide(local: JSONObject?, remote: JSONObject?): Action {
        val usable = local != null && local.optString("state_sha256").isNotEmpty() && local.optBoolean("sha_ok")
        if (remote == null) return if (usable) Action.PUSH else Action.KEEP
        if (!usable) return Action.PULL
        if (local!!.optString("state_sha256") == remote.optString("state_sha256")) return Action.KEEP
        val l = local.optLong("saved_unix")
        val r = remote.optLong("saved_unix")
        return when {
            r > l -> Action.PULL
            l > r -> Action.PUSH
            else -> Action.KEEP
        }
    }

    /** The newest candidate with this build and a state hash, a tie going to "local". Each has `where`: "local" or a save name. */
    fun pickContinue(candidates: List<JSONObject>, build: String): JSONObject? =
        candidates.filter { it.optString("build") == build && it.optString("state_sha256").isNotEmpty() }
            .maxWithOrNull(compareBy<JSONObject>({ it.optLong("saved_unix") }, { it.optString("where") == "local" }))
}

/**
 * Syncs the files folder with a store: slots 1 to 4 by name, this device's autosave as tlk_autosave@DEVICE, and the
 * newest loadable autosave from any device into tlk_autosave. Pushes run while the game is up too, since the store
 * refuses a state that doesn't match its sidecar, but pulls are skipped while the game process is up, as it reads and
 * writes those files.
 */
class SaveSync(
    private val dir: File,
    private val store: SaveStore,
    private val device: String,
    private val build: String,
    private val gameRunning: () -> Boolean,
) {
    /** `kept` names the saves the store refused, which stay as they are here. */
    class Report(val pulled: List<String>, val pushed: List<String>, val kept: List<String>, val problem: String?)

    @Volatile private var stopped = false

    /** Pushes what is newer here, then, with the game process gone, pulls what is newer there. */
    fun run(offline: String): Report {
        val pulled = mutableListOf<String>()
        val pushed = mutableListOf<String>()
        val kept = mutableListOf<String>()
        return try {
            var remote = store.list()
            for ((base, name) in Slots.bases.drop(1).map { it to it } + (Slots.AUTOSAVE to autosaveName(device))) {
                if (stopped) break
                if (SyncRule.decide(localInfo(base), remote[name]) != SyncRule.Action.PUSH) continue
                try {
                    store.put(name, packLocal(base))
                    pushed += name
                } catch (_: SaveConflict) {
                    kept += name
                } catch (e: IOException) {
                    if (e is StoreUnreachable) throw e
                    kept += name   // as when the game rewrote the state while it was read; it goes next time
                }
            }
            if (stopped || gameRunning()) return Report(pulled, pushed, kept, null)
            if (pushed.isNotEmpty()) remote = store.list()
            for (slot in Slots.bases.drop(1)) {
                if (SyncRule.decide(localInfo(slot), remote[slot]) == SyncRule.Action.PULL && fetch(slot, slot)) pulled += slot
            }
            val local = localInfo(Slots.AUTOSAVE)
            val candidates = remote.filterKeys { it.startsWith(Slots.AUTOSAVE + "@") }
                .map { (name, info) -> JSONObject(info.toString()).put("where", name) }
                .plus(listOfNotNull(local?.takeIf { it.optBoolean("sha_ok") }?.let { JSONObject(it.toString()).put("where", "local") }))
            val best = SyncRule.pickContinue(candidates, build)
            val where = best?.optString("where")
            if (best != null && where != "local" && best.optString("state_sha256") != local?.optString("state_sha256")) {
                if (fetch(where!!, Slots.AUTOSAVE)) pulled += where
            }
            Report(pulled, pushed, kept, null)
        } catch (e: StoreUnreachable) {
            Report(pulled, pushed, kept, offline)
        } catch (e: Exception) {
            Report(pulled, pushed, kept, "Not synced: ${e.message}")
        }
    }

    /** The sidecar of `base` with `sha_ok`, or null when there's no state. */
    fun localInfo(base: String): JSONObject? {
        val state = File(dir, "$base.state").takeIf { it.isFile } ?: return null
        val info = try {
            JSONObject(File(dir, "$base.json").readText())
        } catch (_: Exception) {
            JSONObject()
        }
        return info.put("sha_ok", info.optString("state_sha256") == SaveZip.sha256(state.readBytes()))
    }

    private fun packLocal(base: String): ByteArray {
        val png = File(dir, "$base.png").takeIf { it.isFile }?.readBytes()
        return SaveZip.pack(File(dir, "$base.state").readBytes(), JSONObject(File(dir, "$base.json").readText()), png)
    }

    /** Downloads `name` into `base`, keeping what it replaces as base.prev.*; the json goes last. */
    private fun fetch(name: String, base: String): Boolean {
        val save = try {
            SaveZip.unpack(store.get(name))
        } catch (_: NoSuchElementException) {
            return false
        } catch (e: IOException) {
            if (e is StoreUnreachable) throw e
            return false
        }
        synchronized(this) {
            if (stopped) return false
            for (part in listOf("json", "state", "png")) {
                val cur = File(dir, "$base.$part")
                val prev = File(dir, "$base.prev.$part")
                prev.delete()
                if (cur.isFile) cur.renameTo(prev)
            }
            write(File(dir, "$base.state"), save.state)
            save.png?.let { write(File(dir, "$base.png"), it) }
            write(File(dir, "$base.json"), save.info.toString().toByteArray())
        }
        return true
    }

    /** Blocks until a fetch in progress has finished, then stops any more. */
    fun stop() {
        stopped = true
        synchronized(this) {}
    }

    private fun write(f: File, data: ByteArray) {
        val tmp = File(f.parentFile, "${f.name}.sync")
        tmp.writeBytes(data)
        if (!tmp.renameTo(f)) throw IOException("couldn't write ${f.name}")
    }

    companion object {
        fun autosaveName(device: String) = "${Slots.AUTOSAVE}@$device"

        /** A device name the store accepts: letters, digits, '.', '_', and '-', at most 63 of them. */
        fun deviceName(raw: String): String =
            raw.trim().replace(Regex("[^A-Za-z0-9._-]+"), "-").trim('-').take(63).ifEmpty { "device" }
    }
}
