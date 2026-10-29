package uk.callum.tlkrecomp

import android.app.Activity
import android.app.ActivityManager
import android.graphics.Typeface
import android.os.Bundle
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import java.io.File

/**
 * The settings that matter before the game starts, written to tlk_sdl.ini in place. The rest are in the
 * game's own menu, which rewrites the whole file, so nothing here is written while the game is running.
 */
class SettingsActivity : Activity() {
    private val ini get() = File(filesDir, "tlk_sdl.ini")
    private lateinit var column: LinearLayout
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        column.addView(TextView(this).apply {
            text = "Settings"
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })
        choice("Picture shape", "aspect", listOf("4:3 TV" to "4:3", "8:7 pixels" to "8:7", "1:1" to "1:1"), "8:7")
        choice("Widescreen", "widescreen",
            listOf("Off" to "off", "16:9" to "16:9", "21:9" to "21:9", "Fit the screen" to "adaptive"), "adaptive")
        choice("Touch pad", "touch", listOf("Auto: hidden while a controller is connected" to "auto", "On" to "on", "Off" to "off"), "auto")
        number("Touch pad size", "touch_size", 50, 150, 100, "%")
        choice("Vibrate on touch", "touch_haptics", listOf("On" to "1", "Off" to "0"), "1")
        number("Volume", "volume", 0, 100, 100, "%")
        choice("Autosave", "autosave_seconds", listOf("Every 30 seconds" to "30", "Every minute" to "60",
            "Every 2 minutes" to "120", "Every 5 minutes" to "300", "Off" to "0"), "60")
        val sync = SyncPrefs(this)
        field("Save sync server", sync.server, "Empty for no sync, or https://HOST:8450", InputType.TYPE_TEXT_VARIATION_URI) { sync.server = it }
        field("This device", sync.device, "The name its autosave is kept under", InputType.TYPE_CLASS_TEXT) { sync.device = it }
        status = TextView(this).apply { setPadding(0, pad, 0, 0) }
        column.addView(status)
        setContentView(ScrollView(this).apply {
            fitsSystemWindows = true
            addView(column)
        })
    }

    private fun current(key: String) = if (ini.isFile) Ini.get(ini.readText(), key) else null

    private fun write(key: String, value: String) {
        if ((getSystemService(ACTIVITY_SERVICE) as ActivityManager).runningAppProcesses?.any { it.processName == "$packageName:game" } == true) {
            status.text = "The game is still running, so this wasn't saved. Close it from its menu first."
            return
        }
        ini.writeText(Ini.set(if (ini.isFile) ini.readText() else "", key, value))
        status.text = ""
    }

    private fun label(s: String) = column.addView(TextView(this).apply {
        text = s
        setTypeface(typeface, Typeface.BOLD)
        setPadding(0, (12 * resources.displayMetrics.density).toInt(), 0, 0)
    })

    private fun field(name: String, value: String, hint: String, type: Int, save: (String) -> Unit) {
        label(name)
        column.addView(EditText(this).apply {
            setText(value)
            this.hint = hint
            inputType = InputType.TYPE_CLASS_TEXT or type
            isSingleLine = true
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                override fun afterTextChanged(s: Editable?) = save(s.toString())
            })
        })
    }

    private fun choice(name: String, key: String, options: List<Pair<String, String>>, default: String) {
        label(name)
        val have = current(key)?.lowercase()
        val picked = options.indexOfFirst { it.second.lowercase() == have }.takeIf { it >= 0 }
            ?: options.indexOfFirst { it.second == default }
        column.addView(Spinner(this, Spinner.MODE_DROPDOWN).apply {
            adapter = ArrayAdapter(this@SettingsActivity, android.R.layout.simple_spinner_dropdown_item, options.map { it.first })
            setSelection(picked)
            var shown = picked   // the listener also hears setSelection
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, v: View?, position: Int, id: Long) {
                    if (position == shown) return
                    shown = position
                    write(key, options[position].second)
                }
                override fun onNothingSelected(parent: AdapterView<*>?) {}
            }
        })
    }

    private fun number(name: String, key: String, min: Int, max: Int, default: Int, unit: String) {
        label(name)
        val value = TextView(this)
        val start = (current(key)?.toIntOrNull() ?: default).coerceIn(min, max)
        value.text = "$start$unit"
        column.addView(value)
        column.addView(SeekBar(this).apply {
            this.max = (max - min) / 10
            progress = (start - min) / 10
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar?, p: Int, fromUser: Boolean) { value.text = "${min + p * 10}$unit" }
                override fun onStartTrackingTouch(bar: SeekBar?) {}
                override fun onStopTrackingTouch(bar: SeekBar?) { write(key, "${min + progress * 10}") }
            })
        })
    }
}
