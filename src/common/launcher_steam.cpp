#include "launcher_steam.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace launcher_steam {
namespace fs = std::filesystem;
namespace {
constexpr size_t max_file = 16 * 1024 * 1024;
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
std::string utf8(const fs::path& p, bool generic = false) {
    auto s = generic ? p.generic_u8string() : p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
fs::path from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}
std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("Could not read Steam's shortcut file.");
    if (fs::file_size(path) > max_file) fail("Steam's shortcut file is too large to edit safely.");
    std::string bytes{std::istreambuf_iterator<char>(f), {}};
    if (f.bad()) fail("Could not finish reading Steam's shortcut file.");
    return bytes;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool numeric(const std::string& s) {
    return !s.empty() && s.size() <= 10 &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::string zstring(const std::string& b, size_t& pos) {
    const auto end = b.find('\0', pos);
    if (end == std::string::npos) fail("Steam's shortcut file is incomplete; it was left unchanged.");
    std::string s = b.substr(pos, end - pos);
    pos = end + 1;
    return s;
}
/* Parse only to locate the end and inspect direct Exe fields. Existing data,
 * including unknown keys, tags, IDs and artwork links, is never reserialized.
 * Binary KeyValues types follow Valve's tier1/KeyValues.cpp. */
void object(const std::string& b, size_t& p, int depth, std::string* exe = nullptr) {
    if (depth > 32) fail("Steam's shortcut file is nested too deeply.");
    while (p < b.size()) {
        unsigned type = static_cast<unsigned char>(b[p++]);
        if (type == 8) return;
        const auto key = lower(zstring(b, p));
        if (type == 0) { object(b, p, depth + 1); continue; }
        if (type == 1) {
            auto value = zstring(b, p);
            if (exe && key == "exe") *exe = value;
            continue;
        }
        size_t count = 0;
        if (type == 2 || type == 3 || type == 4 || type == 6) count = 4;
        else if (type == 7) count = 8;
        else fail("Steam's shortcut file uses an unsupported type; it was left unchanged.");
        if (count > b.size() - p) fail("Steam's shortcut file is incomplete; it was left unchanged.");
        p += count;
    }
    fail("Steam's shortcut file is incomplete; it was left unchanged.");
}
void string_field(std::string& b, const char* key, const std::string& value) {
    b += '\1'; b += key; b += '\0'; b += value; b += '\0';
}
void int_field(std::string& b, const char* key, uint32_t value) {
    b += '\2'; b += key; b += '\0';
    for (unsigned i = 0; i < 4; ++i) b += static_cast<char>(value >> (8 * i));
}
uint32_t appid(const std::string& s) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return (~crc) | 0x80000000u;
}
bool same_target(std::string exe, const fs::path& target) {
    if (exe.size() >= 2 && exe.front() == '"' && exe.back() == '"')
        exe = exe.substr(1, exe.size() - 2);
    auto a = utf8(from_utf8(exe).lexically_normal(), true);
    auto b = utf8(target.lexically_normal(), true);
#ifdef _WIN32
    a = lower(a); b = lower(b);
#endif
    return a == b;
}
/* Flush a newly created sibling, then atomically replace the live file. Never
 * truncate it in place. A fixed backup retains the first pre-change version. */
void replace(const fs::path& path, const std::string& bytes) {
    fs::path tmp = path; tmp += ".recomp-ui.tmp";
#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) fail("Could not create the temporary shortcut file. Check permissions or an unfinished add.");
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) fail("Could not create the temporary shortcut file. Check permissions or an unfinished add.");
    size_t n = 0;
    while (n < bytes.size()) {
        auto wrote = write(fd, bytes.data() + n, bytes.size() - n);
        if (wrote <= 0) break;
        n += static_cast<size_t>(wrote);
    }
    bool ok = n == bytes.size() && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!ok) {
        std::error_code ignored; fs::remove(tmp, ignored);
        fail("Could not save the Steam shortcut. The original file was preserved.");
    }
}
} // namespace

