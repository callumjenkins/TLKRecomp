package uk.callum.tlkrecomp

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** The autosave and the four slots. A tap loads one, through the launcher; a long press copies or deletes it. */
class SlotsActivity : Activity() {
    private lateinit var column: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        setContentView(ScrollView(this).apply {
            fitsSystemWindows = true
            addView(column)
        })
        show()
    }

    private fun show() {
        column.removeAllViews()
        val dp = resources.displayMetrics.density
        val now = System.currentTimeMillis() / 1000
        column.addView(TextView(this).apply {
            text = "Save slots"
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })
        column.addView(TextView(this).apply {
            text = "Save to a slot from the game's menu (Back), under Save states."
            setPadding(0, 0, 0, (8 * dp).toInt())
        })
        for (base in Slots.bases) {
            val slot = Slots.read(filesDir, base, BuildConfig.BUILD_ID)
            val row = LinearLayout(this).apply {
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, (8 * dp).toInt(), 0, (8 * dp).toInt())
            }
            row.addView(ImageView(this).apply {
                adjustViewBounds = true
                minimumHeight = (112 * dp).toInt()
                slot?.picture?.let { setImageBitmap(BitmapFactory.decodeFile(it.path)) }
            }, LinearLayout.LayoutParams((128 * dp).toInt(), LinearLayout.LayoutParams.WRAP_CONTENT).apply { marginEnd = (16 * dp).toInt() })
            val words = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            words.addView(TextView(this).apply {
                text = Slots.title(base)
                textSize = 18f
                setTypeface(typeface, Typeface.BOLD)
            })
            words.addView(TextView(this).apply {
                text = slot?.let { Slots.describe(it, now) + (it.lives?.let { n -> ", $n lives" } ?: "") } ?: "Empty"
            })
            row.addView(words)
            if (slot != null) {
                if (slot.usable) row.setOnClickListener {
                    setResult(RESULT_OK, Intent().putExtra(EXTRA_STATE, slot.state.path))
                    finish()
                }
                row.setOnLongClickListener { manage(slot); true }
            }
            column.addView(row)
        }
    }

    private fun manage(slot: Slot) {
        val targets = Slots.bases.filter { it != Slots.AUTOSAVE && it != slot.base }
        val labels = targets.map { base ->
            val there = Slots.read(filesDir, base, BuildConfig.BUILD_ID)
            "Copy to ${Slots.title(base).lowercase()}" + (there?.let { " (replaces ${it.levelName ?: "a save"})" } ?: "")
        } + "Delete"
        AlertDialog.Builder(this)
            .setTitle(slot.title)
            .setItems(labels.toTypedArray()) { _, i ->
                if (i < targets.size) {
                    Slots.copy(slot, targets[i])
                    show()
                } else AlertDialog.Builder(this)
                    .setMessage("Delete ${slot.title.lowercase()}?")
                    .setPositiveButton("Delete") { _, _ -> Slots.delete(slot); show() }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
            .show()
    }

    companion object {
        const val EXTRA_STATE = "state"
    }
}
