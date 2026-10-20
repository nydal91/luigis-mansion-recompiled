package dev.lmport;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import dev.encounter.aurora.AuroraSurface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The game's activity (docs/ANDROID.md): SDL's activity running libmain.so (the port's
 * runtime, SDL3, Aurora). Before the native side starts it exports the app's folders as
 * environment variables (src/port/android.cpp reads them) and copies the APK assets the
 * runtime reads by path. The static methods are called from native code through JNI.
 */
public class LMActivity extends SDLActivity {
    private static final String TAG = "LuigisMansion";
    private static LMActivity sSelf;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sSelf = this;
        try {
            final String files = getFilesDir().getAbsolutePath();
            Os.setenv("LM_ANDROID_FILES", files, true);
            Os.setenv("LM_ANDROID_CACHE", getCacheDir().getAbsolutePath(), true);
            Os.setenv("LM_ANDROID_LIBDIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("HOME", files, true);
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), true);
            // Performance lines in the log (frame rate every 2 s, GPU time per pass every 10 s),
            // so a shared log (F1 > Enhancements > Debug > Share log) shows where time goes.
            Os.setenv("LM_FPS_LOG", "1", false);
            Os.setenv("AURORA_GPU_PROFILE", "10", false);
            final File res = new File(getFilesDir(), "res");
            copyAssets(res);
            Os.setenv("LM_ANDROID_RES", res.getAbsolutePath(), true);
        } catch (Exception e) {
            Log.e(TAG, "setup of the native environment failed", e);
        }
        super.onCreate(savedInstanceState);
        // The game runs with the screen on (also while the first-run setup compiles).
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // SDL3 is linked into libmain.so (static), so there is only one library to load.
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        final Intent i = getIntent();
        if (i != null && i.getBooleanExtra("setup", false)) return new String[] {"--setup"};
        return new String[0];
    }

    // Aurora tracks surface readiness through its own SDLSurface subclass.
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new AuroraSurface(context);
    }

    // Copies the assets (pipelines.bin, ...) to `dir` once per app version.
    private void copyAssets(File dir) throws Exception {
        long version;
        final PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
        version = Build.VERSION.SDK_INT >= 28 ? pi.getLongVersionCode() : pi.versionCode;
        final File stamp = new File(dir, ".version");
        final String want = version + " " + pi.lastUpdateTime;
        if (stamp.exists() && want.equals(readSmall(stamp))) return;
        dir.mkdirs();
        final AssetManager am = getAssets();
        final String[] names = am.list("");
        if (names != null) {
            byte[] buf = new byte[1 << 16];
            for (String name : names) {
                InputStream in;
                try {
                    in = am.open(name);
                } catch (Exception e) {
                    continue;  // a folder
                }
                try (InputStream is = in; OutputStream os = new FileOutputStream(new File(dir, name))) {
                    for (int n; (n = is.read(buf)) > 0;) os.write(buf, 0, n);
                }
            }
        }
        try (OutputStream os = new FileOutputStream(stamp)) {
            os.write(want.getBytes("UTF-8"));
        }
    }

    private static String readSmall(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8") : "";
        } catch (Exception e) {
            return "";
        }
    }

    // ---- Called from native code (src/port/android.cpp) ----

    /**
     * Opens a disc image the user picked with the system file picker (a content:// URI) and
     * returns a file descriptor the native side reads through /proc/self/fd, or -1. The read
     * permission is made persistent so the next start can open it again without asking.
     */
    public static int openContentFd(String uriString) {
        if (sSelf == null) return -1;
        try {
            final Uri uri = Uri.parse(uriString);
            final ContentResolver cr = sSelf.getContentResolver();
            try {
                cr.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception e) {
                // Not persistable (some providers): works for this run only.
            }
            final ParcelFileDescriptor pfd = cr.openFileDescriptor(uri, "r");
            return pfd != null ? pfd.detachFd() : -1;
        } catch (Exception e) {
            Log.w(TAG, "could not open " + uriString, e);
            return -1;
        }
    }

    /** The file name of a content:// URI (for the setup screen), or null. */
    public static String contentDisplayName(String uriString) {
        if (sSelf == null) return null;
        try (Cursor c = sSelf.getContentResolver().query(Uri.parse(uriString),
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception e) {
            Log.w(TAG, "no name for " + uriString, e);
        }
        return null;
    }

    /** Opens the system share sheet with the game's log (LogProvider). */
    public static void shareLog() {
        if (sSelf == null) return;
        sSelf.runOnUiThread(() -> {
            final Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, Uri.parse("content://" + LogProvider.AUTHORITY + "/last_run.log"));
            send.putExtra(Intent.EXTRA_SUBJECT, "Luigi's Mansion Port log");
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            sSelf.startActivity(Intent.createChooser(send, "Share log"));
        });
    }

    /**
     * Restarts the game in a new process (after setup changes; F1 > Game). A small activity
     * in its own process (LMRestartActivity) starts the game again once this process is gone.
     */
    public static void relaunch(boolean setup) {
        if (sSelf == null) return;
        final Intent i = new Intent(sSelf, LMRestartActivity.class);
        i.putExtra("setup", setup);
        i.putExtra("pid", android.os.Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        sSelf.startActivity(i);
    }
}
