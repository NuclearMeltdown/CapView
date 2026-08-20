#include "render/d3d_context.h"

#include <algorithm>
#include <vector>

namespace cap {

std::string GraphicsAdapterSignature() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};

  // Deduplicated and sorted. DXGI happily reports the same card several times --
  // this machine lists one RTX 4080 four times -- and the count can change when
  // a monitor is plugged in. A signature that moves for that reason would throw
  // away a perfectly good encoder test.
  std::vector<std::string> names;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    // Skip the software renderer: it is always there and says nothing about
    // what encoders exist.
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    names.push_back(ToUtf8(desc.Description));
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::string result;
  for (const std::string& name : names) {
    if (!result.empty()) result += " + ";
    result += name;
  }
  return result;
}
namespace {

const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

bool QueryTearingSupport(IDXGIFactory2* factory) {
  ComPtr<IDXGIFactory5> factory5;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) return false;
  BOOL allow = FALSE;
  if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                           sizeof(allow)))) {
    return false;
  }
  return allow != FALSE;
}

}  // namespace

D3DContext::~D3DContext() {
  Shutdown();
}

bool D3DContext::Initialize(HWND hwnd, std::string* error) {
  hwnd_ = hwnd;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

  HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                                   &context_);
#ifdef _DEBUG
  if (FAILED(hr)) {
    // The debug layer is not installed on every machine.
    flags &= ~(UINT)D3D11_CREATE_DEVICE_DEBUG;
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                             (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                             &context_);
  }
#endif
  if (FAILED(hr)) {
    CAP_WARN("Hardware-Gerät nicht verfügbar (%s), versuche WARP", HrToString(hr).c_str());
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                             (UINT)std::size(levels), D3D11_SDK_VERSION, &device_, &obtained,
                             &context_);
  }
  if (FAILED(hr)) {
    if (error) *error = "Direct3D 11 konnte nicht initialisiert werden: " + HrToString(hr);
    return false;
  }

  ComPtr<IDXGIDevice1> dxgiDevice;
  if (FAILED(hr = device_.As(&dxgiDevice))) {
    if (error) *error = "IDXGIDevice1 nicht verfügbar: " + HrToString(hr);
    return false;
  }
  // One frame of latency: the CPU never runs more than a frame ahead of the GPU.
  dxgiDevice->SetMaximumFrameLatency(1);

  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(hr = dxgiDevice->GetAdapter(&adapter))) {
    if (error) *error = "DXGI-Adapter nicht verfügbar: " + HrToString(hr);
    return false;
  }
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    if (error) *error = "DXGI-Factory nicht verfügbar: " + HrToString(hr);
    return false;
  }

  tearingSupported_ = QueryTearingSupport(factory.Get());
  swapchainFlags_ = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  width_ = std::max<int>(1, rc.right - rc.left);
  height_ = std::max<int>(1, rc.bottom - rc.top);

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = (UINT)width_;
  desc.Height = (UINT)height_;
  desc.Format = kBackBufferFormat;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = swapchainFlags_;

  hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapchain_);
  if (FAILED(hr)) {
    if (error) *error = "Swapchain konnte nicht erstellt werden: " + HrToString(hr);
    return false;
  }

  // We handle fullscreen ourselves with a borderless window, so DXGI must keep
  // its hands off Alt+Enter and off resizing the window.
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  if (!CreateRenderTarget()) {
    if (error) *error = "Rendertarget konnte nicht erstellt werden";
    return false;
  }

  CAP_LOG("D3D11 bereit: Feature Level 0x%X, Tearing %s", (unsigned)obtained,
          tearingSupported_ ? "unterstützt" : "nicht unterstützt");
  return true;
}

void D3DContext::Shutdown() {
  ReleaseRenderTarget();
  swapchain_.Reset();
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
  context_.Reset();
  device_.Reset();
  hwnd_ = nullptr;
}

bool D3DContext::CreateRenderTarget() {
  ComPtr<ID3D11Texture2D> backbuffer;
  if (FAILED(CAP_HR(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))) return false;
  if (FAILED(CAP_HR(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_)))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  backbuffer->GetDesc(&desc);
  width_ = (int)desc.Width;
  height_ = (int)desc.Height;
  return true;
}

void D3DContext::ReleaseRenderTarget() {
  rtv_.Reset();
}

void D3DContext::Resize() {
  if (!swapchain_) return;

  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;             // minimised
  if (w == width_ && h == height_) return;  // nothing changed

  ReleaseRenderTarget();
  HRESULT hr = swapchain_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, swapchainFlags_);
  if (FAILED(hr)) {
    CAP_ERR("ResizeBuffers fehlgeschlagen: %s", HrToString(hr).c_str());
  }
  CreateRenderTarget();
}

bool D3DContext::BeginFrame(const float clearColor[4]) {
  if (!rtv_ || width_ <= 0 || height_ <= 0) return false;

  if (occluded_) {
    // Cheap test present; nothing is drawn while the window is fully covered.
    if (swapchain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return false;
    occluded_ = false;
  }

  ID3D11RenderTargetView* targets[] = {rtv_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->ClearRenderTargetView(rtv_.Get(), clearColor);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width_;
  vp.Height = (float)height_;
  vp.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &vp);
  return true;
}

void D3DContext::EndFrame(bool vsync) {
  if (!swapchain_) return;

  UINT interval = vsync ? 1u : 0u;
  UINT flags = (!vsync && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;

  HRESULT hr = swapchain_->Present(interval, flags);
  if (hr == DXGI_STATUS_OCCLUDED) {
    occluded_ = true;
  } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    CAP_ERR("Grafikgerät verloren: %s",
            HrToString(device_ ? device_->GetDeviceRemovedReason() : hr).c_str());
  }
}

}  // namespace cap
