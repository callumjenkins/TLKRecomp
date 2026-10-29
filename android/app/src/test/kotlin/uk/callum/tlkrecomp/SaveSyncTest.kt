package uk.callum.tlkrecomp

import com.sun.net.httpserver.HttpServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.net.InetSocketAddress

class SaveSyncTest {
    @get:Rule val tmp = TemporaryFolder()

    private val build = "b1"

    /** Keeps zips by name and refuses a put that isn't newer. */
    private class MemoryStore : SaveStore {
        val zips = mutableMapOf<String, ByteArray>()
        var reachable = true
        override fun list(): Map<String, JSONObject> {
            if (!reachable) throw StoreUnreachable("off")
            return zips.mapValues { SaveZip.unpack(it.value).info }
        }
        override fun get(name: String) = zips[name] ?: throw NoSuchElementException(name)
        override fun put(name: String, zip: ByteArray) {
            val new = SaveZip.unpack(zip).info
            zips[name]?.let { if (SaveZip.unpack(it).info.getLong("saved_unix") >= new.getLong("saved_unix")) throw SaveConflict(name) }
            zips[name] = zip
        }
    }

    private fun zip(at: Long, state: String, device: String = "cj-cos-laptop", build: String = this.build): ByteArray {
        val bytes = state.toByteArray()
        val info = JSONObject().put("build", build).put("saved_unix", at).put("level_name", "Exile").put("device", device)
            .put("state_sha256", SaveZip.sha256(bytes))
        return SaveZip.pack(bytes, info, "png".toByteArray())
    }

    private fun local(dir: File, base: String, at: Long, state: String) {
        val s = SaveZip.unpack(zip(at, state, device = "fold"))
        File(dir, "$base.state").writeBytes(s.state)
        File(dir, "$base.png").writeBytes(s.png!!)
        File(dir, "$base.json").writeText(s.info.toString())
    }

    private fun sync(dir: File, store: SaveStore, running: Boolean = false) =
        SaveSync(dir, store, "fold", build) { running }.run("offline")

    private fun cases(kind: String): List<JSONObject> {
        val file = generateSequence(File("").absoluteFile) { it.parentFile }.map { File(it, "sync/merge_cases.json") }.first { it.isFile }
        val list = JSONObject(file.readText()).getJSONArray(kind)
        return (0 until list.length()).map { list.getJSONObject(it) }
    }

    @Test
    fun sharedMergeCasesDecide() {
        for (c in cases("decide")) {
            val want = SyncRule.Action.valueOf(c.getString("want").uppercase())
            assertEquals(c.getString("id"), want, SyncRule.decide(c.optJSONObject("local"), c.optJSONObject("remote")))
        }
    }

    @Test
    fun sharedMergeCasesContinue() {
        for (c in cases("continue")) {
            val all = c.getJSONArray("candidates").let { a: JSONArray -> (0 until a.length()).map { a.getJSONObject(it) } }
            val got = SyncRule.pickContinue(all, c.getString("build"))?.getString("where")
            assertEquals(c.getString("id"), if (c.isNull("want")) null else c.getString("want"), got)
        }
    }

    @Test
    fun pullIsSkippedWhileTheGameRunsButPushIsNot() {
        val dir = tmp.newFolder()
        val store = MemoryStore().apply { zips["tlk_slot1"] = zip(300, "remote") }
        local(dir, "tlk_slot2", 100, "mine")
        val r = sync(dir, store, running = true)
        assertEquals(listOf("tlk_slot2"), r.pushed)
        assertTrue(r.pulled.isEmpty() && !File(dir, "tlk_slot1.state").exists())
        assertEquals(listOf("tlk_slot1"), sync(dir, store).pulled)
    }

