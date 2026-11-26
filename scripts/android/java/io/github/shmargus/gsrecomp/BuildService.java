package io.github.shmargus.gsrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
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

    // This process's build is in the foreground: progress() may update the
    // notice. Never posts one after the build stopped the service.
    private static volatile boolean active;

    private PowerManager.WakeLock wakeLock;

    // The notification: the build's current step and a progress bar (a
    // moving bar while the step has no count). Tapping it opens the app.
    static Notification notice(Context context, String step, int done, int total) {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager manager = (NotificationManager) context.getSystemService(NOTIFICATION_SERVICE);
            manager.createNotificationChannel(new NotificationChannel(
                CHANNEL, "Game build", NotificationManager.IMPORTANCE_LOW));
            builder = new Notification.Builder(context, CHANNEL);
        } else {
            builder = new Notification.Builder(context);
        }
        builder.setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Building the game")
            .setContentText(step.isEmpty() ? "You can use other apps meanwhile." : step)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setProgress(total, Math.min(done, total), total <= 0);
        if (total > 0) builder.setSubText((int) (100L * Math.min(done, total) / total) + "%");
        Intent open = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (open != null)
            builder.setContentIntent(PendingIntent.getActivity(context, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        return builder.build();
    }

    // The build's progress, from GsrActivity.buildProgress. Any thread.
    static void progress(Context context, String step, int done, int total) {
        if (!active) return;
        NotificationManager manager = (NotificationManager) context.getSystemService(NOTIFICATION_SERVICE);
        manager.notify(NOTIFICATION, notice(context, step, done, total));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "gsrecomp:build");
        wakeLock.setReferenceCounted(false);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = notice(this, "", 0, 0);
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
        active = on;
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
        active = false;
        if (wakeLock.isHeld()) wakeLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
