package io.github.shmargus.gsrecomp;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// LauncherActivity, SessionActivity and GameActivity each run in their
// own process (AndroidManifest.xml.in). All start at SDL_main in
// libmain.so. The first argument selects the role. android_host.cpp calls
// these methods from native code.
public abstract class GsrActivity extends SDLActivity {
    private static final int REQUEST_CHILD = 1;
    private static final int REQUEST_PICK = 2;
    // If a child exits without reporting a code, use 128 + SIGKILL.
    private static final int KILLED = 137;

    static native void nativeChildExited(int code);
    static native void nativeFilePicked(String path);

    private String pickDestination;

    protected abstract String part();

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        String[] extra = getIntent().getStringArrayExtra("args");
        if (extra == null) extra = new String[0];
        String[] args = new String[extra.length + 1];
        args[0] = part();
        System.arraycopy(extra, 0, args, 1, extra.length);
        return args;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Each part runs once per process, like a program on a PC: the next
        // start gets a fresh process (the engine is not made to restart).
        if (isFinishing()) System.exit(0);
    }

    // ---- called from native code (android_host.cpp) ----

    public String nativeLibraryDir() {
        return getApplicationInfo().nativeLibraryDir;
    }

    public String appVersion() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName + " " + info.lastUpdateTime;
        } catch (Exception e) {
            return "unknown";
        }
    }

    public String systemName() {
        return Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE;
    }

    // Unpack payload.zip into `destination`. Return an error or an empty
    // string on success.
    public String unpackPayload(String destination) {
        File root = new File(destination);
        try (ZipInputStream zip = new ZipInputStream(getAssets().open("payload.zip"))) {
            byte[] buffer = new byte[1 << 16];
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                File out = new File(root, entry.getName());
                if (!out.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator))
                    return "bad path in payload: " + entry.getName();
                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buffer)) > 0) o.write(buffer, 0, n);
                }
            }
            return "";
        } catch (Exception e) {
            return e.toString();
        }
    }

    // Starts "session" or "game" in its own process; nativeChildExited
    // reports how it ended.
    public void startChild(final String kind, final String[] args) {
        runOnUiThread(new Runnable() {
            public void run() {
                Class<?> target = "game".equals(kind) ? GameActivity.class : SessionActivity.class;
                Intent intent = new Intent(GsrActivity.this, target);
                intent.putExtra("args", args);
                startActivityForResult(intent, REQUEST_CHILD);
            }
        });
    }

    // Request a display rate that matches the frame rate on Android 11+.
    // On resume, reapply it to the new surface.
    public void setFrameRate(float hz) {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            android.view.Surface surface = getNativeSurface();
            if (surface != null && surface.isValid())
                surface.setFrameRate(hz, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        } catch (Throwable ignored) {
        }
    }

    // Report the exit code before SDL_main returns and SDL closes the
    // activity. setResult can be called from any thread.
    public void setExitCode(int code) {
        Intent data = new Intent();
        data.putExtra("exit", code);
        setResult(RESULT_OK, data);
    }

    public void pickFile(final String title, final String destination) {
        runOnUiThread(new Runnable() {
            public void run() {
                pickDestination = destination;
                Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                pick.addCategory(Intent.CATEGORY_OPENABLE);
                pick.setType("*/*");
                // The caller labels the choice because the system picker has no title.
                startActivityForResult(pick, REQUEST_PICK);
            }
        });
    }

    public void openUrl(final String url) {
        runOnUiThread(new Runnable() {
            public void run() {
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
                } catch (Exception ignored) {
                }
            }
        });
    }

    // One HTTPS request on the caller's (native) thread. Returns
    // {status, reply}; status "0" when there was no connection.
    public String[] httpRequest(String url, String[] headers, String bodyFile) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(300000);
            for (String header : headers) {
                int colon = header.indexOf(':');
                if (colon > 0)
                    connection.setRequestProperty(header.substring(0, colon).trim(),
                                                  header.substring(colon + 1).trim());
            }
            if (bodyFile != null && !bodyFile.isEmpty()) {
                File body = new File(bodyFile);
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(body.length());
                try (InputStream in = new FileInputStream(body);
                     OutputStream out = connection.getOutputStream()) {
                    copy(in, out);
                }
            }
            int status = connection.getResponseCode();
            InputStream in = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            ByteArrayOutputStream reply = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream stream = in) {
                    copy(stream, reply);
                }
            }
            return new String[] { Integer.toString(status), reply.toString("UTF-8") };
        } catch (Exception e) {
            return new String[] { "0", e.toString() };
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    // ---- results ----

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == REQUEST_CHILD) {
            int code = data != null ? data.getIntExtra("exit", KILLED) : KILLED;
            nativeChildExited(code);
        } else if (request == REQUEST_PICK) {
            final Uri uri = (result == RESULT_OK && data != null) ? data.getData() : null;
            final String destination = pickDestination;
            new Thread(new Runnable() {
                public void run() {
                    nativeFilePicked(uri != null && copy(uri, destination) ? destination : "");
                }
            }).start();
        }
    }

    private boolean copy(Uri from, String to) {
        File target = new File(to);
        target.getParentFile().mkdirs();
        try (InputStream in = getContentResolver().openInputStream(from);
             OutputStream out = new FileOutputStream(target)) {
            if (in == null) return false;
            copy(in, out);
            return true;
        } catch (Exception e) {
            target.delete();
            return false;
        }
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buffer = new byte[1 << 16];
        int n;
        while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
    }
}
