// make_dxbc.cpp — compiles the test shaders in this folder to DXBC with Windows' d3dcompiler_47.dll (Windows only).
// The .dxbc files are committed, so the translator test itself runs anywhere. Usage: make_dxbc <name.hlsl> <profile>
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
struct Blob {
    struct V { void* qi; ULONG(__stdcall* AddRef)(Blob*); ULONG(__stdcall* Release)(Blob*);
               void*(__stdcall* Ptr)(Blob*); SIZE_T(__stdcall* Size)(Blob*); } * v;
};
typedef HRESULT(WINAPI* D3DCompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                      UINT, UINT, Blob**, Blob**);
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "make_dxbc <name.hlsl> <vs_5_0|ps_5_0>\n"); return 2; }
    auto compile = (D3DCompileFn)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
    if (!compile) { fprintf(stderr, "no d3dcompiler_47.dll\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::string src; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) src.append(b, n);
    fclose(f);
    Blob *code = nullptr, *err = nullptr;
    // D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15), as shipped games compile.
    if (FAILED(compile(src.data(), src.size(), argv[1], nullptr, nullptr, "main", argv[2], 1u << 15, 0, &code, &err))) {
        fprintf(stderr, "%s\n", err ? (const char*)err->v->Ptr(err) : "D3DCompile failed");
        return 1;
    }
    std::string out = std::string(argv[1]).substr(0, std::string(argv[1]).rfind('.')) + ".dxbc";
    f = fopen(out.c_str(), "wb");
    fwrite(code->v->Ptr(code), 1, code->v->Size(code), f);
    fclose(f);
    printf("%s: %zu bytes\n", out.c_str(), (size_t)code->v->Size(code));
    return 0;
}
