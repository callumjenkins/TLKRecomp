package uk.callum.tlkrecomp

import android.os.Bundle
import android.os.Process
import android.system.Os
import android.view.InputDevice
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

    // The phone's own Back opens the game's menu. A controller's Back is its Select button, which SDL
    // reads as the gamepad's Back; Back from any other real device is dropped.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        val device = event.device
        if (device != null && !device.isVirtual) {
            val pad = device.sources and (InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK) != 0
            return if (pad) super.dispatchKeyEvent(event) else true
        }
        if (event.action != KeyEvent.ACTION_UP) return true
        onNativeKeyDown(KeyEvent.KEYCODE_BACK)
        onNativeKeyUp(KeyEvent.KEYCODE_BACK)
        return true
    }

    // SDLActivity's finishes the activity, which ends the game; the menu's Quit does that instead.
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
