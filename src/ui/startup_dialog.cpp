#include "ui/startup_dialog.h"

#include "app_identity.h"
#include "common.h"
#include "config.h"
#include "render/d3d_context.h"
#include "resource.h"
#include "ui/theme.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace cap {
namespace {

// The accent the program starts life with. There are no settings to read one
// from yet -- that is the whole reason this window exists.
constexpr unsigned kDefaultAccent = 0x8B5CF6;

D3DContext* g_d3d = nullptr;
StartupAnswer g_answer = StartupAnswer::Postpone;
bool g_done = false;

// How tall the content turned out to be. The window is created too big and
// trimmed to this after the first frame: a question with one file in it and one
// with three are not the same size, and guessing at the difference leaves either
// a hole under the buttons or a scrollbar.
float g_contentHeight = 0.0f;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
  switch (msg) {
    case WM_SIZE:
      if (g_d3d && wparam != SIZE_MINIMIZED) g_d3d->Resize();
      return 0;
    case WM_CLOSE:
      // Closing decides nothing. Both files stay where they are and the
      // question comes back at the next start, which is the only answer that
      // cannot be given by accident.
      g_answer = StartupAnswer::Postpone;
      g_done = true;
      return 0;
    // No WM_DESTROY handler, and deliberately none. The usual PostQuitMessage
    // belongs to a window that *is* the program; this one is asked before the
    // program exists and hands control back to a caller that still has work to
    // do. A WM_QUIT posted here outlives the window -- the message queue is the
    // thread's, not the window's -- and the main loop would find it waiting on
    // its very first pass and shut down without ever drawing a frame. The loop
    // below ends on g_done, which is what the buttons and WM_CLOSE set.
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void Draw(const StartupQuestion& q, float scale) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##startup", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::TextUnformatted(q.heading.c_str());
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::TextWrapped("%s", q.body.c_str());

  if (!q.rows.empty()) {
    ImGui::Spacing();
    if (ImGui::BeginTable("##files", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
      for (const StartupQuestion::Row& row : q.rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (row.highlight) {
          ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s",
                             row.label.c_str());
        } else {
          ImGui::TextUnformatted(row.label.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", row.detail.c_str());
      }
      ImGui::EndTable();
    }
  }

  // Buttons after the facts, footnote under them: the last thing read before
  // clicking should be what the click cannot break.
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));

  const float buttonHeight = ImGui::GetFrameHeight() * 1.4f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float width = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
  if (ImGui::Button(q.acceptLabel.c_str(), ImVec2(width, buttonHeight))) {
    g_answer = StartupAnswer::Accept;
    g_done = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(q.rejectLabel.c_str(), ImVec2(width, buttonHeight))) {
    g_answer = StartupAnswer::Reject;
    g_done = true;
  }

  if (!q.footnote.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", q.footnote.c_str());
    ImGui::PopTextWrapPos();
  }

  g_contentHeight = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
  ImGui::End();
  (void)scale;
}

// Trims the window to the height the content actually needed and puts it back
// in the middle of the screen.
void FitToContent(HWND hwnd, int clientHeight) {
  RECT window = {}, client = {};
  ::GetWindowRect(hwnd, &window);
  ::GetClientRect(hwnd, &client);
  const int chrome = (window.bottom - window.top) - (client.bottom - client.top);
  const int height = clientHeight + chrome;
  const int width = window.right - window.left;

  RECT work = {0, 0, 0, 0};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  ::SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

StartupAnswer AskAtStartup(HINSTANCE instance, const StartupQuestion& question) {
  g_answer = StartupAnswer::Postpone;
  g_done = false;

  const std::wstring className = WindowClassName(L"StartupDialog");
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = className.c_str();
  wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_QBLANK));
  wc.hIconSm = wc.hIcon;
  if (!::RegisterClassExW(&wc)) return StartupAnswer::Postpone;

  // Centred on the primary screen's work area, at that screen's scaling. There
  // is no saved position to restore -- this window is seen once.
  const UINT dpi = ::GetDpiForSystem();
  const float scale = dpi > 0 ? (float)dpi / 96.0f : 1.0f;
  // Created generously tall and hidden; the first frame measures the content and
  // FitToContent trims it before it is shown.
  const int width = (int)(620 * scale);
  const int height = (int)(760 * scale);
  RECT work = {0, 0, 0, 0};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;

  const HWND hwnd = ::CreateWindowExW(0, className.c_str(), kAppName,
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height,
                                      nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    ::UnregisterClassW(className.c_str(), instance);
    return StartupAnswer::Postpone;
  }

  D3DContext d3d;
  std::string error;
  if (!d3d.Initialize(hwnd, &error)) {
    // No device, no window worth showing. The caller falls back to deciding
    // nothing, which leaves both files untouched.
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(className.c_str(), instance);
    return StartupAnswer::Postpone;
  }
  g_d3d = &d3d;

  const bool dark = ResolveDark(Theme::System);
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  LoadUiFont(17.0f * scale);
  ApplyImGuiTheme(dark, kDefaultAccent);
  ImGui::GetStyle().ScaleAllSizes(scale);

  bool ready = ImGui_ImplWin32_Init(hwnd) && ImGui_ImplDX11_Init(d3d.device(), d3d.context());
  if (ready) {
    ApplyWindowDarkMode(hwnd, dark);

    float clear[4];
    GetBackgroundColor(dark, kDefaultAccent, clear);
    // Vsync on: this window has all the time in the world and no reason to
    // spin a core while somebody reads it.
    bool shown = false;
    while (!g_done) {
      MSG msg;
      bool quit = false;
      while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) quit = true;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
      }
      if (quit) break;

      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      Draw(question, scale);
      ImGui::Render();

      // The first frame is only measured, never shown: it exists to find out how
      // tall the window has to be.
      if (!shown) {
        shown = true;
        FitToContent(hwnd, (int)(g_contentHeight + 0.5f));
        ::ShowWindow(hwnd, SW_SHOW);
        ::SetForegroundWindow(hwnd);
        continue;
      }

      if (d3d.BeginFrame(clear)) {
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        d3d.EndFrame(true);
      } else {
        ::Sleep(16);
      }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
  }

  ImGui::DestroyContext();
  ImGui::SetCurrentContext(previous);
  g_d3d = nullptr;
  d3d.Shutdown();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(className.c_str(), instance);

  // Nothing of this window may be left in the queue when the program starts.
  // The handler above no longer posts a quit, but ImGui's backend and the
  // shutdown of a swap chain both dispatch through the same queue, and the
  // caller's loop treats any WM_QUIT it finds as its own.
  MSG stray;
  while (::PeekMessageW(&stray, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
  }
  return ready ? g_answer : StartupAnswer::Postpone;
}

}  // namespace cap
