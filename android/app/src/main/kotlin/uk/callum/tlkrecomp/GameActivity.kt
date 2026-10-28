package uk.callum.tlkrecomp

import android.os.Bundle
import android.os.Process
import android.system.Os
import android.view.KeyEvent
import org.libsdl.app.SDLActivity
import java.io.File
import java.io.FileOutputStream

/** The game, in SDL's activity, in a process of its own, with its settings, saves and ROM in the app's files folder. */
class GameActivity : SDLActivity() {
    override fun getLibraries() = arrayOf("SDL3", "main")

    // sdl_main.c defines main() itself, with SDL_MAIN_HANDLED
    override fun getMainFunction() = "main"

    override fun onCreate(savedInstanceState: Bundle?) {
        // A proof run (adb shell am start --es stdout NAME) writes the game's stdout to that file in the files folder.
        intent.getStringExtra(STDOUT)?.let { FileOutputStream(File(filesDir, it)).use { out -> Os.dup2(out.fd, 1) } }
        super.onCreate(savedInstanceState)
    }

    override fun getArguments(): Array<String> {
        val extra = intent.getStringArrayExtra(ARGS) ?: emptyArray()
        return arrayOf("--fullscreen", "--config", File(filesDir, "tlk_sdl.ini").path, "--rom", File(filesDir, ROM).path) + extra
    }

    // The phone's own Back opens the game's menu. A controller can send Back too, for a button or as
    // it reconnects, so a Back from any real device is dropped; the gamepad's Back button opens the menu.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        if (event.device?.isVirtual == false || event.action != KeyEvent.ACTION_UP) return true
        onNativeKeyDown(KeyEvent.KEYCODE_BACK)
        onNativeKeyUp(KeyEvent.KEYCODE_BACK)
        return true
    }

    // SDL's would finish the activity mid-frame; the menu's Quit ends the game instead.
    @Deprecated("SDLActivity overrides it")
    override fun onBackPressed() {}

    override fun onDestroy() {
        super.onDestroy()
        Process.killProcess(Process.myPid())
    }

    companion object {
        const val ARGS = "args"
        const val ROM = "rom.sfc"
        const val STDOUT = "stdout"
    }
}
