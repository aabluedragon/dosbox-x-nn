package org.nukemnet.dosboxx;

import android.content.res.AssetManager;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

public class DosBoxActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    // Keep the activity locked to landscape no matter what window size
    // dosbox-x requests; a mid-run rotation invalidates SDL's window
    // framebuffer under the emulator core and crashes it.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(
                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected String[] getArguments() {
        File conf = new File(getFilesDir(), "dos/dosbox-android.conf");
        return new String[] { "-conf", conf.getAbsolutePath() };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            // SDL reads hints from the environment: keep the app landscape
            // and log SDL errors to logcat while we stabilize the port.
            android.system.Os.setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight", true);
            android.system.Os.setenv("SDL_LOGGING", "*=info", true);
        } catch (Exception e) { /* hints only */ }
        redirectNativeOutput();
        unpackAssets();
        super.onCreate(savedInstanceState);
    }

    // dosbox-x prints startup errors to stdout/stderr, which Android
    // discards; keep them in files so failures are diagnosable.
    private void redirectNativeOutput() {
        try {
            FileOutputStream out = new FileOutputStream(
                    new File(getFilesDir(), "stdout.txt"));
            FileOutputStream err = new FileOutputStream(
                    new File(getFilesDir(), "stderr.txt"));
            android.system.Os.dup2(out.getFD(), 1);
            android.system.Os.dup2(err.getFD(), 2);
        } catch (Exception e) {
            // non-fatal; we just lose the logs
        }
    }

    // Copies the bundled DOS environment out of the APK on first launch.
    // The marker file's presence means the copy is complete; bump the
    // asset payload's version file to force a re-extract on upgrade.
    private void unpackAssets() {
        File marker = new File(getFilesDir(), "dos/.unpacked-v1");
        if (marker.exists())
            return;
        try {
            copyAssetDir("dos", new File(getFilesDir(), "dos"));
            marker.createNewFile();
        } catch (Exception e) {
            throw new RuntimeException("asset unpack failed", e);
        }
    }

    private void copyAssetDir(String path, File dest) throws Exception {
        AssetManager am = getAssets();
        String[] entries = am.list(path);
        if (entries == null || entries.length == 0) {
            copyAssetFile(path, dest);
            return;
        }
        dest.mkdirs();
        for (String entry : entries)
            copyAssetDir(path + "/" + entry, new File(dest, entry));
    }

    private void copyAssetFile(String path, File dest) throws Exception {
        try (InputStream in = getAssets().open(path);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0)
                out.write(buf, 0, n);
        }
    }
}
