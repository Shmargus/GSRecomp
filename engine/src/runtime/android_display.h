#pragma once
// Android only (android_display.cpp, built into gbarecomp_imgui on phones).

// Tells Android how many pictures a second this window shows (GsrActivity
// setFrameRate, Android 11+), so a 120 Hz screen can drop to that rate
// instead of refreshing at 120. Used by the game (host_window.cpp) and the
// collection launcher (collection_main.cpp).
void android_set_frame_rate(double hz);
