#include <d3dx9.h>

namespace
{
    struct D3dxModule
    {
        HMODULE module = nullptr;
        HRESULT failure = S_OK;
    };

    D3dxModule LoadD3dxModule()
    {
        HMODULE module = LoadLibraryA("D3DX9_43.dll");
        if (!module)
        {
            const DWORD error = GetLastError();
            return { nullptr, HRESULT_FROM_WIN32(error ? error : ERROR_MOD_NOT_FOUND) };
        }
        return { module, S_OK };
    }

    FARPROC LoadD3dxProc(const char* name, HRESULT& outFailure)
    {
        static const D3dxModule d3dx = LoadD3dxModule();
        if (!d3dx.module)
        {
            outFailure = d3dx.failure;
            return nullptr;
        }

        FARPROC proc = GetProcAddress(d3dx.module, name);
        if (!proc)
        {
            const DWORD error = GetLastError();
            outFailure = HRESULT_FROM_WIN32(error ? error : ERROR_PROC_NOT_FOUND);
            return nullptr;
        }

        outFailure = S_OK;
        return proc;
    }
}

extern "C" HRESULT WINAPI D3DXCompileShader(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    const D3DXMACRO* pDefines,
    ID3DXInclude* pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer** ppShader,
    ID3DXBuffer** ppErrorMsgs,
    ID3DXConstantTable** ppConstantTable)
{
    using Fn = HRESULT(WINAPI*)(
        LPCSTR,
        UINT,
        const D3DXMACRO*,
        ID3DXInclude*,
        LPCSTR,
        LPCSTR,
        DWORD,
        ID3DXBuffer**,
        ID3DXBuffer**,
        ID3DXConstantTable**);

    HRESULT failure = S_OK;
    FARPROC proc = LoadD3dxProc("D3DXCompileShader", failure);
    if (!proc)
        return failure;

    return reinterpret_cast<Fn>(proc)(
        pSrcData,
        SrcDataLen,
        pDefines,
        pInclude,
        pFunctionName,
        pProfile,
        Flags,
        ppShader,
        ppErrorMsgs,
        ppConstantTable);
}

extern "C" HRESULT WINAPI D3DXCreateTextureFromFileInMemory(
    IDirect3DDevice9* pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    IDirect3DTexture9** ppTexture)
{
    using Fn = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DTexture9**);

    HRESULT failure = S_OK;
    FARPROC proc = LoadD3dxProc("D3DXCreateTextureFromFileInMemory", failure);
    if (!proc)
        return failure;

    return reinterpret_cast<Fn>(proc)(pDevice, pSrcData, SrcDataSize, ppTexture);
}