bool append_shortcut(std::string& bytes, const std::string& name, const fs::path& target) {
    const auto path = utf8(target);
    if (name.empty() || name.find('\0') != std::string::npos ||
        !target.is_absolute() || path.find_first_of("\"\r\n") != std::string::npos ||
        path.find('\0') != std::string::npos)
        fail("This game needs an absolute launch path without quotes or line breaks.");
    std::string source = bytes;
    if (source.empty()) source = std::string("\0shortcuts\0\10\10", 13);
    size_t p = 0;
    if (source[p++] != 0 || zstring(source, p) != "shortcuts")
        fail("Steam's shortcut file has an unexpected root; it was left unchanged.");
    std::set<unsigned long> indices;
    bool present = false;
    while (p < source.size() && source[p] != 8) {
        if (source[p++] != 0) fail("Steam's shortcut list is invalid; it was left unchanged.");
        auto index = zstring(source, p);
        if (!numeric(index) || std::stoull(index) > 1000000)
            fail("Steam's shortcut indices are invalid; it was left unchanged.");
        if (!indices.insert(std::stoul(index)).second)
            fail("Steam's shortcut indices are duplicated; it was left unchanged.");
        std::string exe;
        object(source, p, 0, &exe);
        if (!exe.empty() && same_target(exe, target)) present = true;
    }
    const size_t insertion = p;
    if (p + 2 != source.size() || source[p] != 8 || source[p + 1] != 8)
        fail("Steam's shortcut file has an invalid ending; it was left unchanged.");
    if (present) return false;
    unsigned long index = 0;
    while (indices.count(index)) ++index;
    std::string entry(1, '\0'); entry += std::to_string(index); entry += '\0';
    const auto quoted = "\"" + path + "\"";
    int_field(entry, "appid", appid(quoted + name));
    string_field(entry, "AppName", name);
    string_field(entry, "Exe", quoted);
    string_field(entry, "StartDir", "\"" + utf8(target.parent_path()) + "\"");
    string_field(entry, "icon", "");
    string_field(entry, "ShortcutPath", "");
    string_field(entry, "LaunchOptions", "");
    int_field(entry, "IsHidden", 0);
    int_field(entry, "AllowDesktopConfig", 1);
    int_field(entry, "AllowOverlay", 1);
    int_field(entry, "OpenVR", 0);
    int_field(entry, "Devkit", 0);
    string_field(entry, "DevkitGameID", "");
    int_field(entry, "DevkitOverrideAppID", 0);
    int_field(entry, "LastPlayTime", 0);
    string_field(entry, "FlatpakAppID", "");
    entry += std::string("\0tags\0\10\10", 8);
    source.insert(insertion, entry);
    bytes.swap(source);
    return true;
}

namespace {
struct Login { std::string name, login; bool recent = false; };
/* loginusers.vdf is a small text KeyValues file. Read quoted/escaped UTF-8
 * names, comments and nested blocks without assuming display names are IDs. */
class LoginReader {
    const std::string& data;
    size_t pos = 0;
public:
    explicit LoginReader(const std::string& s) : data(s) {
        if (s.compare(0, 3, "\xef\xbb\xbf") == 0) pos = 3;
    }
    std::string token() {
        for (;;) {
            while (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) ++pos;
            if (data.compare(pos, 2, "//") != 0) break;
            pos = data.find('\n', pos);
            if (pos == std::string::npos) { pos = data.size(); return {}; }
        }
        if (pos == data.size()) return {};
        char c = data[pos++];
        if (c == '{' || c == '}') return std::string(1, c);
        std::string out;
        if (c != '"') {
            out += c;
            while (pos < data.size() && !std::isspace(static_cast<unsigned char>(data[pos])) &&
                   data[pos] != '{' && data[pos] != '}') out += data[pos++];
            return out;
        }
        while (pos < data.size()) {
            c = data[pos++];
            if (c == '"') return out;
            if (c == '\\' && pos < data.size()) {
                c = data[pos++];
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c != '\\' && c != '"') out += '\\';
            }
            out += c;
        }
        fail("Incomplete Steam profile names file.");
    }
    void skip_block(int depth = 0) {
        if (depth > 32) fail("Steam profile names are nested too deeply.");
        for (;;) {
            auto t = token();
            if (t.empty()) fail("Incomplete Steam profile names file.");
            if (t == "}") return;
            if (t == "{") skip_block(depth + 1);
        }
    }
};

std::map<std::string, Login> login_names(const fs::path& root) {
    std::map<std::string, Login> names;
    try {
        auto text = read(root / "config/loginusers.vdf");
        LoginReader r(text);
        if (lower(r.token()) != "users" || r.token() != "{") return names;
        for (;;) {
            auto id = r.token();
            if (id.empty() || id == "}") break;
            if (r.token() != "{") break;
            Login profile;
            for (;;) {
                auto key = lower(r.token());
                if (key == "}") break;
                if (key.empty()) fail("Incomplete Steam profile names file.");
                auto value = r.token();
                if (value == "{") { r.skip_block(); continue; }
                if (key == "personaname") profile.name = value;
                else if (key == "accountname") profile.login = value;
                else if (key == "mostrecent") profile.recent = value == "1";
            }
            if (profile.name.empty()) profile.name = profile.login;
            if (profile.name.empty() || id.size() > 20 ||
                !std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
            names[std::to_string(static_cast<uint32_t>(std::stoull(id)))] = std::move(profile);
        }
    } catch (...) { /* Unidentifiable profiles are not offered as opaque IDs. */ }
    return names;
}
}

