// Stub when linking without the DXVK VR fork. Real g_D3DVR9 is set inside DXVK's d3d9_vr.cpp.
#include "d3d9_vr.h"

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9 *, IDirect3DVR9 **pInterface)
{
    if (pInterface)
        *pInterface = nullptr;
    return E_NOTIMPL;
}
