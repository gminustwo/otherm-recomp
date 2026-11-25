// Other M Recompiled on Android: SDL's activity, with the game (SDL and Dawn linked in) in libmain.so and nod's disc
// reader in libnod.so, plus what the game asks of Java (runtime/src/android/android_bridge.cpp): its folders, the disc
// image from the system's file picker, updates, restarting and quitting.
package io.github.gminustwo.otherm;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.OpenableColumns;
import android.system.Os;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public class OtherMActivity extends SDLActivity {
    private static final String TAG = "OtherM";
    private static final int REQUEST_DISC = 0x4F4D;
    /** RestartActivity's start: straight back into the game, past the start screen (OTHERM_SKIP_LAUNCHER). */
    static final String EXTRA_SKIP_LAUNCHER = "skip_launcher";

    /** The game's answer to the picker: the document and its name ("" for both when the player canceled). */
    private static native void nativeDiscPicked(String uri, String name);

    @Override
    protected String[] getLibraries() {
        return new String[] {"nod", "main"};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new OtherMSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        prepareFolders();
        super.onCreate(savedInstanceState);
    }

    // The runtime starts once per process: once the game has ended (Quit, or Android closing the activity), the process
    // goes too, so the next start is a fresh one.
    @Override
    protected void onDestroy() {
        super.onDestroy();
        Process.killProcess(Process.myPid());
    }

    // --- folders ---------------------------------------------------------------------------------------------------
    // OTHERM_DATA_DIR: the player's things (Config.toml, saves, logs, mods, the game set up in Disc/DATA) in the app's
    // folder on shared storage (Android/data/<app>/files), which a PC reaches over USB. OTHERM_RUNTIME_DIR: the files a
    // desktop build keeps beside the program, unpacked from the APK's assets/runtime whenever the APK changed
    // (host_platform.cpp ExecutableDirectory).
    private void prepareFolders() {
        File data = getExternalFilesDir(null);
        if (data == null) data = getFilesDir();
        File runtime = new File(getFilesDir(), "runtime");
        try {
            String build = readAsset("runtime/build.txt");
            File stamp = new File(runtime, "build.txt");
            if (!build.equals(readFile(stamp))) {
                deleteTree(runtime);
                copyAssets(getAssets(), "runtime", runtime);
                // build.txt last of all: an unpack cut short is redone at the next start
                try (InputStream in = getAssets().open("runtime/build.txt")) {
                    copy(in, stamp);
                }
                Log.i(TAG, "unpacked the runtime files for " + build);
            }
            File profile = new File(data, "Metroid.ini");
            if (!profile.exists()) {
                data.mkdirs();
                try (InputStream in = getAssets().open("Metroid.ini")) {
                    copy(in, profile);
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "preparing the app's folders failed", e);
        }
        try {
            Os.setenv("OTHERM_DATA_DIR", data.getAbsolutePath(), true);
            Os.setenv("OTHERM_RUNTIME_DIR", runtime.getAbsolutePath(), true);
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), true);
            Intent start = getIntent();
            if (start != null && start.getBooleanExtra(EXTRA_SKIP_LAUNCHER, false)) Os.setenv("OTHERM_SKIP_LAUNCHER", "1", true);
        } catch (Exception e) {
            Log.e(TAG, "setting the game's folders failed", e);
        }
    }

    private String readAsset(String name) throws IOException {
        try (InputStream in = getAssets().open(name)) {
            return readText(in);
        }
    }

    private static String readFile(File file) {
        try (InputStream in = new FileInputStream(file)) {
            return readText(in);
        } catch (IOException e) {
            return "";
        }
    }

    // (InputStream.readAllBytes is Android 13 and later)
    private static String readText(InputStream in) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        for (int n; (n = in.read(buffer)) > 0; ) bytes.write(buffer, 0, n);
        return new String(bytes.toByteArray(), StandardCharsets.UTF_8).trim();
    }

    private static void copyAssets(AssetManager assets, String path, File out) throws IOException {
        if (path.equals("runtime/build.txt")) return;
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            out.mkdirs();
            for (String child : children) copyAssets(assets, path + "/" + child, new File(out, child));
            return;
        }
        try (InputStream in = assets.open(path)) {
            copy(in, out);
        }
    }

    private static void copy(InputStream in, File out) throws IOException {
        File partial = new File(out.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(partial)) {
            byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) > 0; ) o.write(buffer, 0, n);
        }
        if (!partial.renameTo(out)) throw new IOException("could not write " + out);
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    // --- the disc image ----------------------------------------------------------------------------------------------
    /** Shows the system's file picker (any folder, an SD card, a USB drive); the choice comes back through nativeDiscPicked. */
    public static void pickDisc() {
        final SDLActivity activity = mSingleton;
        if (activity == null) return;
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                activity.startActivityForResult(intent, REQUEST_DISC);
            } catch (Exception e) {
                Log.e(TAG, "no file picker", e);
                nativeDiscPicked("", "");
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_DISC) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
            nativeDiscPicked("", "");
            return;
        }
        Uri uri = data.getData();
        String name = "";
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) name = cursor.getString(0);
        } catch (Exception e) {
            // (a provider without a display name: the game names the image by its contents)
        }
        nativeDiscPicked(uri.toString(), name);
    }

    /** Opens a picked document for reading; the game owns the descriptor and closes it. -1 when it can't be opened. */
    public static int openDocument(String uri) {
        try {
            ParcelFileDescriptor descriptor = mSingleton.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return descriptor == null ? -1 : descriptor.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "could not open the picked document", e);
            return -1;
        }
    }

    // --- updates (runtime/src/updater.cpp) ------------------------------------------------------------------------------
    private static final String ACTION_INSTALL_STATUS = "io.github.gminustwo.otherm.INSTALL_STATUS";

    /** A GET over HTTPS (Android's own certificate store): the body, or null when the server could not be reached. */
    public static byte[] httpGet(String url, String accept, int[] status) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(20000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("Accept", accept);
            connection.setRequestProperty("User-Agent", "OtherMRecompiled");
            status[0] = connection.getResponseCode();
            InputStream in = status[0] >= 400 ? connection.getErrorStream() : connection.getInputStream();
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream s = in) {
                    byte[] buffer = new byte[16384];
                    for (int n; (n = s.read(buffer)) > 0; ) body.write(buffer, 0, n);
                }
            }
            return body.toByteArray();
        } catch (Exception e) {
            Log.w(TAG, "GET " + url + " failed: " + e);
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    /** Downloads `url` into `path` (written beside it first, renamed once complete). */
    public static boolean download(String url, String path) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(60000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("User-Agent", "OtherMRecompiled");
            if (connection.getResponseCode() != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "download " + url + ": HTTP " + connection.getResponseCode());
                return false;
            }
            try (InputStream in = connection.getInputStream()) {
                copy(in, new File(path));
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "download " + url + " failed: " + e);
            return false;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    /** Hands a downloaded APK to Android's package installer: it asks the player, then replaces the app and ends this
     *  process. The answer comes back through onNewIntent. */
    public static boolean installPackage(String path) {
        try {
            File apk = new File(path);
            PackageInstaller installer = mSingleton.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setSize(apk.length());
            int id = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                    byte[] buffer = new byte[1 << 16];
                    for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
                    session.fsync(out);
                }
                Intent intent = new Intent(mSingleton, OtherMActivity.class).setAction(ACTION_INSTALL_STATUS);
                PendingIntent pending = PendingIntent.getActivity(mSingleton, id, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                session.commit(pending.getIntentSender());
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "could not start the update's installation", e);
            return false;
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent == null || !ACTION_INSTALL_STATUS.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            // Android's own question (and, the first time, its setting to allow installing from this app)
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) startActivity(confirm);
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            Log.w(TAG, "the update was not installed: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        }
    }

    // --- restart and quit ----------------------------------------------------------------------------------------------
    /** Quit in the game's menus: the app's task closes, and onDestroy ends the process. */
    public static void quitApp() {
        final SDLActivity activity = mSingleton;
        if (activity != null) activity.runOnUiThread(activity::finishAndRemoveTask);
    }

    /** Options > Game > Reset game, an imported save: RestartActivity ends this process and starts the game again. */
    public static void restartApp() {
        Intent intent = new Intent(mSingleton, RestartActivity.class);
        intent.putExtra(RestartActivity.EXTRA_PID, Process.myPid());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        mSingleton.startActivity(intent);
    }
}
