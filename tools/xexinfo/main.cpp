// Dump d'un XEX : sections, point d'entrée, imports par bibliothèque, et image PE décompressée.
#include <file.h>
#include <image.h>
#include <xex.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: xexinfo <file.xex> [out.pe]\n"); return 1; }
    auto file = LoadFile(argv[1]);
    if (file.empty()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    // Bibliothèques importées (en clair dans l'en-tête XEX)
    auto* hdr = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
    if (hdr) {
        const char* str = reinterpret_cast<const char*>(hdr + 1);
        std::vector<std::string> names; size_t off = 0;
        for (uint32_t i = 0; i < hdr->numImports; i++) { names.emplace_back(str + off); off += ((names.back().size() + 1) + 3) & ~3; }
        auto* lib = (const Xex2ImportLibrary*)((const char*)(hdr + 1) + hdr->sizeOfStringTable);
        for (uint32_t i = 0; i < hdr->numImports; i++) {
            uint32_t v = lib->version;
            printf("LIB %-14s v%u.%u.%u.%u  %u imports\n", names[lib->name].c_str(), v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF, (unsigned)lib->numberOfImports);
            lib = (const Xex2ImportLibrary*)((const char*)(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
    }

    Image img = Image::ParseImage(file.data(), file.size());
    printf("BASE 0x%zX SIZE 0x%X ENTRY 0x%zX\n", img.base, img.size, img.entry_point);
    for (auto& s : img.sections) printf("SECT %-8s 0x%zX 0x%X flags=%u\n", s.name.c_str(), s.base, s.size, s.flags);
    size_t n = 0;
    for (auto& s : img.symbols) { printf("SYM 0x%08zX %s\n", s.address, s.name.c_str()); n++; }
    printf("NAMED_IMPORTS %zu\n", n);
    if (argc > 2) { std::ofstream(argv[2], std::ios::binary).write((const char*)img.data.get(), img.size); }
    return 0;
}