std::vector<Account> accounts_in(const std::vector<fs::path>& roots) {
    std::vector<Account> result;
    std::vector<fs::path> seen;
    for (const auto& root : roots) {
        std::error_code ec;
        auto userdata = fs::weakly_canonical(root / "userdata", ec);
        if (ec || !fs::is_directory(userdata, ec)) continue;
        bool duplicate = false;
        for (const auto& prior : seen) {
            if (fs::equivalent(userdata, prior, ec)) { duplicate = true; break; }
        }
        if (duplicate) continue;
        seen.push_back(userdata);
        const auto names = login_names(root);
        for (const auto& dir : fs::directory_iterator(userdata)) {
            auto id = utf8(dir.path().filename());
            if (id == "0" || !numeric(id) || !dir.is_directory()) continue;
            auto profile = names.find(std::to_string(std::stoull(id)));
            if (profile == names.end()) continue;
            result.push_back({profile->second.name +
                              (utf8(root).find("com.valvesoftware.Steam") != std::string::npos ? " (Flatpak)" : ""),
                              dir.path() / "config" / "shortcuts.vdf",
                              profile->second.recent, profile->second.login});
        }
    }
    std::map<std::string, size_t> label_counts;
    for (const auto& account : result) ++label_counts[account.label];
    for (auto& account : result) {
        if (label_counts[account.label] > 1 && !account.login_name.empty())
            account.label += " (" + account.login_name + ")";
    }
    std::sort(result.begin(), result.end(), [](const Account& a, const Account& b) { return a.label < b.label; });
    return result;
}

std::vector<Account> accounts() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    wchar_t value[32768]; DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
        roots.emplace_back(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        size = sizeof(value);
        if (RegGetValueW(key, nullptr, L"InstallPath", RRF_RT_REG_SZ,
                         nullptr, value, &size) == ERROR_SUCCESS) roots.emplace_back(value);
        RegCloseKey(key);
    }
#elif defined(__linux__)
    if (const char* home = std::getenv("HOME")) {
        auto base = from_utf8(home);
        roots = {base / ".steam/steam", base / ".steam/root", base / ".local/share/Steam",
                 base / ".var/app/com.valvesoftware.Steam/.local/share/Steam"};
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) roots.push_back(from_utf8(xdg) / "Steam");
#endif
    return accounts_in(roots);
}

fs::path launch_target(const fs::path& native, const char* appimage) {
    auto path = appimage && *appimage ? from_utf8(appimage) : native;
    if (!path.is_absolute() || !fs::is_regular_file(path))
        fail("The installed game executable could not be found. Extract the game to its permanent folder first.");
#ifndef _WIN32
    if (access(path.c_str(), X_OK) != 0) fail("The game file is not executable. Enable its executable permission first.");
    if (path.string().find("/tmp/.mount_") != std::string::npos)
        fail("The AppImage's permanent path is unavailable. Launch the AppImage normally and try again.");
#endif
    return path.lexically_normal();
}

fs::path executable() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) fail("The game executable path is unavailable.");
    path.resize(count);
    return launch_target(fs::path(path), nullptr);
#elif defined(__linux__)
    return launch_target(fs::read_symlink("/proc/self/exe"), std::getenv("APPIMAGE"));
#else
    fail("Add to Steam is supported on Windows and Linux.");
#endif
}

bool launched_from_steam() {
    for (const char* key : {"SteamAppId", "SteamGameId", "SteamOverlayGameId"}) {
        const char* value = std::getenv(key);
        if (value && *value) return true;
    }
    return false;
}

std::string protocol_url(const fs::path& target) {
#ifdef _WIN32
    (void)target;
    // The desktop client's current Add Game menu uses this route. Its handler
    // opens Steam's chooser and does not accept a supplied executable path.
    return "steam://open/addnonsteamgame";
#else
    static const char hex[] = "0123456789ABCDEF";
    std::string url = "steam://addnonsteamgame/";
    for (unsigned char c : utf8(target)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            url += static_cast<char>(c);
        else { url += '%'; url += hex[c >> 4]; url += hex[c & 15]; }
    }
    return url;
#endif
}

