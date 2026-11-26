// JNI bridge from the launcher to GsrActivity.
#include "android_host.h"

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include "launcher_game.h"

extern char** environ;

namespace gsr_android {
namespace {

// ---- JNI: the activity's methods, called on the SDL thread ----

struct Jni {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    jclass type = activity ? env->GetObjectClass(activity) : nullptr;
    ~Jni() {
        if (type) env->DeleteLocalRef(type);
        if (activity) env->DeleteLocalRef(activity);
    }
    jmethodID method(const char* name, const char* signature) {
        return type ? env->GetMethodID(type, name, signature) : nullptr;
    }
    std::string text(jstring s) {
        if (!s) return {};
        const char* chars = env->GetStringUTFChars(s, nullptr);
        std::string out = chars ? chars : "";
        if (chars) env->ReleaseStringUTFChars(s, chars);
        env->DeleteLocalRef(s);
        return out;
    }
    jstring string(const std::string& s) { return env->NewStringUTF(s.c_str()); }
    jobjectArray strings(const std::vector<std::string>& list) {
        jclass string_type = env->FindClass("java/lang/String");
        jobjectArray array = env->NewObjectArray(static_cast<jsize>(list.size()), string_type, nullptr);
        for (jsize i = 0; i < static_cast<jsize>(list.size()); ++i) {
            jstring item = string(list[i]);
            env->SetObjectArrayElement(array, i, item);
            env->DeleteLocalRef(item);
        }
        env->DeleteLocalRef(string_type);
        return array;
    }
    std::string call_text(const char* name) {
        jmethodID m = method(name, "()Ljava/lang/String;");
        return m ? text(static_cast<jstring>(env->CallObjectMethod(activity, m))) : std::string();
    }
};

std::mutex g_mutex;
std::condition_variable g_changed;
bool g_child_running = false;
bool g_child_exited = false;
int g_child_code = 0;
bool g_pick_waiting = false;
std::string g_picked;

// Pump SDL while waiting for the other activity to finish.
template <typename Done>
void wait_pumping(Done done) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (g_changed.wait_for(lock, std::chrono::milliseconds(50), done)) return;
        }
        SDL_PumpEvents();
    }
}

void link(const fs::path& target, const fs::path& at) {
    std::error_code ec;
    fs::create_directories(at.parent_path(), ec);
    fs::remove(at, ec);
    fs::create_symlink(target, at, ec);
}

}  // namespace

fs::path storage_root() {
    const char* files = SDL_AndroidGetInternalStoragePath();
    return fs::path(files ? files : ".") / "collection";
}

fs::path native_library_dir() {
    static const fs::path dir = Jni().call_text("nativeLibraryDir");
    return dir;
}

bool prepare_collection(std::string* error) {
    const fs::path root = storage_root();
    const fs::path marker = root / "local" / "app-version.txt";
    const std::string version = Jni().call_text("appVersion");
    std::string unpacked;
    {
        std::ifstream in(marker);
        std::getline(in, unpacked);
    }
    std::error_code ec;
    // Before unpacking, which writes Music/README.txt. Not fatal: without
    // the visible folder the app keeps its own.
    std::string link_error;
    if (!link_shared_folders(&link_error))
        std::fprintf(stderr, "[files] %s\n", link_error.c_str());
    if (unpacked != version) {
        // Refresh the unpacked app files on upgrade. Saves, ROMs and built games
        // live elsewhere in the collection.
        fs::remove_all(root / "toolchain", ec);
        for (const auto& game : gsr_launcher::kGames) {
            fs::remove_all(root / "games" / game.stem / "builder" / "data", ec);
            fs::remove_all(root / "games" / game.stem / "builder" / "engine", ec);
        }
        fs::create_directories(root / "local", ec);
        Jni jni;
        jmethodID unpack = jni.method("unpackPayload", "(Ljava/lang/String;)Ljava/lang/String;");
        const std::string failure = unpack
            ? jni.text(static_cast<jstring>(jni.env->CallObjectMethod(
                  jni.activity, unpack, jni.string(root.string()))))
            : std::string("the app has no unpackPayload");
        if (!failure.empty()) {
            *error = "Could not unpack the app's files: " + failure;
            return false;
        }
        std::ofstream(marker) << version << "\n";
    }
    for (const char* folder : {"ROMs", "Saves", "Controls", "Music"})
        fs::create_directories(root / folder, ec);
    // The PC release's layout, with its programs in the app's library folder.
    const fs::path lib = native_library_dir();
    const char* builders[2] = {"libgsr_builder.so", "libtla_builder.so"};
    for (int i = 0; i < 2; ++i) {
        const auto& game = gsr_launcher::kGames[i];
        const fs::path dir = root / "games" / game.stem;
        link(lib / game.engine, dir / game.engine);
        link(lib / builders[i], dir / "builder" / "gsr_builder");
        link(lib / "libgba_recompile.so", dir / "builder" / "gba_recompile");
        link(root / "toolchain", dir / "builder" / "toolchain");
    }
    return true;
}

