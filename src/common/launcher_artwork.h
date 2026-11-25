#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

// Optional read-only cover images, one folder per disc serial
// (<artwork root>/<serial>/). The launcher never writes into these folders.
namespace launcher_artwork {
inline std::string utf8_bytes(const std::filesystem::path& path) {
    auto text=path.u8string();
    return {reinterpret_cast<const char*>(text.data()),text.size()};
}
namespace fs = std::filesystem;
struct Catalog { std::vector<fs::path> images; };
inline std::string lower(std::string s) {
    for (char& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool supported(const std::string& serial) {
    // Any console's disc serial that is a safe single folder name
    // (SCES-01330, T-1234G, MK-81000): no separators, no dot names.
    if(serial.empty() || serial.size()>64 || serial[0]=='.')return false;
    for(char c:serial) {
        const bool ok=(c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
                      c=='-' || c=='_' || c=='.';
        if(!ok)return false;
    }
    return true;
}
inline fs::path root(fs::path executable_directory) {
    executable_directory=executable_directory.lexically_normal();
    // SDL_GetBasePath ends in a separator; remove its empty filename first.
    if(executable_directory.filename().empty()) executable_directory=executable_directory.parent_path();
    return executable_directory/"artwork";
}
inline fs::path folder(const fs::path& home,const std::string& serial) {
    if(!supported(serial))return {};
    return fs::absolute(home/serial).lexically_normal();
}
inline int rank(const fs::path& p) {
    auto name=lower(utf8_bytes(p.filename()));
    if(name.find("front")!=std::string::npos)return 0;
    if(name.find("back")!=std::string::npos)return 1;
    if(name.find("disc")!=std::string::npos)return 2;
    return 3;
}
inline Catalog scan(const fs::path& home,const std::string& serial) {
    Catalog c;if(!supported(serial))return c;
    try {
        std::error_code ec;auto directory=folder(home,serial);
        fs::directory_iterator it(directory,fs::directory_options::skip_permission_denied,ec),end;
        while(!ec && it!=end) {
            const auto p=it->path();
            if(!it->is_symlink(ec) && it->is_regular_file(ec)) {
                auto ext=lower(utf8_bytes(p.extension()));
                if((ext==".png" || ext==".jpg" || ext==".jpeg" || ext==".tga") &&
                    it->file_size(ec)<=32*1024*1024 && !ec)c.images.push_back(p);
            }
            it.increment(ec);
        }
        std::sort(c.images.begin(),c.images.end(),[](const auto& a,const auto& b){
            return rank(a)!=rank(b)?rank(a)<rank(b):a.u8string()<b.u8string();
        });
    } catch(const std::exception&) { return {}; }
    return c;
}
inline size_t step(size_t current,size_t count,int direction) {
    if(!count)return 0;
    return direction<0?(current+count-1)%count:(current+1)%count;
}
struct Size { float w,h; };
inline Size fit(float w,float h,float max_w,float max_h) {
    if(w<=0 || h<=0 || max_w<=0 || max_h<=0)return {0,0};
    float scale=std::min(max_w/w,max_h/h);return {w*scale,h*scale};
}
}
