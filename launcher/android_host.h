// Android storage, process and system services for the launcher.
//
// The collection, session and game run as separate activities and
// processes. Starting a child gives it a fresh process and full screen.
// The parent can read its exit code, as on Linux with waitpid().
//
// Game libraries live in private app storage so Android can load them.
// The bundled builder, translator and Clang run from the app's library
// folder. link_shipped_files() maps them to the paths the PC layout expects.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gsr_android {

namespace fs = std::filesystem;

using Environment = std::vector<std::pair<std::string, std::string>>;

// The collection root: <app files>/collection.
fs::path storage_root();
// The app's library folder (libmain.so, the engines, the programs).
fs::path native_library_dir();

// Unpack builder data, engine headers and the sysroot once per app version.
// Link the bundled programs into each game folder. On failure, return false
// and set *error.
bool prepare_collection(std::string* error);

// The visible folder on the phone's storage ("Golden Sun Recompiled"),
// where ROMs, Saves and Music live once the player allows file access, so
// their file manager and a PC over USB can reach them.
fs::path shared_root();
// Whether the player allowed it (All files access on Android 11+).
bool has_file_access();
// Opens the system page that allows it; the answer arrives when the app
// comes back to the front.
void ask_file_access();
// With file access, makes the collection's ROMs, Saves and Music folders
// links to the visible folder. First it copies in what the app already held:
//   - a file already in the visible folder wins, and
//   - the app's old folders are kept in local/.
// Without access, or on failure, nothing changes. Returns false and sets
// *error when a folder could not be moved.
bool link_shared_folders(std::string* error);
// Opens the visible folder in the phone's own file manager.
void open_shared_folder();

// Start one session or game at a time. Pass arguments without argv[0] and
// apply the environment changes. Redirect stdout and stderr to the output
// file, or use the session log if it is empty. False sets *error.
bool start_child(const char* kind, const std::vector<std::string>& args,
                 const Environment& set, const std::vector<std::string>& unset,
                 const std::string& output, std::string* error,
                 const std::string& cwd = {});
// Returns true once per finished child and stores its exit code in *code.
bool take_child_exit(int* code);
// Wait for the child and return its exit code while pumping SDL.
int wait_child();

// Show the system picker and copy the chosen file to `destination`.
// Return its path, or "" on cancellation or read failure.
std::string pick_file(const std::string& title, const fs::path& destination);

// While a game build runs: keep this process and its compilers at full speed
// when the player is in another app or the screen is off (BuildService.java).
// Any thread.
void keep_awake(bool on);

// Opens a web page in the phone's browser.
void open_url(const std::string& url);

// One HTTPS request. `body_file` empty: GET, else POST of that file. Returns
// the HTTP status (0: no connection) and the reply in *reply.
int http_request(const std::string& url, const std::vector<std::string>& headers,
                 const std::string& body_file, std::string* reply);

// The phone's make and Android version, for bug reports.
std::string system_name();

}  // namespace gsr_android
