// SPDX-License-Identifier: MIT
// UI-only card views and picker contract; never parses or writes a PS1 card.
#pragma once
#include "recomp_card_manager.h"
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <cstring>
#if defined(_WIN32)
#include "card_picker_win.hpp"
#else
namespace cardpicker {
enum class Kind {card,save,import_mcs,export_mcs};
struct Request {Kind kind;std::filesystem::path initial;};
struct Result {std::filesystem::path path;std::string error;};
}
#endif
namespace cardview {
struct Save {std::string name,title,region,game_id,raw_title;size_t bytes=0;std::vector<int> blocks;std::vector<std::array<uint32_t,256>> icons;};
struct Card {std::vector<Save> saves;std::vector<std::string> errors,warnings;std::array<bool,15> occupied{};int free_blocks=0;bool valid_flag=false,editable_flag=false;bool valid()const{return valid_flag;}bool editable()const{return editable_flag;}};
struct View {struct Snapshot {std::filesystem::path path;bool readonly=false;}snapshot;Card card;std::string error;int selected=-1;uint64_t revision=0;bool loaded_flag=false,dirty_flag=false,writable_flag=false;bool loaded()const{return loaded_flag;}bool dirty()const{return dirty_flag;}};
struct Drag {int source=-1;size_t entry=0;uint64_t revision=0;};
}
