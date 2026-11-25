#include "launcher_artwork.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace launcher_artwork;
int main(int argc,char** argv) {
    assert(argc==2);const auto qa=fs::u8path(argv[1]);
    fs::remove_all(qa); // a previous run's tree; the test owns this directory
    fs::create_directories(qa);const auto home=qa/"portable"/"artwork";
    const std::vector<std::string> regions={"SCES-01330","SCES-01331","SLPS-01144"};
    for(const auto& serial:regions) {
        const auto standard=home/serial;fs::create_directories(standard);
        assert(folder(home,serial)==fs::absolute(standard).lexically_normal());
        std::ofstream(standard/"front.png")<<"fixture";
        std::ofstream(standard/"back.JPG")<<"fixture";
        std::ofstream(standard/"disc.tga")<<"fixture";
        std::ofstream(standard/"manual.pdf")<<"%PDF-fixture";
        auto c=scan(home,serial);assert(c.images.size()==3);
        assert(rank(c.images[0])==0 && rank(c.images[1])==1 && rank(c.images[2])==2);
        assert(fs::file_size(standard/"front.png")==7);
        assert(fs::file_size(standard/"manual.pdf")==12); // existing documents untouched
    }
    // Unicode serial folders and more than 32 images (no old truncation).
    const auto many=home/"SLPS-01144";
    for(int i=0;i<40;++i)std::ofstream(many/fs::u8path(std::to_string(i)+utf8_bytes(fs::u8path(u8" 日本.png"))))<<"fixture";
    assert(scan(home,"SLPS-01144").images.size()==43);
    // A missing folder or an unsafe serial is simply empty.
    assert(scan(home,"SLUS-00000").images.empty());
    assert(scan(home,"../bad").images.empty() && folder(home,"../bad").empty());
    assert(supported("T-1234G") && supported("MK-81000") && !supported("") &&
           !supported("..") && !supported("a/b") && !supported(std::string(65,'A')));
    // Folders are found relative to the artwork root, so a moved portable tree keeps its covers.
    const auto moved=qa/"moved portable";fs::rename(home.parent_path(),moved);
    assert(scan(moved/"artwork",regions[0]).images.size()==3);
    assert(step(0,3,-1)==2 && step(2,3,1)==0 && step(0,0,1)==0);
    auto fit_size=fit(800,400,200,200);assert(fit_size.w==200 && fit_size.h==100);
    std::cout<<"PASS: per-serial folders, relocation, Unicode, missing/unsafe serials, 43 images, navigation and aspect ratio; no writes.\n";
}
