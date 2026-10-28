// file_replace.cpp — see file_replace.h.

#include "file_replace.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace gba {

namespace fs = std::filesystem;

bool replace_file(const fs::path& path, const void* data, std::size_t len,
                  std::string* err) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            if (err) *err = "could not create directory " +
                            path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "could not open for write " + tmp.string();
            return false;
        }
        if (len) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
        f.close();  // close() flushes; a late write error shows up here
        if (!f) {
            if (err) *err = "could not write " + tmp.string();
            fs::remove(tmp, ec);
            return false;
        }
    }

#ifdef _WIN32
    const bool replaced =
        MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    const std::string why = replaced ? std::string()
        : std::system_category().message(static_cast<int>(GetLastError()));
#else
    const bool replaced = std::rename(tmp.c_str(), path.c_str()) == 0;
    const std::string why = replaced ? std::string()
        : std::generic_category().message(errno);
#endif
    if (!replaced) {
        // The previous file is untouched; keep the complete new copy too.
        if (err) *err = "could not replace " + path.string() + " (" + why +
                        "); the new copy is kept as " + tmp.string();
        return false;
    }
    return true;
}

}  // namespace gba