fs::path shared_root() {
    static const fs::path dir = Jni().call_text("sharedFolder");
    return dir;
}

bool has_file_access() {
    Jni jni;
    jmethodID has = jni.method("hasFileAccess", "()Z");
    return has && jni.env->CallBooleanMethod(jni.activity, has);
}

void ask_file_access() {
    Jni jni;
    if (jmethodID ask = jni.method("askFileAccess", "()V"))
        jni.env->CallVoidMethod(jni.activity, ask);
}

void open_shared_folder() {
    Jni jni;
    if (jmethodID open = jni.method("openSharedFolder", "()V"))
        jni.env->CallVoidMethod(jni.activity, open);
}

bool link_shared_folders(std::string* error) {
    const fs::path root = storage_root(), shared = shared_root();
    std::error_code ec;
    if (!has_file_access() || shared.empty()) {
        // Access taken away after linking: the links lead nowhere the app may
        // go, so the app's own empty folders come back (the visible folder
        // keeps the files; allowing access again relinks it).
        for (const char* folder : {"ROMs", "Saves", "Music"}) {
            if (!fs::is_symlink(fs::symlink_status(root / folder, ec))) continue;
            fs::remove(root / folder, ec);
            fs::create_directories(root / folder, ec);
        }
        return true;
    }
    for (const char* folder : {"ROMs", "Saves", "Music"}) {
        const fs::path in_app = root / folder, visible = shared / folder;
        fs::create_directories(visible, ec);
        if (ec) {
            *error = "Could not create " + visible.string() + ": " + ec.message();
            return false;
        }
        if (fs::is_symlink(fs::symlink_status(in_app, ec))) {
            if (fs::read_symlink(in_app, ec) == visible) continue;
            fs::remove(in_app, ec);
        } else if (fs::is_directory(in_app, ec)) {
            // The app's files go across first; a file of the same name
            // already in the visible folder is the player's and stays.
            // Copy, not rename: the two are different file systems.
            fs::copy(in_app, visible,
                     fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            if (ec) {
                *error = "Could not copy " + in_app.string() + " to " + visible.string() + ": " +
                         ec.message();
                return false;
            }
            fs::path kept = root / "local" / (std::string(folder) + " before shared folder");
            for (int n = 2; fs::exists(kept, ec); ++n)
                kept = root / "local" / (std::string(folder) + " before shared folder " + std::to_string(n));
            fs::rename(in_app, kept, ec);
            if (ec) {
                *error = "Could not move " + in_app.string() + " aside: " + ec.message();
                return false;
            }
        }
        fs::create_symlink(visible, in_app, ec);
        if (ec) {
            *error = "Could not link " + in_app.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool start_child(const char* kind, const std::vector<std::string>& args,
                 const Environment& set, const std::vector<std::string>& unset,
                 const std::string& output, std::string* error, const std::string& cwd) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_child_running) {
            *error = "Another part of the launcher is still running.";
            return false;
        }
        g_child_running = true;
        g_child_exited = false;
    }
    // Forward GSR_ and GBARECOMP_ settings, then apply this child's overrides.
    // android_main.cpp sets them before starting the child.
    std::vector<std::string> list;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string text = *entry;
        if (text.rfind("GSR_", 0) == 0 || text.rfind("GBARECOMP_", 0) == 0) {
            list.push_back("--env");
            list.push_back(text);
        }
    }
    for (const auto& [name, value] : set) {
        list.push_back("--env");
        list.push_back(name + "=" + value);
    }
    for (const std::string& name : unset) {
        list.push_back("--unset");
        list.push_back(name);
    }
    if (!output.empty()) {
        list.push_back("--output");
        list.push_back(output);
    }
    if (!cwd.empty()) {
        list.push_back("--cwd");
        list.push_back(cwd);
    }
    list.push_back("--");
    list.insert(list.end(), args.begin(), args.end());
    Jni jni;
    jmethodID start = jni.method("startChild", "(Ljava/lang/String;[Ljava/lang/String;)V");
    if (!start) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_child_running = false;
        *error = "The app cannot start its parts.";
        return false;
    }
    jstring name = jni.string(kind);
    jobjectArray array = jni.strings(list);
    jni.env->CallVoidMethod(jni.activity, start, name, array);
    jni.env->DeleteLocalRef(name);
    jni.env->DeleteLocalRef(array);
    return true;
}

