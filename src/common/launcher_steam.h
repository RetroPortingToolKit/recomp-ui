#pragma once

#include <filesystem>
#include <string>
#include <vector>

/* Native URL handoff without a Steam SDK, shell commands, or client automation.
 * The legacy file-writing helpers below require a stopped client and are not
 * used by the launcher action. */
namespace launcher_steam {
struct Account {
    std::string label;
    std::filesystem::path shortcuts;
    bool most_recent = false;
    std::string login_name;
};
std::vector<Account> accounts();
std::vector<Account> accounts_in(const std::vector<std::filesystem::path>& roots);
std::filesystem::path executable();
std::filesystem::path launch_target(const std::filesystem::path& native,
                                  const char* appimage);
bool running(); /* Throws if the process list cannot be inspected. */
bool launched_from_steam();
bool locally_added(const std::filesystem::path& target, const char* config_path);
bool remember_added(const std::filesystem::path& target, const char* config_path);
std::string protocol_url(const std::filesystem::path& target);
std::string prepare_protocol_request(const std::string& name,
    const std::filesystem::path& target, const char* config_path);
/* Throws on invalid/unsupported input; retains all original entries byte for
 * byte. Returns false when this target is already present. */
bool append_shortcut(std::string& bytes, const std::string& name,
                     const std::filesystem::path& target);
bool add(const Account& account, const std::string& name,
         const std::filesystem::path& target, bool (*client_running)() = running);
}
