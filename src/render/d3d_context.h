#pragma once

// D3D11 device and swap chain, set up for the lowest presentation latency the
// API allows: flip model, two buffers, a maximum frame latency of one, and
// tearing permitted so a frame can reach the panel without waiting for vblank.

#include <d3d11.h>
#include <dxgi1_6.h>

#include <string>

#include "common.h"

namespace cap {

// Names of the installed graphics adapters, joined. Used to notice that the
// machine's hardware changed since the encoder test was cached -- a saved
// result from someone's old card is worse than no result at all.
std::string GraphicsAdapterSignature();

class D3DContext {
 public:
  D3DContext() = default;
  ~D3DContext();

  D3DContext(const D3DContext&) = delete;
  D3DContext& operator=(const D3DContext&) = delete;

  bool Initialize(HWND hwnd, std::string* error);
  void Shutdown();

  // Recreates the back buffer for the window's current client size. Safe to
  // call on every resize message.
  void Resize();

  // Binds the back buffer and clears it. Returns false when the window is
  // occluded or has no area, in which case the frame should be skipped.
  bool BeginFrame(const float clearColor[4]);

  // Presents. `vsync` false uses immediate presentation with tearing allowed
  // where the system supports it.
  void EndFrame(bool vsync);

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
  IDXGISwapChain1* swapchain() const { return swapchain_.Get(); }

  int width() const { return width_; }
  int height() const { return height_; }
  bool tearingSupported() const { return tearingSupported_; }

 private:
  bool CreateRenderTarget();
  void ReleaseRenderTarget();

  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<ID3D11RenderTargetView> rtv_;

  int width_ = 0;
  int height_ = 0;
  bool tearingSupported_ = false;
  bool occluded_ = false;
  UINT swapchainFlags_ = 0;
};

}  // namespace cap
