#include <windows.h>

#include <cstdio>
#include <string>

#include "app.h"
#include "common.h"
#include "record/ffmpeg_download.h"
#include "record/ffmpeg_locator.h"

namespace {

// Attaches to the parent console if there is one, so a command line run can
// report progress. A windowed program has no console of its own.
void AttachConsoleIfAny() {
  if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* dummy = nullptr;
  freopen_s(&dummy, "CONOUT$", "w", stdout);
  freopen_s(&dummy, "CONOUT$", "w", stderr);
}

// Runs the same download the settings button does, without the window. Useful
// for provisioning a machine from a script -- and the only way to exercise this
// code path without a human clicking.
int FetchFfmpeg() {
  AttachConsoleIfAny();
  // A windowed program handed a console it did not create cannot rely on stdout
  // surviving, so the log file is the dependable record of what happened.
  cap::LogInit(true);
  cap::LogWrite("INFO", "--fetch-ffmpeg gestartet");
  std::printf("\nCapView: hole ffmpeg ...\n");

  cap::FfmpegDownloader downloader;
  if (!downloader.Start(cap::ExeDirectory() + L"ffmpeg")) {
    std::printf("Download konnte nicht gestartet werden.\n");
    return 1;
  }

  std::string last;
  while (downloader.busy()) {
    const std::string message = downloader.message();
    if (message != last) {
      last = message;
      std::printf("  %s\n", message.c_str());
    }
    ::Sleep(200);
  }
  std::printf("  %s\n", downloader.message().c_str());

  const bool ok = downloader.state() == cap::FfmpegDownloader::State::Done;
  if (ok && !downloader.resultPath().empty()) {
    std::printf("  %s\n", downloader.resultPath().c_str());
  }
  cap::LogWrite(ok ? "INFO" : "ERR ", "--fetch-ffmpeg: %s (%s)", downloader.message().c_str(),
                ok ? downloader.resultPath().c_str() : "fehlgeschlagen");
  return ok ? 0 : 1;
}

// Prints what the encoder probe finds, so a recording problem can be diagnosed
// without opening the settings.
int ListEncoders() {
  AttachConsoleIfAny();
  cap::FfmpegInfo info = cap::LocateFfmpeg({});
  if (!info.found) {
    std::printf("\nffmpeg nicht gefunden.\n");
    return 1;
  }
  std::printf("\n%s\n%s\n\n", info.path.c_str(), info.version.c_str());
  cap::ProbeEncoders(&info);
  for (const cap::EncoderInfo& e : info.encoders) {
    std::printf("  %-30s %-9s %s\n", e.label.c_str(), e.hardware ? "Hardware" : "CPU",
                e.available ? "verfügbar" : ("nicht verfügbar  " + e.error).c_str());
  }
  return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int showCmd) {
  // Per monitor DPI so the picture is not stretched by the compositor on a
  // scaled display.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Multi threaded apartment: DirectShow filters and WASAPI both push from
  // their own threads, and MTA keeps those calls free of apartment marshalling.
  cap::ComScope com(COINIT_MULTITHREADED);
  if (!com.ok()) {
    ::MessageBoxW(nullptr, L"COM konnte nicht initialisiert werden.", L"CapView",
                  MB_ICONERROR | MB_OK);
    return 1;
  }

  const std::wstring args = commandLine ? commandLine : L"";
  if (args.find(L"--fetch-ffmpeg") != std::wstring::npos) return FetchFfmpeg();
  if (args.find(L"--list-encoders") != std::wstring::npos) return ListEncoders();

  // 1 ms timer resolution: the wait in the render loop and the second field of
  // a bob deinterlaced frame both need better than the default 15.6 ms.
  ::timeBeginPeriod(1);

  int result = 1;
  {
    cap::App app;
    if (app.Initialize(instance, showCmd)) {
      result = app.Run();
    }
    app.Shutdown();
  }

  ::timeEndPeriod(1);
  cap::LogWrite("INFO", "CapView beendet");
  return result;
}
