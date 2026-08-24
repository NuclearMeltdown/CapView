#include "ui/settings_host.h"

#include "resource.h"

#include <dwmapi.h>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "ui/theme.h"
#include "i18n.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {
namespace {

const wchar_t kClassName[] = L"CapViewSettingsWindow";

void ApplyDarkTitleBar(HWND hwnd, bool dark) {
  const BOOL value = dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
}

}  // namespace

SettingsHost::~SettingsHost() { Destroy(); }

LRESULT CALLBACK SettingsHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  SettingsHost* self = reinterpret_cast<SettingsHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }
  if (!self || !self->imgui_) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

  // Input has to be fed to *this* window's context, not the main one, so the
  // current context is swapped for the duration of the handler.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(self->imgui_);
  const bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
  ImGui::SetCurrentContext(previous);
  if (handled) return 1;

  switch (msg) {
    case WM_CLOSE:
      self->closeRequested_ = true;
      self->Hide();
      return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) self->Resize();
      return 0;
    case WM_ENTERSIZEMOVE:
      // 8 ms is about half a field; fast enough that the picture keeps moving
      // while the window is dragged, slow enough not to fight the drag itself.
      ::SetTimer(hwnd, 1, 8, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      ::KillTimer(hwnd, 1);
      return 0;
    case WM_TIMER:
      // The callback draws a whole frame, which comes back through here for this
      // window -- so it is kept out of itself.
      if (wParam == 1 && self->onFrame_ && !self->inFrameCallback_) {
        self->inFrameCallback_ = true;
        self->onFrame_();
        self->inFrameCallback_ = false;
      }
      return 0;
    case WM_DESTROY:
      self->hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SettingsHost::Create(HINSTANCE instance, HWND owner, ID3D11Device* device,
                          ID3D11DeviceContext* context, ImFontAtlas* atlas, float uiScale,
                          std::string* error) {
  if (hwnd_) return true;
  device_ = device;
  ctx_ = context;
  uiScale_ = uiScale > 0.1f ? uiScale : 1.0f;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &SettingsHost::WndProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // The same icon as the preview. Without it the taskbar button this window now
  // has -- and its Alt+Tab entry -- would show the generic placeholder.
  wc.hIcon = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                 ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON),
                                 LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_CAPVIEW), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
  wc.lpszClassName = kClassName;
  // Registering twice is not an error worth failing over; the second call just
  // tells us it is already there.
  ::RegisterClassExW(&wc);

  const int w = (int)(720 * uiScale_);
  const int h = (int)(640 * uiScale_);
  // WS_EX_APPWINDOW, and it is not decoration. An owned window is deliberately
  // kept off the taskbar by Windows -- and a window with no taskbar button, when
  // minimised, has nowhere to go, so it lands as a stub in the bottom left
  // corner of the screen the way windows did before there was a taskbar. That is
  // both reported symptoms, and this one flag is both fixes. The owner is worth
  // keeping: it makes the settings stay above the preview and close with it.
  hwnd_ = ::CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"CapView", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, owner, nullptr, instance, this);
  if (!hwnd_) {
    if (error) *error = T("Einstellungsfenster konnte nicht erstellt werden",
                             "The settings window could not be created");
    return false;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(CAP_HR(device_.As(&dxgiDevice))) ||
      FAILED(CAP_HR(dxgiDevice->GetAdapter(&adapter))) ||
      FAILED(CAP_HR(adapter->GetParent(IID_PPV_ARGS(&factory))))) {
    if (error) *error = T("DXGI-Factory für das Einstellungsfenster fehlt",
                             "The settings window has no DXGI factory");
    Destroy();
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (FAILED(CAP_HR(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                                    &swapchain_)))) {
    if (error) *error = T("Swapchain für das Einstellungsfenster fehlgeschlagen",
                             "The settings window swapchain failed");
    Destroy();
    return false;
  }
  // Deliberately not calling MakeWindowAssociation here. It is a property of the
  // *factory*, not of a window: calling it again would replace the association
  // the main window made, and with it the NO_WINDOW_CHANGES that stops DXGI
  // resizing the preview behind our back. Leaving it alone means Alt+Enter keeps
  // meaning the preview, which is what it should mean anyway.

  // A context of its own, sharing the main one's fonts: the glyphs are the same
  // and one atlas on the GPU is enough for both.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  imgui_ = ImGui::CreateContext(atlas);
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const bool ok = ImGui_ImplWin32_Init(hwnd_) && ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());
  ImGui::SetCurrentContext(previous);
  if (!ok) {
    if (error) *error = T("ImGui für das Einstellungsfenster fehlgeschlagen",
                             "ImGui failed for the settings window");
    Destroy();
    return false;
  }

  CreateRenderTarget();
  CAP_LOG("Einstellungsfenster angelegt");
  return true;
}

