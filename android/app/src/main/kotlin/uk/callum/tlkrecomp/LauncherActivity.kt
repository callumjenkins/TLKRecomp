package uk.callum.tlkrecomp

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors

/** The ROM, Continue, New game, Save slots and Settings, with everything kept in the app's files folder. */
class LauncherActivity : Activity() {
    private val work = Executors.newSingleThreadExecutor()
    private val rom get() = File(filesDir, GameActivity.ROM)
    private val identity = RomIdentity(BuildConfig.ROM_SHA256, BuildConfig.ROM_SIZE)
    private var busy = false

    private lateinit var romLabel: TextView
    private lateinit var chooseRom: Button
    private lateinit var continueRow: LinearLayout
    private lateinit var continuePicture: ImageView
    private lateinit var continueText: TextView
    private lateinit var newGame: Button
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val dp = resources.displayMetrics.density
        val pad = (16 * dp).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        fun text(s: String = "", size: Float = 16f, bold: Boolean = false, into: LinearLayout = column) = TextView(this).apply {
            text = s
            textSize = size
            if (bold) setTypeface(typeface, Typeface.BOLD)
            into.addView(this)
        }
        fun button(label: String, action: () -> Unit) = Button(this).apply {
            text = label
            setOnClickListener { action() }
            column.addView(this)
        }
        LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            addView(ImageView(this@LauncherActivity).apply { setImageResource(R.mipmap.ic_launcher) },
                LinearLayout.LayoutParams((56 * dp).toInt(), (56 * dp).toInt()).apply { marginEnd = pad })
            val names = LinearLayout(this@LauncherActivity).apply { orientation = LinearLayout.VERTICAL }
            text(getString(R.string.app_name), 24f, bold = true, into = names)
            text("${BuildConfig.VERSION_NAME}, build ${BuildConfig.BUILD_ID}", 12f, into = names)
            addView(names)
            column.addView(this)
        }
        romLabel = text().apply { setPadding(0, pad, 0, 0) }
        chooseRom = button("Choose ROM…") { pickRom() }
        continueRow = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, pad, 0, 0)
            isClickable = true
            setOnClickListener { play(File(filesDir, "${Slots.AUTOSAVE}.state")) }
            continuePicture = ImageView(this@LauncherActivity).apply { adjustViewBounds = true }
            addView(continuePicture, LinearLayout.LayoutParams((128 * dp).toInt(), LinearLayout.LayoutParams.WRAP_CONTENT).apply { marginEnd = pad })
            val words = LinearLayout(this@LauncherActivity).apply { orientation = LinearLayout.VERTICAL }
            text("Continue", 20f, bold = true, into = words)
            continueText = text(into = words)
            addView(words)
            column.addView(this)
        }
        newGame = button("New game") { newGame() }
        button("Save slots") { startActivityForResult(Intent(this, SlotsActivity::class.java), SLOTS) }
        button("Settings") { startActivity(Intent(this, SettingsActivity::class.java)) }
        status = text().apply { setPadding(0, pad, 0, 0) }
        setContentView(ScrollView(this).apply {
            fitsSystemWindows = true   // Android 15 draws an app under the status and navigation bars
            addView(column)
        })
    }

    override fun onResume() {
        super.onResume()
        show()
    }

    private fun autosave() = Slots.read(filesDir, Slots.AUTOSAVE, BuildConfig.BUILD_ID)

    private fun show() {
        val have = rom.isFile
        romLabel.text = if (have) "ROM: The Lion King (USA), checked." else "Choose your copy of The Lion King (USA), a .sfc or .smc file."
        chooseRom.text = if (have) "Choose another ROM…" else "Choose ROM…"
        newGame.isEnabled = have && !busy
        val auto = autosave()
        continueRow.visibility = if (have && auto?.usable == true) View.VISIBLE else View.GONE
        if (auto?.usable == true) {
            continueText.text = Slots.describe(auto, System.currentTimeMillis() / 1000)
            continuePicture.setImageBitmap(auto.picture?.let { BitmapFactory.decodeFile(it.path) })
        } else if (auto != null) {
            say("Your autosave is from another build of the game, which this one can't load. Save slots shows it.")
        }
    }

    private fun newGame() {
        val auto = autosave()
        if (auto?.usable != true) return play(null)
        AlertDialog.Builder(this)
            .setTitle("Start a new game?")
            .setMessage("Continue keeps your last game until the new one autosaves. To keep it for good, copy it to a slot in Save slots first.")
            .setPositiveButton("New game") { _, _ -> play(null) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun gameRunning() =
        (getSystemService(ACTIVITY_SERVICE) as ActivityManager).runningAppProcesses?.any { it.processName == "$packageName:game" } == true

    private fun play(state: File?) {
        if (!rom.isFile) return say("Choose your ROM first.")
        // The game keeps its state in globals, so a second one in the same process would share them.
        if (gameRunning()) return say("The last game is still closing. Try again in a moment.")
        val args = state?.let { arrayOf("--load", it.path) } ?: emptyArray()
        say("")
        startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.ARGS, args))
    }

    private fun pickRom() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK)
    }

    @Deprecated("startActivityForResult is all a plain Activity has")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        when (requestCode) {
            PICK -> data.data?.let { uri -> importRom(uri) }
            SLOTS -> data.getStringExtra(SlotsActivity.EXTRA_STATE)?.let { play(File(it)) }
        }
    }

    private fun importRom(uri: Uri) {
        if (busy) return
        busy = true
        newGame.isEnabled = false
        say("Checking the ROM…")
        work.execute {
            val message = try {
                val length = contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use {
                    if (it.moveToFirst() && !it.isNull(0)) it.getLong(0) else null
                } ?: -1L
                if (length < 0) throw java.io.IOException("the phone didn't say how big it is")
                val next = File(filesDir, "${GameActivity.ROM}.new")
                val why = contentResolver.openInputStream(uri)!!.use { input ->
                    next.outputStream().use { out -> Rom.copyChecked(input, length, out, identity) }
                }
                if (why == null && next.renameTo(rom)) "ROM checked. Ready to play."
                else {
                    next.delete()
                    "That file isn't The Lion King (USA), so it wasn't used. ${why ?: ""}"
                }
            } catch (e: Exception) {
                "Couldn't read that file: ${e.message}"
            }
            runOnUiThread {
                busy = false
                show()
                say(message)
            }
        }
    }

    private fun say(s: String) = runOnUiThread { status.text = s }

    companion object {
        private const val PICK = 1
        private const val SLOTS = 2
    }
}
