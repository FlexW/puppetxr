// Compiles a shader of shaders/ to DXBC at build time, for the D3D11 and D3D12 backends
// (docs/shaders.md). It calls D3DCompile of d3dcompiler_47.dll, which every Windows since 10
// ships and fxc wraps, so the build needs nothing of the Windows SDK but its headers. Cross
// builds run it through CMAKE_CROSSCOMPILING_EMULATOR.
//
// Usage: puppetxr_hlsl <input> <output> <profile> [-D<name>=<value>]...
// The input has its includes inlined (cmake/shader_text.cmake). Warnings are errors.

#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// Types
//

#define HLSL_MAX_DEFINES 16

typedef HRESULT (WINAPI* HLSL_D3DCompile) (const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entry, const char* target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors);

//
// Statics
//

static char* read_all (const char* path, long* out_size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return nullptr;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* data = size >= 0 ? (char*)malloc((size_t)size + 1) : nullptr;
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size)
    {
        free(data);
        data = nullptr;
    }
    fclose(file);
    if (data)
    {
        data[size] = 0;
        *out_size  = size;
    }
    return data;
}

//
// Entry point
//

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: puppetxr_hlsl <input> <output> <profile> [-D<name>=<value>]...\n");
        return 2;
    }
    const char* input   = argv[1];
    const char* output  = argv[2];
    const char* profile = argv[3];

    char             define_text[HLSL_MAX_DEFINES][256] = {};
    D3D_SHADER_MACRO defines[HLSL_MAX_DEFINES + 1]      = {};
    int              defines_count                      = 0;
    for (int i = 4; i < argc; i++)
    {
        if (strncmp(argv[i], "-D", 2) != 0 || defines_count == HLSL_MAX_DEFINES || strlen(argv[i] + 2) >= sizeof(define_text[0]))
        {
            fprintf(stderr, "puppetxr_hlsl: bad argument %s\n", argv[i]);
            return 2;
        }
        char* text = define_text[defines_count];
        strcpy(text, argv[i] + 2);
        char* value = strchr(text, '=');
        if (value)
        {
            *value++ = 0;
        }
        defines[defines_count++] = { text, value ? value : "1" };
    }

    HMODULE library = LoadLibraryA("d3dcompiler_47.dll");
    const HLSL_D3DCompile compile = library ? (HLSL_D3DCompile)(void (*)(void))GetProcAddress(library, "D3DCompile") : nullptr;
    if (!compile)
    {
        fprintf(stderr, "puppetxr_hlsl: d3dcompiler_47.dll has no D3DCompile\n");
        return 1;
    }

    long  size   = 0;
    char* source = read_all(input, &size);
    if (!source)
    {
        fprintf(stderr, "puppetxr_hlsl: cannot read %s\n", input);
        return 1;
    }
    const UINT flags  = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_ENABLE_STRICTNESS;
    ID3DBlob*  code   = nullptr;
    ID3DBlob*  errors = nullptr;
    const HRESULT result = compile(source, (SIZE_T)size, input, defines, nullptr, "main", profile, flags, 0, &code, &errors);
    free(source);
    if (errors)
    {
        fprintf(stderr, "%.*s", (int)errors->GetBufferSize(), (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(result) || !code)
    {
        fprintf(stderr, "puppetxr_hlsl: %s failed to compile for %s (0x%08lx)\n", input, profile, (unsigned long)result);
        return 1;
    }

    FILE* file    = fopen(output, "wb");
    bool  ok      = file && fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), file) == code->GetBufferSize();
    if (file)
    {
        ok = fclose(file) == 0 && ok;
    }
    code->Release();
    if (!ok)
    {
        fprintf(stderr, "puppetxr_hlsl: cannot write %s\n", output);
        return 1;
    }
    return 0;
}
