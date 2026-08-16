#pragma once

#include <Windows.h>
#include <d3d9.h>

struct D3DXMACRO
{
    LPCSTR Name;
    LPCSTR Definition;
};

struct ID3DXInclude
{
};

struct ID3DXConstantTable;

struct __declspec(uuid("8ba5fb08-5195-40e2-ac58-0d989c3a0102")) ID3DXBuffer : public IUnknown
{
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};

extern "C"
{
    HRESULT WINAPI D3DXCompileShader(
        LPCSTR pSrcData,
        UINT SrcDataLen,
        const D3DXMACRO* pDefines,
        ID3DXInclude* pInclude,
        LPCSTR pFunctionName,
        LPCSTR pProfile,
        DWORD Flags,
        ID3DXBuffer** ppShader,
        ID3DXBuffer** ppErrorMsgs,
        ID3DXConstantTable** ppConstantTable);

    HRESULT WINAPI D3DXCreateTextureFromFileInMemory(
        IDirect3DDevice9* pDevice,
        LPCVOID pSrcData,
        UINT SrcDataSize,
        IDirect3DTexture9** ppTexture);
}
