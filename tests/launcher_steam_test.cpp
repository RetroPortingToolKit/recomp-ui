#include "launcher_steam.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cctype>

namespace fs = std::filesystem;
using namespace launcher_steam;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> static void refuses(F f) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "operation should have refused");
}
static bool stopped() { return false; }
static bool active() { return true; }
static void environment(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1); else unsetenv(key);
#endif
}
static int calls;
static bool starting() { return ++calls > 1; }
static void put(const fs::path& p, const std::string& b) { std::ofstream f(p, std::ios::binary); f.write(b.data(), b.size()); check(bool(f), "fixture write"); }
static std::string get(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory argument required");
        const char* steam_keys[] = {"SteamAppId", "SteamGameId", "SteamOverlayGameId"};
        std::string previous[3]; bool existed[3]{};
        for (int i = 0; i < 3; ++i) {
            const char* value = std::getenv(steam_keys[i]);
            existed[i] = value != nullptr;
            if (value) previous[i] = value;
            environment(steam_keys[i], nullptr);
        }
        check(!launched_from_steam(), "ordinary launch detected as Steam");
        for (const char* key : steam_keys) {
            environment(key, "123456");
            check(launched_from_steam(), "Steam launch environment not detected");
            environment(key, nullptr);
        }
        for (int i = 0; i < 3; ++i) environment(steam_keys[i], existed[i] ? previous[i].c_str() : nullptr);
        const fs::path target = fs::absolute(argv[0]).lexically_normal();
        std::string b;
        check(append_shortcut(b, "F-Zero Forever", target), "empty library add");
        auto first = b;
        check(!append_shortcut(b, "User renamed title", target), "deduplicate by executable");
        check(b == first, "duplicate must not alter bytes");
        auto other = target.parent_path() / fs::path(u8"Other game $ ` & é");
        check(append_shortcut(b, "Another title", other), "second add");
        check(b.substr(0, first.size() - 2) == first.substr(0, first.size() - 2), "existing entry changed");
        check(b.find("Other game $ ` &") != std::string::npos, "path must be stored literally");
        for (size_t n = 1; n < first.size(); ++n) {
            auto truncated = first.substr(0, n);
            refuses([&] { append_shortcut(truncated, "Second", other); });
            check(truncated == first.substr(0, n), "invalid input mutated");
        }
        auto bad = first + "trailing garbage";
        refuses([&] { append_shortcut(bad, "Second", other); });
        refuses([&] { append_shortcut(b, "Bad", "relative/game"); });
        refuses([&] { append_shortcut(b, "Bad", target.parent_path() / "quote\"game"); });
        // Unknown keys and nested tags remain byte-exact, sparse indices work.
        auto custom = first;
        const auto index = custom.find(std::string("\0" "0\0", 3), 11);
        check(index != std::string::npos, "entry index");
        custom.replace(index + 1, 1, "7");
        // Build this explicitly to avoid fixture length/escape ambiguity.
        std::string field(1, '\1'); field += "FutureField"; field += '\0'; field += "kept"; field += '\0';
        custom.insert(custom.size() - 3, field);
        auto preserved = custom.substr(0, custom.size() - 2);
        check(append_shortcut(custom, "Second", other), "sparse append");
        check(custom.substr(0, preserved.size()) == preserved, "unknown data not preserved");

        const auto root = fs::absolute(argv[1]).lexically_normal();
        check(!fs::exists(root), "fixture path already exists; refusing to reuse it");
        fs::create_directories(root / "userdata/123/config");
        fs::create_directories(root / "userdata/456/config");
        fs::create_directories(root / "userdata/0/config");
        fs::create_directories(root / "userdata/not-an-account/config");
        fs::create_directories(root / "config");
        put(root / "config/loginusers.vdf", R"vdf("users" {
            // Synthetic Steam profiles, not real account data.
            "76561197960265851" { "AccountName" "racer_one" "PersonaName" "Racer \"One\"" "MostRecent" "1" }
            "76561197960266184" { "AccountName" "racer_two" "PersonaName" "Second racer" "MostRecent" "0" }
        })vdf");
        auto found = accounts_in({root, root / ".", root / "missing"});
        check(found.size() == 2, "account discovery/deduplication");
        check(found[0].label == "Racer \"One\"" && found[0].most_recent,
              "friendly display name, escaping, or last-used profile missing");
        check(found[1].label == "Second racer", "second friendly profile name missing");
#ifdef _WIN32
        auto alias = root.string();
        for (char& c : alias) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        check(accounts_in({root, fs::path(alias)}).size() == 2, "case aliases duplicated Steam profiles");
#endif
        const auto account = found.front();
        refuses([&] { add(account, "Game", target, active); });
        check(!fs::exists(account.shortcuts), "running Steam touched library");
        check(add(account, "Game", target, stopped), "file create");
        const auto original = get(account.shortcuts);
        check(!add(account, "Game", target, stopped), "file deduplication");
        check(get(account.shortcuts) == original, "duplicate rewrote library");
        check(!fs::exists(account.shortcuts.string() + ".recomp-ui.bak"), "duplicate created backup");
        // Use a real executable with a different path; the fixture needn't run.
        const auto copied = root / fs::path(u8"Game é with spaces.exe");
        fs::copy_file(target, copied);
        fs::permissions(copied, fs::status(target).permissions());
        const auto config = (root / "config.ini").string();
        check(!locally_added(copied, config.c_str()), "fresh folder has an added flag");
        check(remember_added(copied, config.c_str()), "could not save added flag");
        check(locally_added(copied, config.c_str()), "added flag not remembered");
        check(!locally_added(target, config.c_str()), "flag hid a different executable");
        calls = 0;
        refuses([&] { add(account, "Second", copied, starting); });
        check(get(account.shortcuts) == original, "Steam starting must block commit");
        check(add(account, "Second", copied, stopped), "file append");
        check(get(account.shortcuts.string() + ".recomp-ui.bak") == original, "backup differs");
        check(launch_target(target, nullptr) == target, "native path");
        const auto encoded = copied.u8string();
        const std::string image(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        check(launch_target(target, image.c_str()) == copied, "AppImage outer path");
        refuses([&] { launch_target(target, "relative.AppImage"); });
        put(account.shortcuts, "malformed");
        refuses([&] { add(account, "Game", target, stopped); });
        check(get(account.shortcuts) == "malformed", "malformed library overwritten");
        put(account.shortcuts, original);
        const auto temporary = fs::path(account.shortcuts.string() + ".recomp-ui.tmp");
        put(temporary, "unfinished operation");
        refuses([&] { add(account, "Second", copied, stopped); });
        check(get(account.shortcuts) == original && get(temporary) == "unfinished operation",
              "temporary-file conflict changed data");
        fs::remove(temporary);
#ifndef _WIN32
        const auto linked = found.back();
        fs::create_symlink(account.shortcuts, linked.shortcuts);
        refuses([&] { add(linked, "Second", copied, stopped); });
        check(fs::is_symlink(linked.shortcuts) && get(account.shortcuts) == original,
              "symbolic link was replaced");
#endif
        // Entirely test-owned root, created above and never shared with Steam.
        fs::remove_all(root);
        std::cout << "Steam shortcut tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
