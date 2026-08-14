// SPDX-License-Identifier: MIT
#pragma once
//
// Scoped capture and restore of the D3D11 pipeline state the mod's own passes
// touch. Shared by the SMAA passes (smaa.cpp) and the supersampling downscale
// (supersample.cpp) so the two cannot drift apart on what gets put back: a
// mod-owned pass must leave the context exactly as the game prepared it
// (AGENTS.md, "engine state the mod changes and does not put back").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>

namespace atfix {

// Captures on construction, restores and releases on destruction. Covers the
// superset of what the SMAA passes and the downscale bind: IA layout/VB0/
// topology, all viewports, rasterizer, blend, depth-stencil, all render
// targets, PS samplers 0-1, VS/PS constant buffer 0, PS SRVs 0-9, and the
// VS/PS shaders with their class instances. Scissor rects are omitted because
// no pass sets them.
class ScopedPipelineState {
public:
  explicit ScopedPipelineState(ID3D11DeviceContext* context) : ctx(context) {
    ctx->IAGetInputLayout(&inputLayout);
    ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
    ctx->IAGetPrimitiveTopology(&topology);

    viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&viewportCount, viewports);
    ctx->RSGetState(&rasterizer);

    ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ctx->OMGetDepthStencilState(&depthStencil, &stencilRef);
    ctx->OMGetRenderTargets(
      D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthTarget);

    ctx->PSGetSamplers(0, 2, psSamplers);
    ctx->VSGetConstantBuffers(0, 1, &vsConstantBuffer);
    ctx->PSGetConstantBuffers(0, 1, &psConstantBuffer);
    ctx->PSGetShaderResources(0, 10, psResources);

    vsClassCount = kMaxClassInstances;
    psClassCount = kMaxClassInstances;
    ctx->VSGetShader(&vertexShader, vsClasses, &vsClassCount);
    ctx->PSGetShader(&pixelShader, psClasses, &psClassCount);

    // Geometry, hull and domain are deliberately absent. These engines ship no
    // shader of those kinds: every DXBC blob in all three games is a vertex or
    // pixel shader, and no gs_/hs_/ds_ compile target appears in any executable
    // or asset. The stages are null at every injection point, so capturing them
    // would be restoring null, and the passes never bind them either. Revisit
    // if a build ever ships one.
  }

  ~ScopedPipelineState() {
    ctx->IASetInputLayout(inputLayout);
    ctx->IASetVertexBuffers(
      0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
    ctx->IASetPrimitiveTopology(topology);

    ctx->RSSetViewports(viewportCount, viewports);
    ctx->RSSetState(rasterizer);

    ctx->OMSetBlendState(blend, blendFactor, sampleMask);
    ctx->OMSetDepthStencilState(depthStencil, stencilRef);
    ctx->OMSetRenderTargets(
      D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthTarget);

    ctx->PSSetSamplers(0, 2, psSamplers);
    ctx->VSSetConstantBuffers(0, 1, &vsConstantBuffer);
    ctx->PSSetConstantBuffers(0, 1, &psConstantBuffer);
    ctx->PSSetShaderResources(0, 10, psResources);
    ctx->VSSetShader(vertexShader, vsClasses, vsClassCount);
    ctx->PSSetShader(pixelShader, psClasses, psClassCount);

    release(inputLayout);
    release(vertexBuffer);
    release(rasterizer);
    release(blend);
    release(depthStencil);
    for (auto*& target : renderTargets) release(target);
    release(depthTarget);
    for (auto*& sampler : psSamplers) release(sampler);
    release(vsConstantBuffer);
    release(psConstantBuffer);
    for (auto*& resource : psResources) release(resource);
    release(vertexShader);
    release(pixelShader);
    for (UINT i = 0; i < vsClassCount; ++i) release(vsClasses[i]);
    for (UINT i = 0; i < psClassCount; ++i) release(psClasses[i]);
  }

  ScopedPipelineState(const ScopedPipelineState&) = delete;
  ScopedPipelineState& operator=(const ScopedPipelineState&) = delete;

private:
  static constexpr UINT kMaxClassInstances = 256;

  template<typename T>
  static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

  ID3D11DeviceContext* ctx;
  ID3D11InputLayout* inputLayout = nullptr;
  ID3D11Buffer* vertexBuffer = nullptr;
  UINT vertexStride = 0;
  UINT vertexOffset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  UINT viewportCount = 0;
  D3D11_VIEWPORT viewports[
    D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  ID3D11RasterizerState* rasterizer = nullptr;
  ID3D11BlendState* blend = nullptr;
  FLOAT blendFactor[4] = {};
  UINT sampleMask = 0;
  ID3D11DepthStencilState* depthStencil = nullptr;
  UINT stencilRef = 0;
  ID3D11RenderTargetView* renderTargets[
    D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* depthTarget = nullptr;
  ID3D11SamplerState* psSamplers[2] = {};
  ID3D11Buffer* vsConstantBuffer = nullptr;
  ID3D11Buffer* psConstantBuffer = nullptr;
  ID3D11ShaderResourceView* psResources[10] = {};
  ID3D11VertexShader* vertexShader = nullptr;
  ID3D11PixelShader* pixelShader = nullptr;
  ID3D11ClassInstance* vsClasses[kMaxClassInstances] = {};
  ID3D11ClassInstance* psClasses[kMaxClassInstances] = {};
  UINT vsClassCount = 0;
  UINT psClassCount = 0;
};

}  // namespace atfix
