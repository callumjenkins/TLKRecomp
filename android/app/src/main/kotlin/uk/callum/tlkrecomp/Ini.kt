package uk.callum.tlkrecomp

/** tlk_sdl.ini's "key = value" lines, read and changed in place so every other line stays as it was. */
object Ini {
    private fun keyOf(line: String): String? {
        val s = line.trim()
        if (s.isEmpty() || s[0] == ';' || s[0] == '#' || s[0] == '[' || '=' !in s) return null
        return s.substringBefore('=').trim().lowercase()
    }

    fun get(text: String, key: String): String? =
        text.lines().lastOrNull { keyOf(it) == key }?.substringAfter('=')?.trim()

    /** Changes the line the game reads, which is the last with that key, or adds one at the end. */
    fun set(text: String, key: String, value: String): String {
        val lines = text.lines().toMutableList()
        val at = lines.indexOfLast { keyOf(it) == key }
        if (at >= 0) {
            lines[at] = "${lines[at].substringBefore('=').trimEnd()} = $value"
            return lines.joinToString("\n")
        }
        val body = if (text.isEmpty() || text.endsWith("\n")) text else "$text\n"
        return "$body$key = $value\n"
    }
}
