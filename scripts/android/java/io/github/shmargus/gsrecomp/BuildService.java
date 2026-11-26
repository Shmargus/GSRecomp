package io.github.shmargus.gsrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

// Keeps a game build running at full speed while the player is in another
// app or the screen is off, as Termux does: a foreground service (Android
// does not freeze or slow its process) and a partial wake lock (the CPU
// stays on). Players reported builds crawling once they left the app.
// It must run in the process that started the build, whose children are the
// compilers: the launcher's own process here, the session's in
// SessionBuildService. GsrActivity.keepAwake starts and stops it.
public class BuildService extends Service {
    private static final String CHANNEL = "build";
    private static final int NOTIFICATION = 1;

    private PowerManager.WakeLock wakeLock;

    @Override
    public void onCreate() {
        super.onCreate();
        PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "gsrecomp:build");
        wakeLock.setReferenceCounted(false);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            manager.createNotificationChannel(new NotificationChannel(
                CHANNEL, "Game build", NotificationManager.IMPORTANCE_LOW));
            builder = new Notification.Builder(this, CHANNEL);
        } else {
            builder = new Notification.Builder(this);
        }
        Notification notification = builder
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
            .setContentText("Building the game. You can use other apps meanwhile.")
            .setOngoing(true)
            .build();
        // Every start, "stop" included, comes from startForegroundService, and
        // Android ends the process if one is not answered by startForeground,
        // so answer first and stop after: a build that ends at once cannot
        // stop the service before it went to the foreground.
        boolean on = intent != null && intent.getBooleanExtra("on", false);
        try {
            if (Build.VERSION.SDK_INT >= 34)
                startForeground(NOTIFICATION, notification,
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else
                startForeground(NOTIFICATION, notification);
        } catch (Exception e) {
            on = false;  // refused: the build runs as before, only slower
        }
        if (on) {
            wakeLock.acquire();
        } else {
            if (wakeLock.isHeld()) wakeLock.release();
            stopForeground(true);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (wakeLock.isHeld()) wakeLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
