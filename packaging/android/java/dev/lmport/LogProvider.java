package dev.lmport;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Hands the game's log files (files/last_run.log, previous_run.log) to the app the player
 * shares them with (F1 > Enhancements > Debug > Share log; LMActivity.shareLog). Read-only,
 * only those two files, and only to apps the share grants access to.
 */
public class LogProvider extends ContentProvider {
    static final String AUTHORITY = "dev.lmport.luigismansion.logs";

    private File fileFor(Uri uri) throws FileNotFoundException {
        final String name = uri.getLastPathSegment();
        if (!"last_run.log".equals(name) && !"previous_run.log".equals(name)) throw new FileNotFoundException(name);
        final File f = new File(getContext().getFilesDir(), name);
        if (!f.exists()) throw new FileNotFoundException(name);
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            final File f = fileFor(uri);
            final MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            c.addRow(new Object[] {f.getName(), f.length()});
            return c;
        } catch (FileNotFoundException e) {
            return null;
        }
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
