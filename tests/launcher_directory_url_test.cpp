#include "launcher_directory_url.h"

#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "recomp-ui-directory-url-test";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return 2;
    const fs::path folder = root / fs::u8path(u8"folder space # % \u65e5\u672c");
    fs::create_directories(folder, ec);
    if (ec) return 2;
    std::string url, error;
    int failures = 0;
    const auto check = [&](bool value, const char* what) {
        if (!value) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
    };
    const std::string path = folder.u8string();
    check(launcher_directory_url(path.c_str(), url, error), "Unicode folder opens");
    check(url.find("folder%20space%20%23%20%25%20%E6%97%A5%E6%9C%AC") != std::string::npos,
          "UTF-8, space, hash and percent bytes are escaped");
    check(url.compare(0, 8, "file:///") == 0, "absolute local file URL");
#if defined(_WIN32)
    check(url.size() > 10 && url[9] == ':', "Windows drive colon is preserved");
    // Exercise the production encoder, without contacting any network share.
    check(launcher_directory_path_url(fs::u8path("//server/share/folder")) ==
              "file://server/share/folder", "forward-slash UNC authority is preserved");
    check(launcher_directory_path_url(fs::u8path("\\\\server\\share\\folder")) ==
              "file://server/share/folder", "backslash UNC authority is preserved");
    const char* escaped_unc = "file://server/share/folder%20space%20%23%20%25%20%E6%97%A5%E6%9C%AC";
    check(launcher_directory_path_url(fs::u8path(u8"//server/share/folder space # % \u65e5\u672c")) ==
              escaped_unc, "forward-slash UNC Unicode path is escaped");
    check(launcher_directory_path_url(fs::u8path(u8"\\\\server\\share\\folder space # % \u65e5\u672c")) ==
              escaped_unc, "backslash UNC Unicode path is escaped");
    check(launcher_directory_path_url(fs::u8path("//server/share/old/../folder")) ==
              "file://server/share/folder", "UNC authority survives lexical normalization");
#endif
    const fs::path original = fs::current_path();
    fs::current_path(root);
    const std::string relative = folder.filename().u8string();
    check(launcher_directory_url(relative.c_str(), url, error), "relative directory is resolved");
    check(url.compare(0, 8, "file:///") == 0, "relative path produces absolute file URL");
    fs::current_path(original);
    const fs::path file = root / "file.txt";
    std::ofstream(file) << "test";
    check(!launcher_directory_url(file.u8string().c_str(), url, error) && !error.empty(),
          "plain file is refused");
    check(!launcher_directory_url((root / "missing").u8string().c_str(), url, error) && !error.empty(),
          "missing directory is refused");
    check(!launcher_directory_url(nullptr, url, error) && !error.empty(), "empty input is refused");
    check(!launcher_directory_url("\xff", url, error), "invalid UTF-8 is handled without throwing");
    fs::remove_all(root, ec);
    if (!failures) std::cout << "launcher directory URL tests passed\n";
    return failures ? 1 : 0;
}
