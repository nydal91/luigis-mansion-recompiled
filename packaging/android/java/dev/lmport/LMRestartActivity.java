package dev.lmport;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Runs in its own process (":restart", AndroidManifest.xml): waits for the game's process to
 * exit, then starts LMActivity again in a fresh one. Native code cannot restart itself on
 * Android (no fork/exec), see port::relaunch.
 */
public class LMRestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final int pid = getIntent().getIntExtra("pid", -1);
        final boolean setup = getIntent().getBooleanExtra("setup", false);
        final Handler h = new Handler(Looper.getMainLooper());
        h.postDelayed(new Runnable() {
            int tries = 0;

            @Override
            public void run() {
                if (pid > 0 && new java.io.File("/proc/" + pid).exists() && tries++ < 40) {
                    h.postDelayed(this, 50);  // the old process is still shutting down
                    return;
                }
                final Intent i = new Intent(LMRestartActivity.this, LMActivity.class);
                i.putExtra("setup", setup);
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
                finish();
                Runtime.getRuntime().exit(0);
            }
        }, 100);
    }
}