bool take_child_exit(int* code) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_child_exited) return false;
    g_child_exited = false;
    *code = g_child_code;
    return true;
}

int wait_child() {
    wait_pumping([] { return g_child_exited; });
    int code = 0;
    take_child_exit(&code);
    return code;
}

std::string pick_file(const std::string& title, const fs::path& destination) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pick_waiting = true;
        g_picked.clear();
    }
    Jni jni;
    jmethodID pick = jni.method("pickFile", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!pick) return {};
    jstring t = jni.string(title), d = jni.string(destination.string());
    jni.env->CallVoidMethod(jni.activity, pick, t, d);
    jni.env->DeleteLocalRef(t);
    jni.env->DeleteLocalRef(d);
    wait_pumping([] { return !g_pick_waiting; });
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_picked;
}

void keep_awake(bool on) {
    Jni jni;
    if (jmethodID keep = jni.method("keepAwake", "(Z)V"))
        jni.env->CallVoidMethod(jni.activity, keep, static_cast<jboolean>(on));
}

void build_progress(const std::string& step, int done, int total) {
    Jni jni;
    jmethodID progress = jni.method("buildProgress", "(Ljava/lang/String;II)V");
    if (!progress) return;
    jstring s = jni.string(step);
    jni.env->CallVoidMethod(jni.activity, progress, s, static_cast<jint>(done), static_cast<jint>(total));
    jni.env->DeleteLocalRef(s);
}

void open_url(const std::string& url) {
    Jni jni;
    jmethodID open = jni.method("openUrl", "(Ljava/lang/String;)V");
    if (!open) return;
    jstring u = jni.string(url);
    jni.env->CallVoidMethod(jni.activity, open, u);
    jni.env->DeleteLocalRef(u);
}

int http_request(const std::string& url, const std::vector<std::string>& headers,
                 const std::string& body_file, std::string* reply) {
    // Any thread: attach it to Java for the call (SDL does on first use).
    Jni jni;
    jmethodID request = jni.method(
        "httpRequest", "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
    if (!request) return 0;
    jstring u = jni.string(url), b = jni.string(body_file);
    jobjectArray h = jni.strings(headers);
    auto result = static_cast<jobjectArray>(
        jni.env->CallObjectMethod(jni.activity, request, u, h, b));
    jni.env->DeleteLocalRef(u);
    jni.env->DeleteLocalRef(b);
    jni.env->DeleteLocalRef(h);
    if (!result || jni.env->GetArrayLength(result) != 2) return 0;
    const std::string status =
        jni.text(static_cast<jstring>(jni.env->GetObjectArrayElement(result, 0)));
    *reply = jni.text(static_cast<jstring>(jni.env->GetObjectArrayElement(result, 1)));
    jni.env->DeleteLocalRef(result);
    return std::atoi(status.c_str());
}

std::string system_name() { return Jni().call_text("systemName"); }

}  // namespace gsr_android

// ---- from Java (GsrActivity) ----

extern "C" JNIEXPORT void JNICALL
Java_io_github_shmargus_gsrecomp_GsrActivity_nativeChildExited(JNIEnv*, jclass, jint code) {
    std::lock_guard<std::mutex> lock(gsr_android::g_mutex);
    gsr_android::g_child_running = false;
    gsr_android::g_child_exited = true;
    gsr_android::g_child_code = code;
    gsr_android::g_changed.notify_all();
    // The launcher's main loop sleeps while a game plays.
    SDL_Event event{};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_shmargus_gsrecomp_GsrActivity_nativeFilePicked(JNIEnv* env, jclass, jstring path) {
    const char* chars = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    std::lock_guard<std::mutex> lock(gsr_android::g_mutex);
    gsr_android::g_picked = chars ? chars : "";
    gsr_android::g_pick_waiting = false;
    if (chars) env->ReleaseStringUTFChars(path, chars);
    gsr_android::g_changed.notify_all();
}
