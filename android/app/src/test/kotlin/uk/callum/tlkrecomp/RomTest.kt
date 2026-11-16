package uk.callum.tlkrecomp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.security.MessageDigest

/** The ROM check against a made-up dump of 3 KiB, in place of the game's 3 MiB. */
class RomTest {
    private val dump = ByteArray(3072) { (it * 7 + 3).toByte() }
    private val want = RomIdentity(MessageDigest.getInstance("SHA-256").digest(dump).joinToString("") { "%02x".format(it) }, dump.size.toLong())

    private fun check(file: ByteArray): Pair<String?, ByteArray> {
        val out = ByteArrayOutputStream()
        return Rom.copyChecked(ByteArrayInputStream(file), file.size.toLong(), out, want) to out.toByteArray()
    }

    @Test
    fun romCheckAcceptsTheDumpWithAndWithoutACopierHeaderAndCopiesItHeaderless() {
        val (plain, copied) = check(dump)
        assertNull(plain)
        assertArrayEquals(dump, copied)
        val (headered, stripped) = check(ByteArray(512) { 0x55 } + dump)
        assertNull(headered)
        assertArrayEquals(dump, stripped)
    }

    @Test
    fun romCheckRefusesAnotherGameATruncatedFileAndOneChangedByte() {
        assertTrue(check(ByteArray(4096))!!.first!!.contains("bytes"))
        val short = object : java.io.InputStream() {                           // claims the size, then ends early
            var left = 1000
            override fun read() = if (left-- > 0) 0 else -1
        }
        assertEquals("The file ended early.", Rom.copyChecked(short, dump.size.toLong(), ByteArrayOutputStream(), want))
        val changed = dump.copyOf().also { it[100] = (it[100] + 1).toByte() }
        assertTrue(check(changed).first!!.startsWith("Its SHA-256"))
    }

    @Test
    fun copierHeaderIsTheHalfKiBPastAWholeNumberOfKiB() {
        assertEquals(512L, Rom.headerBytes(3145728 + 512))
        assertEquals(0L, Rom.headerBytes(3145728))
    }
}
