package com.oxwald.player;

import android.os.Bundle;
import android.view.View;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

/**
 * OxwaldPlayer activity: GameActivity hosts liboxwald_player.so (android_main in apps/player/android).
 * Launch arguments are intent extras read by the native side:
 *   adb shell am start -n com.oxwald.player/.OxwaldActivity --es args "--frames 300 --stats" --ez wait_debugger true
 */
public class OxwaldActivity extends GameActivity {
    static {
        // Tracy's client starts in a static constructor of the library and, on x86 without an invariant-TSC CPUID
        // bit (the Android emulator), calls exit(1) ("CPU doesn't support invariant TSC") before android_main runs.
        // rdtsc is still usable there for profiling, so skip the check; it has to be in the environment before
        // the library is loaded.
        try {
            android.system.Os.setenv("TRACY_NO_INVARIANT_CHECK", "1", false);
        } catch (android.system.ErrnoException ignored) {
        }
        System.loadLibrary("oxwald_player");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    // Immersive full screen: the game draws edge to edge, system bars come back with a swipe.
    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(getWindow(), decor);
        controller.hide(WindowInsetsCompat.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
