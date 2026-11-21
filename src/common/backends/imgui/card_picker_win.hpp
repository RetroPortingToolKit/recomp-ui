// SPDX-License-Identifier: MIT
// Native Windows picker for the manager. Called BETWEEN ImGui frames, not from
// a nested modal. No fixed 512-byte UTF-8 result buffer or hidden fallback UI.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <commdlg.h>
#include <functional>
namespace cardpicker {
enum class Kind {card,save,export_mcs,import_mcs};
struct Request {Kind kind=Kind::card;std::filesystem::path start;};
struct Result {std::filesystem::path path;std::string error;};
using Dialog=BOOL(WINAPI*)(LPOPENFILENAMEW);
using Error=DWORD(WINAPI*)();
inline Result choose(const Request& request,Dialog injected=nullptr,Error last_error=CommDlgExtendedError) {
    bool save=request.kind==Kind::save || request.kind==Kind::export_mcs;
    bool mcs=request.kind==Kind::export_mcs || request.kind==Kind::import_mcs;
    std::vector<wchar_t> buffer(32768,0);auto initial=request.start.wstring();
    if(initial.size()<buffer.size())std::copy(initial.begin(),initial.end(),buffer.begin());
    static const wchar_t card_filter[]=L"PS1 memory cards\0*.mcd;*.mcr;*.mc;*.srm\0All files\0*.*\0";
    static const wchar_t save_filter[]=L"PS1 single saves\0*.mcs\0All files\0*.*\0";
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=GetActiveWindow();
    std::error_code ec;auto directory=request.start.parent_path().wstring();
    if(std::filesystem::is_directory(request.start,ec)){directory=request.start.wstring();buffer[0]=0;}
    ofn.lpstrInitialDir=directory.empty()?nullptr:directory.c_str();
    ofn.lpstrFile=buffer.data();ofn.nMaxFile=DWORD(buffer.size());ofn.lpstrFilter=mcs?save_filter:card_filter;
    ofn.lpstrDefExt=mcs?L"mcs":L"mcd";ofn.lpstrTitle=save?(mcs?L"Export PS1 save":L"Save Card As"):mcs?L"Import PS1 save":L"Open Card";
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    Dialog show=injected?injected:(save?GetSaveFileNameW:GetOpenFileNameW);
    if(show(&ofn))return {std::filesystem::path(buffer.data()),{}};
    DWORD code=last_error();return {{},code?"The Windows file dialog failed (code "+std::to_string(code)+"). No card was changed.":""};
}
} // namespace cardpicker
