// SPDX-License-Identifier: MIT
#pragma once
//
// D3D11 proxy vtable dispatch types: the PFN typedefs and the DeviceProcs /
// ContextProcs tables that hold the original (unhooked) entry points, plus the
// accessors returning them. The tables are defined and populated in sync_fix.cpp;
// feature modules split out of the proxy include this to call original methods
// through getDeviceProcs() / getContextProcs().
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace atfix {

/** Hooking-related stuff */
using PFN_ID3D11Device_CreateBuffer = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_ID3D11Device_CreateDeferredContext = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  UINT, ID3D11DeviceContext**);
using PFN_ID3D11Device_CreateTexture1D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using PFN_ID3D11Device_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_ID3D11Device_CreateTexture3D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
using PFN_ID3D11Device_CreateVertexShader = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_ID3D11Device_CreatePixelShader = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_ID3D11Device_CreateSamplerState = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

using PFN_ID3D11DeviceContext_ClearRenderTargetView = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11RenderTargetView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearDepthStencilView = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const UINT[4]);
using PFN_ID3D11DeviceContext_CopyResource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, ID3D11Resource*);
using PFN_ID3D11DeviceContext_CopySubresourceRegion = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_ID3D11DeviceContext_CopyStructureCount = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
using PFN_ID3D11DeviceContext_Dispatch = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DispatchIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_OMSetRenderTargets = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_ID3D11DeviceContext_UpdateSubresource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_ID3D11DeviceContext_Map = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_ID3D11DeviceContext_Unmap = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT);
using PFN_ID3D11DeviceContext_RSSetViewports = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, const D3D11_VIEWPORT*);
using PFN_ID3D11DeviceContext_RSSetScissorRects = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, const D3D11_RECT*);
using PFN_ID3D11DeviceContext_Draw = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexed = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_ID3D11DeviceContext_DrawInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DrawIndexedInstanced = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_ID3D11DeviceContext_DrawAuto = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*);
using PFN_ID3D11DeviceContext_DrawInstancedIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_DrawIndexedInstancedIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_ExecuteCommandList = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_ID3D11DeviceContext_FinishCommandList = HRESULT (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_ID3D11DeviceContext_PSSetShaderResources = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

struct DeviceProcs {
  PFN_ID3D11Device_CreateBuffer                         CreateBuffer                  = nullptr;
  PFN_ID3D11Device_CreateDeferredContext                CreateDeferredContext         = nullptr;
  PFN_ID3D11Device_CreateTexture1D                      CreateTexture1D               = nullptr;
  PFN_ID3D11Device_CreateTexture2D                      CreateTexture2D               = nullptr;
  PFN_ID3D11Device_CreateTexture3D                      CreateTexture3D               = nullptr;
  PFN_ID3D11Device_CreateVertexShader                   CreateVertexShader            = nullptr;
  PFN_ID3D11Device_CreatePixelShader                    CreatePixelShader             = nullptr;
  PFN_ID3D11Device_CreateSamplerState                   CreateSamplerState            = nullptr;
};

struct ContextProcs {
  PFN_ID3D11DeviceContext_ClearRenderTargetView         ClearRenderTargetView         = nullptr;
  PFN_ID3D11DeviceContext_ClearDepthStencilView         ClearDepthStencilView         = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat ClearUnorderedAccessViewFloat = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint  ClearUnorderedAccessViewUint  = nullptr;
  PFN_ID3D11DeviceContext_CopyResource                  CopyResource                  = nullptr;
  PFN_ID3D11DeviceContext_CopySubresourceRegion         CopySubresourceRegion         = nullptr;
  PFN_ID3D11DeviceContext_CopyStructureCount            CopyStructureCount            = nullptr;
  PFN_ID3D11DeviceContext_Dispatch                      Dispatch                      = nullptr;
  PFN_ID3D11DeviceContext_DispatchIndirect              DispatchIndirect              = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargets            OMSetRenderTargets            = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews OMSetRenderTargetsAndUnorderedAccessViews = nullptr;
  PFN_ID3D11DeviceContext_UpdateSubresource             UpdateSubresource             = nullptr;
  PFN_ID3D11DeviceContext_Map                           Map                           = nullptr;
  PFN_ID3D11DeviceContext_Unmap                         Unmap                         = nullptr;
  PFN_ID3D11DeviceContext_RSSetViewports                 RSSetViewports                = nullptr;
  PFN_ID3D11DeviceContext_RSSetScissorRects              RSSetScissorRects             = nullptr;
  PFN_ID3D11DeviceContext_Draw                          Draw                          = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexed                   DrawIndexed                   = nullptr;
  PFN_ID3D11DeviceContext_DrawInstanced                 DrawInstanced                 = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexedInstanced          DrawIndexedInstanced          = nullptr;
  PFN_ID3D11DeviceContext_DrawAuto                      DrawAuto                      = nullptr;
  PFN_ID3D11DeviceContext_DrawInstancedIndirect         DrawInstancedIndirect         = nullptr;
  PFN_ID3D11DeviceContext_DrawIndexedInstancedIndirect  DrawIndexedInstancedIndirect  = nullptr;
  PFN_ID3D11DeviceContext_ExecuteCommandList            ExecuteCommandList            = nullptr;
  PFN_ID3D11DeviceContext_FinishCommandList             FinishCommandList             = nullptr;
  PFN_ID3D11DeviceContext_PSSetShaderResources          PSSetShaderResources          = nullptr;
};

const DeviceProcs* getDeviceProcs(ID3D11Device* pDevice);
const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext);

}  // namespace atfix
