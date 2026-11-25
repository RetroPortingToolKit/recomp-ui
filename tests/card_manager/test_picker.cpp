#include "card_picker_win.hpp"

#include <iostream>
static std::wstring chosen;
static BOOL WINAPI accepted(LPOPENFILENAMEW p) {
    if(p->nMaxFile<32768 || !(p->Flags&OFN_NOCHANGEDIR) || !(p->Flags&OFN_FILEMUSTEXIST))return FALSE;
    std::copy(chosen.begin(),chosen.end(),p->lpstrFile);p->lpstrFile[chosen.size()]=0;return TRUE;
}
static BOOL WINAPI cancelled(LPOPENFILENAMEW){return FALSE;}
static BOOL WINAPI saved(LPOPENFILENAMEW p) {
    if(!(p->Flags&OFN_NOCHANGEDIR) || !(p->Flags&OFN_OVERWRITEPROMPT) || (p->Flags&OFN_FILEMUSTEXIST))return FALSE;
    std::copy(chosen.begin(),chosen.end(),p->lpstrFile);p->lpstrFile[chosen.size()]=0;return TRUE;
}
static DWORD WINAPI no_error(){return 0;}
static DWORD WINAPI fail_error(){return FNERR_INVALIDFILENAME;}
int main()try {
    chosen=L"C:\\"+std::wstring(800,L'X')+L"\\日本語 memory card.mcd";
    auto r=cardpicker::choose({},accepted,no_error);
    if(r.path.wstring()!=chosen || !r.error.empty())throw std::runtime_error("Unicode/long path was truncated");
    r=cardpicker::choose({cardpicker::Kind::save,{}},saved,no_error);if(r.path.wstring()!=chosen)throw std::runtime_error("Save Card As dialog contract");
    r=cardpicker::choose({},cancelled,no_error);if(!r.path.empty() || !r.error.empty())throw std::runtime_error("Cancel handling");
    r=cardpicker::choose({},cancelled,fail_error);if(r.error.empty())throw std::runtime_error("Native error was silently ignored");
    std::cout<<"PASS native-Windows picker contract, Unicode/long paths, Cancel/error\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
