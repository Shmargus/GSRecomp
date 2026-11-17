package io.github.shmargus.gsrecomp;

// Collection window. Runs in its own process (see GsrActivity).
public class LauncherActivity extends GsrActivity {
    @Override
    protected String part() { return "launcher"; }
}
