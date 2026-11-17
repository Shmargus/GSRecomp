package io.github.shmargus.gsrecomp;

// Game setup and build. Runs in its own process (see GsrActivity).
public class SessionActivity extends GsrActivity {
    @Override
    protected String part() { return "session"; }
}
