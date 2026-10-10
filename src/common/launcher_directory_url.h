#pragma once

#include <algorithm>
#include <filesystem>
#include <exception>
#include <string>

/* Encode a resolved directory path without probing it. Keeping this lexical
 * step separate lets callers test UNC URLs without contacting a share. */
inline std::string launcher_directory_path_url(const std::filesystem::path& path) {
    std::string bytes = path.lexically_normal().u8string();
#if defined(_WIN32)
    // generic_u8string can collapse a UNC prefix on MinGW. Keep native UTF-8
    // and normalize separators ourselves before identifying the URL authority.
    const std::string native = path.u8string();
    const auto separator = [](char c) { return c == '/' || c == '\\'; };
    const bool unc = native.size() >= 2 && separator(native[0]) && separator(native[1]);
    std::replace(bytes.begin(), bytes.end(), '\\', '/');
    if (unc && bytes.compare(0, 2, "//") != 0)
        bytes.insert(0, !bytes.empty() && bytes[0] == '/' ? "/" : "//");
#endif
    std::string url;
    if (bytes.compare(0, 2, "//") == 0) url = "file:";  // UNC host/share
    else if (!bytes.empty() && bytes[0] == '/') url = "file://";
    else url = "file:///";                             // Windows drive
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        const bool drive_colon = i == 1 && c == ':' && bytes.size() > 2 && bytes[2] == '/';
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/' || drive_colon) {
            url.push_back(static_cast<char>(c));
        } else {
            url.push_back('%');
            url.push_back(hex[c >> 4]);
            url.push_back(hex[c & 15]);
        }
    }
    return url;
}

/* Turn a UTF-8 directory path into an escaped file URL for SDL_OpenURL.
 * Never interpolate paths into shell commands. File resources are excluded:
 * opening a directory is an explicit launcher action, not file execution. */
inline bool launcher_directory_url(const char* text, std::string& url,
                                    std::string& error) {
    namespace fs = std::filesystem;
    url.clear();
    error.clear();
    if (!text || !*text) {
        error = "No folder selected.";
        return false;
    }
    try {
        std::error_code ec;
        fs::path path = fs::absolute(fs::u8path(text), ec);
        if (ec || !fs::is_directory(path, ec)) {
            error = ec ? ec.message() : "The selected folder does not exist.";
            return false;
        }
        url = launcher_directory_path_url(path);
        return true;
    } catch (const std::exception& e) {
        url.clear();
        error = std::string("Invalid folder path: ") + e.what();
        return false;
    }
}
