package uk.callum.tlkrecomp

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest

/** The one dump a build plays: cmake/rom_identity.txt's SHA-256 and size of the headerless file. */
data class RomIdentity(val sha256: String, val size: Long)

object Rom {
    /** A copier header: the 512 bytes past a whole number of KiB, which the game strips too. */
    fun headerBytes(length: Long): Long = if (length % 1024 == 512L) 512 else 0

    /**
     * Copies `input`, `length` bytes long, to `out` without its copier header, and returns null when that is
     * the dump `want` names, or else why not. `out` holds rubbish after a refusal.
     */
    fun copyChecked(input: InputStream, length: Long, out: OutputStream, want: RomIdentity): String? {
        val skip = headerBytes(length)
        if (length - skip != want.size) return "It is ${length - skip} bytes; the US release is ${want.size}."
        val digest = MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(1 shl 16)
        fun read(max: Long): Int = input.read(buf, 0, minOf(buf.size.toLong(), max).toInt()).also { if (it < 0) throw EOFException() }
        try {
            var left = skip
            while (left > 0) left -= read(left)
            var copied = 0L
            while (copied < want.size) {
                val n = read(want.size - copied)
                digest.update(buf, 0, n)
                out.write(buf, 0, n)
                copied += n
            }
        } catch (_: EOFException) {
            return "The file ended early."
        }
        val got = digest.digest().joinToString("") { "%02x".format(it) }
        return if (got == want.sha256) null else "Its SHA-256 is ${got.take(12)}…, not the US release's ${want.sha256.take(12)}…."
    }
}