    @Test
    fun pullKeepsTheReplacedSlotAsPrev() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot2", 100, "old bytes")
        val store = MemoryStore().apply { zips["tlk_slot2"] = zip(300, "new bytes") }
        sync(dir, store)
        assertEquals("new bytes", File(dir, "tlk_slot2.state").readText())
        assertEquals("old bytes", File(dir, "tlk_slot2.prev.state").readText())
        assertTrue(SaveSync(dir, store, "fold", build) { false }.localInfo("tlk_slot2")!!.getBoolean("sha_ok"))
    }

    @Test
    fun continueOffersNewestAutosaveAcrossDevices() {
        val dir = tmp.newFolder()
        local(dir, Slots.AUTOSAVE, 100, "phone")
        val store = MemoryStore().apply {
            zips["tlk_autosave@cj-cos-laptop"] = zip(500, "laptop")
            zips["tlk_autosave@cj-desk-pc"] = zip(400, "pc", device = "cj-desk-pc")
            zips["tlk_autosave@old"] = zip(900, "old build", build = "b0")
        }
        val r = sync(dir, store)
        assertEquals(listOf("tlk_autosave@fold"), r.pushed)
        assertEquals(listOf("tlk_autosave@cj-cos-laptop"), r.pulled)
        assertEquals("laptop", File(dir, "tlk_autosave.state").readText())
        val slot = Slots.read(dir, Slots.AUTOSAVE, build)!!
        assertEquals("cj-cos-laptop", slot.device)
        assertEquals("Exile, from cj-cos-laptop, 1 minute ago", Slots.describe(slot, 560, "fold"))
        assertEquals("Exile, 1 minute ago", Slots.describe(slot, 560, "cj-cos-laptop"))
    }

    @Test
    fun localAutosaveNewerThanEveryOtherIsKept() {
        val dir = tmp.newFolder()
        local(dir, Slots.AUTOSAVE, 900, "phone")
        val store = MemoryStore().apply { zips["tlk_autosave@cj-cos-laptop"] = zip(500, "laptop") }
        assertTrue(sync(dir, store).pulled.isEmpty())
        assertEquals("phone", File(dir, "tlk_autosave.state").readText())
    }

    @Test
    fun offlineSyncReportsAndDoesNotBlock() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot1", 100, "mine")
        val r = sync(dir, MemoryStore().apply { reachable = false })
        assertEquals("offline", r.problem)
        assertEquals("mine", File(dir, "tlk_slot1.state").readText())
    }

    @Test
    fun aStoppedSyncLeavesTheFilesAlone() {
        val dir = tmp.newFolder()
        val store = MemoryStore().apply { zips["tlk_slot1"] = zip(300, "remote") }
        val s = SaveSync(dir, store, "fold", build) { false }
        s.stop()
        s.run("offline")
        assertTrue(!File(dir, "tlk_slot1.state").exists())
    }

    @Test
    fun zipWithAStateThatDoesNotMatchItsSidecarIsRefused() {
        val info = JSONObject().put("saved_unix", 1).put("state_sha256", SaveZip.sha256("other".toByteArray()))
        assertThrows(java.io.IOException::class.java) { SaveZip.unpack(SaveZip.pack("state".toByteArray(), info, null)) }
    }

    @Test
    fun sharedDeviceNameCases() {
        for (c in cases("device_name")) assertEquals(c.getString("id"), c.getString("want"), SaveSync.deviceName(c.getString("raw")))
    }

    @Test
    fun localStateRewrittenAfterItsJsonIsReplacedByTheRemoteCopy() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot1", 900, "mine")
        val s = SaveSync(dir, MemoryStore().apply { zips["tlk_slot1"] = zip(100, "remote") }, "fold", build) { false }
        assertTrue(s.localInfo("tlk_slot1")!!.getBoolean("sha_ok"))
        File(dir, "tlk_slot1.state").writeText("rewritten")
        assertTrue(!s.localInfo("tlk_slot1")!!.getBoolean("sha_ok"))
        assertEquals(listOf("tlk_slot1"), s.run("offline").pulled)
        assertEquals("remote", File(dir, "tlk_slot1.state").readText())
    }

    @Test
    fun continueSkipsALocalAutosaveWhoseStateDoesNotMatch() {
        val dir = tmp.newFolder()
        local(dir, Slots.AUTOSAVE, 900, "phone")
        File(dir, "tlk_autosave.state").writeText("cut short")
        val store = MemoryStore().apply { zips["tlk_autosave@cj-cos-laptop"] = zip(100, "laptop") }
        assertEquals(listOf("tlk_autosave@cj-cos-laptop"), sync(dir, store).pulled)
        assertEquals("laptop", File(dir, "tlk_autosave.state").readText())
    }

    @Test
    fun pullDoesNotRefetchAnAutosaveItAlreadyHas() {
        val dir = tmp.newFolder()
        val store = MemoryStore().apply { zips["tlk_autosave@cj-cos-laptop"] = zip(500, "laptop") }
        sync(dir, store)
        File(dir, "tlk_autosave.prev.state").writeText("keep me")
        assertTrue(sync(dir, store).pulled.isEmpty())
        assertEquals("keep me", File(dir, "tlk_autosave.prev.state").readText())
    }

    @Test
    fun stopDuringADownloadLeavesTheFilesAlone() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot1", 100, "mine")
        lateinit var s: SaveSync
        val inner = MemoryStore().apply { zips["tlk_slot1"] = zip(300, "remote") }
        val store = object : SaveStore by inner {
            override fun get(name: String): ByteArray = inner.get(name).also { s.stop() }
        }
        s = SaveSync(dir, store, "fold", build) { false }
        assertTrue(s.run("offline").pulled.isEmpty())
        assertEquals("mine", File(dir, "tlk_slot1.state").readText())
    }

    @Test
    fun aRefusedPutIsSkippedAndTheOthersStillGo() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot1", 100, "being rewritten")
        local(dir, "tlk_slot2", 100, "fine")
        val inner = MemoryStore()
        val store = object : SaveStore by inner {
            override fun put(name: String, zip: ByteArray) {
                if (name == "tlk_slot1") throw java.io.IOException("PUT /v1/saves/tlk_slot1: 400")
                inner.put(name, zip)
            }
        }
        val r = sync(dir, store)
        assertEquals(listOf("tlk_slot2"), r.pushed)
        assertEquals(listOf("tlk_slot1"), r.kept)
        assertNull(r.problem)
    }

    @Test
    fun pushConflictKeepsTheStoredCopyAndGoesOn() {
        val dir = tmp.newFolder()
        local(dir, "tlk_slot1", 200, "mine")
        local(dir, "tlk_slot2", 200, "also mine")
        val inner = MemoryStore()
        val store = object : SaveStore by inner {
            override fun put(name: String, zip: ByteArray) {
                if (name == "tlk_slot1") throw SaveConflict("newer")
                inner.put(name, zip)
            }
        }
        val r = sync(dir, store)
        assertEquals(listOf("tlk_slot2"), r.pushed)
        assertEquals(listOf("tlk_slot1"), r.kept)
        assertEquals("mine", File(dir, "tlk_slot1.state").readText())
    }

    @Test
    fun httpStoreMapsTheServersAnswers() {
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val stored = zip(100, "s")
        val puts = mutableListOf<ByteArray>()
        server.createContext("/") { ex ->
            val (code, body) = when {
                ex.requestURI.path == "/v1/saves" -> 200 to """{"saves": {"tlk_slot1": {"saved_unix": 100}}}""".toByteArray()
                ex.requestMethod == "GET" && ex.requestURI.path == "/v1/saves/tlk_slot1" -> 200 to stored
                ex.requestMethod == "PUT" && ex.requestURI.path == "/v1/saves/tlk_slot2" -> {
                    puts += ex.requestBody.readBytes()
                    204 to ByteArray(0)
                }
                ex.requestMethod == "PUT" && ex.requestURI.path == "/v1/saves/tlk_slot4" -> 502 to "bad gateway".toByteArray()
                ex.requestMethod == "PUT" -> 409 to "newer".toByteArray()
                else -> 404 to "no".toByteArray()
            }
            ex.sendResponseHeaders(code, if (body.isEmpty()) -1 else body.size.toLong())
            if (body.isNotEmpty()) ex.responseBody.use { it.write(body) }
            ex.close()
        }
        server.start()
        try {
            val store = HttpSaveStore("http://127.0.0.1:${server.address.port}/")
            assertEquals(100L, store.list().getValue("tlk_slot1").getLong("saved_unix"))
            assertArrayEquals(stored, store.get("tlk_slot1"))
            assertThrows(NoSuchElementException::class.java) { store.get("tlk_slot3") }
            store.put("tlk_slot2", stored)
            assertArrayEquals(stored, puts.single())
            assertThrows(SaveConflict::class.java) { store.put("tlk_slot1", stored) }
            assertEquals("127.0.0.1", store.host)
            val failed = assertThrows(java.io.IOException::class.java) { store.put("tlk_slot4", stored) }
            assertTrue(failed !is StoreUnreachable && failed.message!!.contains("502"))
        } finally {
            server.stop(0)
        }
        assertThrows(StoreUnreachable::class.java) { HttpSaveStore("http://127.0.0.1:9", connectMs = 500).list() }
        assertEquals("cj-omv", HttpSaveStore("https://cj-omv.bandicoot-exponential.ts.net:8450").host)
    }
}
