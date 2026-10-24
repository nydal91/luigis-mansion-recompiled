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
            final File res = new File(getFilesDir(), "res");
            copyAssets(res);
            saveCrashLog();
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

    /**
     * Copies the previous run's log (files/last_run.log, before the native side renames it) to
     * Download/LuigisMansionPort/previous_run.log on every start, replacing the last copy, so a
     * player can send the log of a run that crashed or froze without any extra tools.
     */
    private void saveCrashLog() {
        final File log = new File(getFilesDir(), "last_run.log");
        if (!log.exists() || log.length() == 0 || Build.VERSION.SDK_INT < 29) return;
        final android.content.ContentResolver cr = getContentResolver();
        final Uri downloads = android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        final String dir = "Download/LuigisMansionPort/";
        final String name = "previous_run.log";
        try {
            // Our earlier copy (an app may delete the files it created).
            cr.delete(downloads,
                    android.provider.MediaStore.Downloads.DISPLAY_NAME + "=? AND " +
                            android.provider.MediaStore.Downloads.RELATIVE_PATH + "=?",
                    new String[] {name, dir});
        } catch (Exception e) {
            Log.w(TAG, "could not remove the old log copy", e);
        }
        try {
            final android.content.ContentValues v = new android.content.ContentValues();
            v.put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name);
            v.put(android.provider.MediaStore.Downloads.MIME_TYPE, "text/plain");
            v.put(android.provider.MediaStore.Downloads.RELATIVE_PATH, dir);
            final Uri uri = cr.insert(downloads, v);
            if (uri == null) {
                Log.w(TAG, "MediaStore refused the log copy");
                return;
            }
            try (InputStream in = new java.io.FileInputStream(log); OutputStream out = cr.openOutputStream(uri)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            }
            Log.i(TAG, "previous run's log saved to " + dir + name);
        } catch (Exception e) {
            Log.w(TAG, "could not save the previous run's log", e);
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

    /** PowerManager thermal status (0 none ... 6 shutdown), -1 if unknown. */
    public static int thermalStatus() {
        if (sSelf == null || Build.VERSION.SDK_INT < 29) return -1;
        final android.os.PowerManager pm = (android.os.PowerManager) sSelf.getSystemService(Context.POWER_SERVICE);
        return pm != null ? pm.getCurrentThermalStatus() : -1;
    }

    /** Opens the system share sheet with the game's log (LogProvider). */
    public static void shareLog() {
        if (sSelf == null) return;
        sSelf.runOnUiThread(new Runnable() { @Override public void run() {
            final Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, Uri.parse("content://" + LogProvider.AUTHORITY + "/last_run.log"));
            send.putExtra(Intent.EXTRA_SUBJECT, "Luigi's Mansion Port log");
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            sSelf.startActivity(Intent.createChooser(send, "Share log"));
        }});
    }

    // ---- Texture packs (src/recomp/texture_pack.cpp) ----

    private static final int REQ_TEXTURE_ZIP = 7101;
    private static final int REQ_TEXTURE_DIR = 7102;
    private static String sTextureDest;
    // 0 idle, 1 running, 2 done, 3 failed (port::android::ImportState)
    private static volatile int sTexState = 0;
    private static volatile int sTexCount = 0;
    private static volatile String sTexMessage = "";

    /** Opens the system picker for a texture pack .zip or folder; the copy starts when one is chosen. */
    public static void pickTexturePack(final boolean folder, final String destDir) {
        if (sSelf == null) return;
        sTextureDest = destDir;
        sSelf.runOnUiThread(new Runnable() { @Override public void run() {
            final Intent i;
            if (folder) {
                i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            } else {
                i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            }
            try {
                sSelf.startActivityForResult(i, folder ? REQ_TEXTURE_DIR : REQ_TEXTURE_ZIP);
            } catch (Exception e) {
                sTexState = 3;
                sTexMessage = "No file picker available: " + e;
            }
        }});
    }

    /** "<state>|<count>|<message>" for the native side. */
    public static String texturePackStatus() {
        return sTexState + "|" + sTexCount + "|" + sTexMessage;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_TEXTURE_ZIP && requestCode != REQ_TEXTURE_DIR) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final Uri uri = data != null ? data.getData() : null;
        if (resultCode != RESULT_OK || uri == null || sTextureDest == null) {
            sTexState = 0;
            return;
        }
        final boolean folder = requestCode == REQ_TEXTURE_DIR;
        sTexState = 1;
        sTexCount = 0;
        sTexMessage = "";
        new Thread(new Runnable() { @Override public void run() {
            try {
                final int n = folder ? importTextureFolder(uri) : importTextureZip(uri);
                sTexMessage = n > 0 ? "Imported " + n + " texture files."
                                    : "No texture files (tex1_*.png / .dds) found in what you picked.";
                sTexState = n > 0 ? 2 : 3;
            } catch (Throwable t) {
                Log.e(TAG, "texture pack import failed", t);
                sTexMessage = "Import failed: " + t.getMessage();
                sTexState = 3;
            }
        }}, "texture-import").start();
    }

    // Texture files only: tex1_*.png / tex1_*.dds (Dolphin's naming; mip sidecars included).
    private static boolean isTextureName(String name) {
        final String n = name.toLowerCase(java.util.Locale.ROOT);
        return n.startsWith("tex1_") && (n.endsWith(".png") || n.endsWith(".dds"));
    }

    // A folder name for the pack from what the player picked (no path separators).
    private static File packDir(String label) {
        String name = label == null ? "" : label.replaceAll("\\.(zip|ZIP)$", "").replaceAll("[^A-Za-z0-9 ._()-]", "_").trim();
        if (name.isEmpty() || name.startsWith(".")) name = "pack";
        final File dir = new File(sTextureDest, name);
        deleteTree(dir);  // importing the same pack again replaces it
        dir.mkdirs();
        return dir;
    }

    private static void deleteTree(File f) {
        final File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    // Writes one texture by its bare file name (flat: names are unique by content hash, and no
    // path from the archive is used, so nothing can land outside the pack folder).
    private static void writeTexture(File dir, String name, InputStream in, byte[] buf) throws Exception {
        final File tmp = new File(dir, name + ".part");
        try (OutputStream os = new FileOutputStream(tmp)) {
            for (int n; (n = in.read(buf)) > 0;) os.write(buf, 0, n);
        }
        if (!tmp.renameTo(new File(dir, name))) throw new java.io.IOException("could not write " + name);
        sTexCount++;
    }

    private static int importTextureZip(Uri uri) throws Exception {
        final File dir = packDir(contentDisplayName(uri.toString()));
        final byte[] buf = new byte[1 << 16];
        try (InputStream raw = sSelf.getContentResolver().openInputStream(uri);
             java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(raw, 1 << 16))) {
            for (java.util.zip.ZipEntry e; (e = zip.getNextEntry()) != null;) {
                if (e.isDirectory()) continue;
                String name = e.getName();
                name = name.substring(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
                if (isTextureName(name)) writeTexture(dir, name, zip, buf);
            }
        }
        return sTexCount;
    }

    private static int importTextureFolder(Uri tree) throws Exception {
        final ContentResolver cr = sSelf.getContentResolver();
        final String rootId = android.provider.DocumentsContract.getTreeDocumentId(tree);
        String label = rootId.substring(rootId.lastIndexOf(':') + 1);
        label = label.substring(label.lastIndexOf('/') + 1);
        final File dir = packDir(label);
        final byte[] buf = new byte[1 << 16];
        final java.util.ArrayDeque<String> pending = new java.util.ArrayDeque<>();
        pending.add(rootId);
        while (!pending.isEmpty()) {
            final String id = pending.poll();
            final Uri children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
            try (Cursor c = cr.query(children, new String[] {
                    android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
                if (c == null) continue;
                while (c.moveToNext()) {
                    final String childId = c.getString(0), name = c.getString(1), mime = c.getString(2);
                    if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                        pending.add(childId);
                    } else if (name != null && isTextureName(name) && name.indexOf('/') < 0) {
                        final Uri doc = android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, childId);
                        try (InputStream in = cr.openInputStream(doc)) {
                            writeTexture(dir, name, in, buf);
                        }
                    }
                }
            }
        }
        return sTexCount;
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
