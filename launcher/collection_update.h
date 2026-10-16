// One shared release check for the collection. The worker never touches UI
// objects and owns its state even if the player closes the selector.
#pragma once

#include "launcher_online.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace gsr_collection {
namespace update_detail {

// Reply size limit (4 MiB) and request timeout (15 seconds).
constexpr std::size_t kMaxReply = 4 * 1024 * 1024;
constexpr int kTimeoutMs = 15000;

#ifdef _WIN32
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

inline std::wstring ascii_wide(const char* text) {
    return std::wstring(text, text + std::strlen(text));
}

inline bool request_releases(std::string& reply) {
    HttpHandle session(WinHttpOpen(ascii_wide(gsr_online::kUserAgent).c_str(),
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs,
                                             kTimeoutMs, kTimeoutMs)) return false;
    HttpHandle connection(WinHttpConnect(session.value,
                                        ascii_wide(gsr_online::kReleasesApiHost).c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return false;
    HttpHandle request(WinHttpOpenRequest(connection.value, L"GET",
                                         ascii_wide(gsr_online::kReleasesApiPath).c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return false;
    // Follow HTTPS redirects only; never downgrade an API request to HTTP.
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirects, sizeof(redirects))) return false;
    const auto started = std::chrono::steady_clock::now();
    if (!WinHttpSendRequest(request.value, L"Accept: application/vnd.github+json\r\n",
                            static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
    char chunk[4096];
    for (;;) {
        if (std::chrono::steady_clock::now() - started >=
            std::chrono::milliseconds(kTimeoutMs)) return false;
        DWORD count = 0;
        if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &count)) return false;
        if (count == 0) return true;
        if (count > kMaxReply - reply.size()) return false;
        reply.append(chunk, count);
    }
}
#else
inline bool request_releases(std::string& reply) {
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    const bool actions_ok =
        posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_addclose(&actions, descriptors[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, descriptors[1]) == 0;
    std::vector<std::string> args = {
        "curl", "-sS", "-f", "-L", "-m", "15", "--proto", "=https",
        "--proto-redir", "=https", "-H", std::string("User-Agent: ") + gsr_online::kUserAgent,
        "-H", "Accept: application/vnd.github+json",
        std::string("https://") + gsr_online::kReleasesApiHost + gsr_online::kReleasesApiPath};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = actions_ok ? posix_spawnp(&pid, argv[0], &actions, nullptr,
                                                 argv.data(), environ) : -1;
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (spawned != 0) {
        close(descriptors[0]);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    bool ok = true;
    char chunk[4096];
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { ok = false; break; }
        pollfd descriptor{descriptors[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL))) { ok = false; break; }
        const ssize_t count = read(descriptors[0], chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > kMaxReply - reply.size()) { ok = false; break; }
        reply.append(chunk, static_cast<std::size_t>(count));
    }
    close(descriptors[0]);
    if (!ok) kill(pid, SIGKILL);
    int status = 0;
    pid_t waited = 0;
    while (ok && waited == 0) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited < 0 && errno == EINTR) { waited = 0; continue; }
        if (waited != 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            ok = false;
            kill(pid, SIGKILL);
            break;
        }
        std::this_thread::yield();
    }
    if (!ok) {
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    return ok && waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

} // namespace update_detail

class ReleaseCheck {
    struct State {
        std::atomic<bool> finished{false};
        std::mutex mutex;
        gsr_online::Release release;
        bool available = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    explicit ReleaseCheck(bool enabled = true) {
        if (!enabled || !gsr_online::update_checks_enabled()) {
            state_->finished = true;
            return;
        }
        const auto state = state_;
        try {
            std::thread([state] {
                struct MarkDone {
                    std::shared_ptr<State> state;
                    ~MarkDone() { state->finished = true; }
                } done{state};
                try {
                    std::string reply;
                    if (!update_detail::request_releases(reply)) return;
                    gsr_online::Release release;
                    std::string error;
#ifdef _WIN32
                    constexpr bool linux_build = false;
#else
                    constexpr bool linux_build = true;
#endif
                    if (!gsr_online::parse_newest_release(reply, linux_build, &release, &error) ||
                        !gsr_online::is_newer_release(release)) return;
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->release = std::move(release);
                    state->available = true;
                } catch (...) {
                    // Offline, invalid replies and resource failures never stop play.
                }
            }).detach();
        } catch (...) {
            state_->finished = true;
        }
    }

    bool done() const { return state_->finished.load(); }

    bool take(gsr_online::Release& output) {
        if (!done()) return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->available) return false;
        output = std::move(state_->release);
        state_->available = false;
        return true;
    }
};

// The newest release's notes for the What's New window, fetched once on the
// first request (any build, developer ones included: it only reads).
class ReleaseNotes {
public:
    enum class Status { Idle, Loading, Ready, Failed };

private:
    struct State {
        std::atomic<Status> status{Status::Idle};
        std::mutex mutex;
        std::string tag, text;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    void request() {
        if (state_->status != Status::Idle && state_->status != Status::Failed) return;
        state_->status = Status::Loading;
        const auto state = state_;
        try {
            std::thread([state] {
                Status result = Status::Failed;
                try {
                    std::string reply, error;
                    gsr_online::Json root;
                    gsr_online::Release release;
                    if (update_detail::request_releases(reply) &&
                        gsr_online::read_newest_release(reply, &root, &release, &error)) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->tag = release.tag;
                        state->text = gsr_online::notes_to_plain_text(release.notes);
                        result = Status::Ready;
                    }
                } catch (...) {
                    // Offline or an unreadable reply: the window says so.
                }
                state->status = result;
            }).detach();
        } catch (...) {
            state_->status = Status::Failed;
        }
    }

    Status status() const { return state_->status.load(); }

    // Only meaningful once status() is Ready.
    void read(std::string& tag, std::string& text) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        tag = state_->tag;
        text = state_->text;
    }
};

} // namespace gsr_collection