void SettingsHost::Destroy() {
  if (imgui_) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    // If this context is the current one, it is about to stop existing -- so
    // there is nothing to go back to and nothing to repair.
    if (previous == imgui_) previous = nullptr;
    ImGui::SetCurrentContext(imgui_);
    // Guarded, because Create can fail between the two backends: the platform
    // one is initialised first and the renderer one only if it succeeded, and
    // the failure path comes straight here. Shutting down a backend that was
    // never started walks a null.
    if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
    // The shared atlas belongs to the main context, so it must survive this.
    ImGui::GetIO().Fonts = nullptr;
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
    ImGui::SetCurrentContext(previous == nullptr ? nullptr : previous);

    // And then put back what that took from the main context, if there is a main
    // context left to put it back into -- at program exit there may not be.
    //
    // Sharing one font atlas between two contexts has a sting in it that is
    // easy to miss: the DX11 backend keeps the atlas's texture id *in the
    // atlas*, and its shutdown calls io.Fonts->SetTexID(0) and releases the
    // texture. Since the atlas is the main context's, closing this window left
    // the main interface drawing every glyph with a null texture -- which is
    // exactly the "everything breaks until restart" that was reported. One call
    // rebuilds the texture and puts the id back.
    if (previous && ImGui::GetIO().BackendRendererUserData) {
      ImGui_ImplDX11_CreateDeviceObjects();
    }
  }
  ReleaseRenderTarget();
  swapchain_.Reset();
  if (hwnd_) {
    HWND h = hwnd_;
    hwnd_ = nullptr;
    ::DestroyWindow(h);
  }
  device_.Reset();
  ctx_.Reset();
  visible_ = false;
}

bool SettingsHost::CreateRenderTarget() {
  if (!swapchain_) return false;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  if (FAILED(CAP_HR(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_)))) return false;

  D3D11_TEXTURE2D_DESC td = {};
  back->GetDesc(&td);
  width_ = (int)td.Width;
  height_ = (int)td.Height;
  return true;
}

void SettingsHost::ReleaseRenderTarget() { rtv_.Reset(); }

void SettingsHost::Resize() {
  if (!swapchain_) return;
  ReleaseRenderTarget();
  swapchain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
  CreateRenderTarget();
}

void SettingsHost::Show(const std::wstring& title) {
  if (!hwnd_) return;
  ::SetWindowTextW(hwnd_, title.c_str());
  // SW_SHOW displays a minimised window *still minimised*, and BeginFrame then
  // refuses to draw it -- so reopening the settings after minimising them did
  // nothing at all. Only reachable since this window gained a taskbar button
  // and could be minimised properly in the first place.
  ::ShowWindow(hwnd_, ::IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(hwnd_);
  visible_ = true;
  closeRequested_ = false;
}

void SettingsHost::Hide() {
  if (!hwnd_) return;
  ::ShowWindow(hwnd_, SW_HIDE);
  visible_ = false;
}

bool SettingsHost::takeCloseRequest() {
  const bool requested = closeRequested_;
  closeRequested_ = false;
  return requested;
}

void SettingsHost::ApplyTheme(bool darkMode, unsigned accentColor) {
  if (!imgui_) return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ApplyImGuiTheme(darkMode, accentColor);
  ImGui::GetStyle().ScaleAllSizes(uiScale_);
  ImGui::SetCurrentContext(previous);
  ApplyDarkTitleBar(hwnd_, darkMode);
  themeApplied_ = true;
}

bool SettingsHost::BeginFrame(bool darkMode, unsigned accentColor) {
  if (!hwnd_ || !visible_ || !imgui_ || !rtv_) return false;
  if (::IsIconic(hwnd_)) return false;
  // Covered by something else: stop drawing and ask cheaply whether that is
  // still true, rather than paying for a present nobody can see.
  if (occluded_) {
    if (swapchain_->Present(0, DXGI_PRESENT_TEST) != S_OK) return false;
    occluded_ = false;
  }
  if (width_ <= 0 || height_ <= 0) return false;

  // Sixty redraws a second is plenty for a dialog, and the preview runs at twice
  // that or more -- drawing the whole settings tree on every one of its frames
  // is work nobody sees.
  const DWORD now = ::GetTickCount();
  if (lastDrawTick_ != 0 && now - lastDrawTick_ < 16) return false;
  lastDrawTick_ = now;

  if (!themeApplied_) ApplyTheme(darkMode, accentColor);

  previous_ = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  return true;
}

void SettingsHost::EndFrame() {
  ImGui::Render();

  float clear[4];
  GetBackgroundColor(true, 0x000000, clear);
  const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  clear[0] = bg.x;
  clear[1] = bg.y;
  clear[2] = bg.z;
  clear[3] = 1.0f;

  ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
  ctx_->OMSetRenderTargets(1, rtvs, nullptr);
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);

  D3D11_VIEWPORT vp = {};
  vp.Width = (float)width_;
  vp.Height = (float)height_;
  vp.MaxDepth = 1.0f;
  ctx_->RSSetViewports(1, &vp);

  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  // Sixty a second, after the preview has already gone to the screen. Both
  // halves of that matter: this used to run in the middle of the preview's own
  // frame, where presenting a second swapchain flushes everything queued for
  // the first one.
  //
  // The result is kept rather than discarded: a fully covered window gets DXGI's
  // occluded-present throttle, and every present then takes far longer than it
  // looks like it should. The main window handles that the same way.
  const HRESULT hr = swapchain_->Present(0, 0);
  occluded_ = hr == DXGI_STATUS_OCCLUDED;

  // And unbind. Defensive now rather than necessary -- the preview sets its own
  // targets at the start of every pass -- but leaving a presented back buffer
  // bound to the shared immediate context is the sort of thing that only shows
  // up later, in something unrelated.
  ID3D11RenderTargetView* none[] = {nullptr};
  ctx_->OMSetRenderTargets(1, none, nullptr);

  ImGui::SetCurrentContext(previous_);
  previous_ = nullptr;
}

}  // namespace cap