std::string prepare_protocol_request(const std::string& name, const fs::path& target,
                                     const char* config_path) {
    (void)launch_target(target, nullptr);
#if defined(__linux__)
    auto escape = [](const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };
    std::string argument = "\"";
    for (char c : utf8(target)) {
        if (c == '\\' || c == '"' || c == '`' || c == '$') argument += '\\';
        if (c == '%') argument += '%';
        argument += c;
    }
    argument += '"';
    const auto directory = fs::absolute(from_utf8(config_path && *config_path ? config_path : "config.ini")).parent_path();
    const auto desktop = directory / (".recomp-ui-steam-" + std::to_string(appid(utf8(target) + name)) + ".desktop");
    std::ofstream file(desktop, std::ios::binary);
    file << "[Desktop Entry]\nType=Application\nName=" << escape(name)
         << "\nExec=" << escape(argument) << "\nPath=" << escape(utf8(target.parent_path()))
         << "\nTerminal=false\n";
    file.flush();
    if (!file) fail("Could not prepare the Steam launcher file.");
    // Steam for Linux uses this sentinel to import the encoded file directly.
    const auto sentinel = fs::temp_directory_path() / "addnonsteamgamefile";
    int fd = open(sentinel.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) fail("Could not prepare Steam's import request.");
    close(fd);
    return protocol_url(desktop);
#else
    (void)name; (void)config_path;
#ifdef _WIN32
    DWORD size = 0;
    if (RegGetValueW(HKEY_CLASSES_ROOT, L"steam\\shell\\open\\command", nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, nullptr, &size) != ERROR_SUCCESS)
        fail("Steam is not registered on this PC. Install or open Steam, then try again.");
#endif
    return protocol_url(target);
#endif
}

static fs::path marker_path(const char* config_path) {
    return fs::absolute(from_utf8(config_path && *config_path ? config_path : "config.ini"))
        .parent_path() / ".recomp-ui-steam-added";
}

bool locally_added(const fs::path& target, const char* config_path) {
    std::ifstream f(marker_path(config_path), std::ios::binary);
    std::string stored;
    return std::getline(f, stored) && same_target(stored, target);
}

bool remember_added(const fs::path& target, const char* config_path) {
    const auto path = marker_path(config_path);
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << utf8(target) << '\n';
    f.flush();
    return bool(f);
}

bool running() {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) fail("Could not check whether Steam is closed.");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    if (!Process32FirstW(snapshot, &entry)) { CloseHandle(snapshot); fail("Could not inspect running processes."); }
    do {
        if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) { found = true; break; }
    } while (Process32NextW(snapshot, &entry));
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    if (!found && error != ERROR_NO_MORE_FILES) fail("Could not finish checking running processes.");
    return found;
#elif defined(__linux__)
    for (const auto& dir : fs::directory_iterator("/proc")) {
        if (!numeric(dir.path().filename().string())) continue;
        std::ifstream f(dir.path() / "comm");
        std::string name; std::getline(f, name);
        if (name == "steam" || name == "steam.exe") return true;
    }
    return false;
#else
    fail("Add to Steam is supported on Windows and Linux.");
#endif
}

bool add(const Account& account, const std::string& name, const fs::path& target,
         bool (*client_running)()) {
    if (client_running()) fail("In Steam, choose Steam > Exit. Then click Add to Steam below.");
    (void)launch_target(target, nullptr);
    const auto& path = account.shortcuts;
    fs::create_directories(path.parent_path());
    if (fs::is_symlink(fs::symlink_status(path)))
        fail("This Steam shortcut file is a symbolic link. Add the game through Steam to preserve that setup.");
    fs::path lock = path; lock += ".recomp-ui-lock";
    if (!fs::create_directory(lock)) fail("Another Add to Steam operation is active. Try again when it finishes.");
    struct Unlock { fs::path path; ~Unlock() { std::error_code ec; fs::remove(path, ec); } } unlock{lock};
    const bool existed = fs::exists(path);
    const auto original = existed ? read(path) : std::string();
    auto bytes = original;
    if (!append_shortcut(bytes, name, target)) return false;
    fs::path backup = path; backup += ".recomp-ui.bak";
    if (existed && !fs::exists(backup)) fs::copy_file(path, backup);
    if (client_running() || fs::exists(path) != existed || (existed && read(path) != original))
        fail("Steam or its shortcut file changed. Exit Steam and try again.");
    replace(path, bytes);
    return true;
}
} // namespace launcher_steam
