#include "record/recorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "render/video_renderer.h"  // for the readback pixel format

namespace cap {
namespace {

int64_t QpcNow() {
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double QpcSeconds(int64_t ticks) {
  static const double freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return (double)f.QuadPart;
  }();
  return (double)ticks / freq;
}

std::wstring TimestampedName(RecordContainer container) {
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t buf[64];
  swprintf_s(buf, L"CapView_%04u-%02u-%02u_%02u-%02u-%02u.%s", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             container == RecordContainer::Mp4 ? L"mp4" : L"mkv");
  return buf;
}

}  // namespace

Recorder::~Recorder() {
  Stop();
}

void Recorder::Fail(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (error_.empty()) error_ = message;
  }
  failed_.store(true, std::memory_order_relaxed);
  CAP_ERR("Aufnahme: %s", message.c_str());
}

// ------------------------------------------------------------- command line

std::wstring Recorder::BuildCommandLine(const RecordSettings& settings,
                                        const EncoderInfo& encoder,
                                        const std::wstring& audioPipe,
                                        const std::wstring& outFile, int audioRate) const {
  std::wstring cmd;
  cmd += L"-hide_banner -loglevel error -y";

  // Input 0: raw frames exactly as the readback hands them over. Declaring the
  // rate here is what makes the output constant frame rate; the writer thread
  // guarantees that many frames per second actually arrive.
  // The pixel format comes from the renderer rather than being spelled out
  // here: these two have to agree byte for byte, and a literal in this file is
  // exactly how they came apart once already.
  cmd += L" -f rawvideo -pix_fmt ";
  cmd += ToWide(VideoRenderer::kReadbackPixelFormat);
  cmd += L" -s " + std::to_wstring(width_) + L"x" + std::to_wstring(height_);
  cmd += L" -r " + ToWide(Format("%.6f", fps_));
  cmd += L" -i pipe:0";

  // Input 1: the untouched captured audio.
  if (audioRate > 0) {
    cmd += L" -f f32le -ar " + std::to_wstring(audioRate) + L" -ac 2";
    cmd += L" -i \"" + audioPipe + L"\"";
  }

  cmd += L" -map 0:v";
  if (audioRate > 0) cmd += L" -map 1:a";

  cmd += L" -c:v " + ToWide(encoder.ffmpegName);
  // 4:2:0 eight bit: the one format every hardware encoder and every player
  // agrees on. Anything wider would exclude the very cards this is meant for.
  cmd += L" -pix_fmt nv12";
  cmd += L" -b:v " + std::to_wstring(settings.bitrateKbps) + L"k";
  cmd += L" -maxrate " + std::to_wstring(settings.bitrateKbps) + L"k";
  cmd += L" -bufsize " + std::to_wstring(settings.bitrateKbps * 2) + L"k";

  // Speed presets only for the software encoders. For hardware the vendor
  // defaults beat anything guessed here.
  if (!encoder.hardware) {
    cmd += L" -preset " + ToWide(RecordSpeedName((int)settings.speed));
  }

  if (audioRate > 0) cmd += L" -c:a aac -b:a 192k";

  cmd += L" \"" + outFile + L"\"";
  return cmd;
}

// -------------------------------------------------------------------- start

bool Recorder::Start(const RecordSettings& settings, const FfmpegInfo& ffmpeg, int width,
                     int height, double sourceFps, int audioRate, AudioPullFn pullAudio,
                     std::string* error) {
  Stop();

  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    CAP_ERR("Aufnahme konnte nicht gestartet werden: %s", msg.c_str());
    Stop();
    return false;
  };

  if (!ffmpeg.found) {
    return fail(T("ffmpeg wurde nicht gefunden.", "ffmpeg was not found."));
  }
  if (width <= 0 || height <= 0) {
    return fail(T("Kein Bild zum Aufnehmen.", "No picture to record."));
  }

  const EncoderInfo* encoder = settings.encoder == RecordEncoder::Auto
                                   ? ffmpeg.BestAvailable()
                                   : ffmpeg.Find(settings.encoder);
  if (!encoder || !encoder->available) {
    return fail(T("Kein verwendbarer Encoder. Encoder-Test in den Einstellungen ausführen.",
                  "No usable encoder. Run the encoder test in the settings."));
  }

  // Odd sizes break 4:2:0 chroma. Rather than refuse, round down by a pixel --
  // the alternative is telling someone their 1439 pixel capture cannot be
  // recorded, which helps nobody.
  width_ = width & ~1;
  height_ = height & ~1;
  fps_ = settings.fps > 0.0 ? settings.fps : (sourceFps > 1.0 ? sourceFps : 60.0);
  audioRate_ = audioRate;
  frameBytes_ = (size_t)width_ * (size_t)height_ * 4;
  pullAudio_ = std::move(pullAudio);

  std::wstring folder = settings.outputFolder.empty() ? DefaultRecordFolder()
                                                      : ToWide(settings.outputFolder);
  if (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
  if (!EnsureFolder(folder)) {
    return fail(T("Zielordner konnte nicht angelegt werden: ", "Could not create the folder: ") +
                ToUtf8(folder));
  }
  const std::wstring outFile = folder + L"\\" + TimestampedName(settings.container);

  // --- pipes -------------------------------------------------------------
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE videoRead = nullptr;
  // A generous pipe buffer keeps a brief encoder stall from reaching back to
  // the writer thread.
  if (!::CreatePipe(&videoRead, &videoPipe_, &sa, 1 << 22)) {
    return fail(T("Videopipe konnte nicht erstellt werden.", "Could not create the video pipe."));
  }
  ::SetHandleInformation(videoPipe_, HANDLE_FLAG_INHERIT, 0);

  if (audioRate_ > 0) {
    // ffmpeg cannot take a second anonymous pipe -- only stdin is inheritable
    // as a standard handle -- so audio goes through a named pipe it opens by
    // path.
    audioPipeName_ = L"\\\\.\\pipe\\capview_audio_" + std::to_wstring(::GetCurrentProcessId());
    audioPipe_ = ::CreateNamedPipeW(audioPipeName_.c_str(),
                                    PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, nullptr);
    if (audioPipe_ == INVALID_HANDLE_VALUE) {
      audioPipe_ = nullptr;
      ::CloseHandle(videoRead);
      return fail(T("Audiopipe konnte nicht erstellt werden.", "Could not create the audio pipe."));
    }
  }

  // --- process -----------------------------------------------------------
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = videoRead;
  si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);

  // ffmpeg reports everything on stderr. A windowed app has no usable standard
  // error, so without this pipe a failed encode leaves nothing but an exit
  // code -- which is how the first version of this managed to say nothing at
  // all about why it produced no file.
  HANDLE stderrWrite = nullptr;
  if (::CreatePipe(&stderrPipe_, &stderrWrite, &sa, 1 << 16)) {
    ::SetHandleInformation(stderrPipe_, HANDLE_FLAG_INHERIT, 0);
    si.hStdError = stderrWrite;
  } else {
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  }

  const std::wstring args =
      BuildCommandLine(settings, *encoder, audioPipeName_, outFile, audioRate_);
  std::wstring commandLine = L"\"" + ToWide(ffmpeg.path) + L"\" " + args;
  CAP_LOG("ffmpeg %s", ToUtf8(args).c_str());

  std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
  mutableCmd.push_back(L'\0');

  PROCESS_INFORMATION pi = {};
  const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  ::CloseHandle(videoRead);  // the child owns it now
  if (stderrWrite) ::CloseHandle(stderrWrite);
  if (!ok) {
    return fail(T("ffmpeg konnte nicht gestartet werden.", "Could not start ffmpeg."));
  }
  ::CloseHandle(pi.hThread);
  process_ = pi.hProcess;

  // --- state -------------------------------------------------------------
  for (int i = 0; i < 3; ++i) slots_[i].assign(frameBytes_, 0);
  readyIdx_ = -1;
  heldIdx_ = -1;
  audioFramesWritten_.store(0, std::memory_order_relaxed);
  videoFramesWritten_.store(0, std::memory_order_relaxed);
  duplicated_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  bytesWritten_.store(0, std::memory_order_relaxed);
  startQpc_ = QpcNow();
  lastAudioSeen_ = 0;
  lastAudioProgressQpc_ = 0;
  audioStallLogged_ = false;
  failed_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    file_ = ToUtf8(outFile);
    encoderLabel_ = encoder->label;
    error_.clear();
  }

  running_.store(true, std::memory_order_relaxed);
  if (stderrPipe_) stderrThread_ = std::thread(&Recorder::StderrThread, this);
  if (audioRate_ > 0) audioThread_ = std::thread(&Recorder::AudioThread, this);
  videoThread_ = std::thread(&Recorder::VideoThread, this);

  CAP_LOG("Aufnahme gestartet: %s, %dx%d @ %.3f fps, %s", ToUtf8(outFile).c_str(), width_,
          height_, fps_, encoder->label.c_str());
  return true;
}

void Recorder::Stop() {
  const bool wasRunning = running_.exchange(false, std::memory_order_relaxed);

  if (videoThread_.joinable()) videoThread_.join();
  if (audioThread_.joinable()) audioThread_.join();

  // Closing the pipes is what tells ffmpeg the streams ended. It then writes
  // the container index and exits -- an MP4 killed before that point has no
  // moov atom and will not play at all.
  if (videoPipe_) {
    ::CloseHandle(videoPipe_);
    videoPipe_ = nullptr;
  }
  if (audioPipe_) {
    ::CloseHandle(audioPipe_);
    audioPipe_ = nullptr;
  }

  // The stderr reader ends by itself once the child closes its end, which
  // happens when ffmpeg exits -- so it is joined after the wait below.
  if (process_) {
    if (::WaitForSingleObject(process_, 15000) == WAIT_TIMEOUT) {
      CAP_WARN("ffmpeg beendet sich nicht, wird abgebrochen - Datei kann unvollständig sein");
      ::TerminateProcess(process_, 1);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    if (wasRunning) {
      CAP_LOG("Aufnahme beendet, ffmpeg-Exitcode %lu, %llu Bilder", code,
              (unsigned long long)videoFramesWritten_.load());
    }
    ::CloseHandle(process_);
    process_ = nullptr;
  }

  if (stderrThread_.joinable()) stderrThread_.join();
  if (stderrPipe_) {
    ::CloseHandle(stderrPipe_);
    stderrPipe_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    for (int i = 0; i < 3; ++i) slots_[i].clear();
    readyIdx_ = -1;
    heldIdx_ = -1;
  }
  pullAudio_ = nullptr;
}

// --------------------------------------------------------------- frame feed

int Recorder::PickWriteSlotLocked() const {
  for (int i = 0; i < 3; ++i) {
    if (i != readyIdx_ && i != heldIdx_) return i;
  }
  return 0;  // unreachable with three slots and two reserved indices
}

void Recorder::PushVideo(const uint8_t* pixels, int stride) {
  if (!running_.load(std::memory_order_relaxed) || !pixels) return;

  int slot;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (slots_[0].size() != frameBytes_) return;  // not started, or size changed
    slot = PickWriteSlotLocked();
  }

  // Copy outside the lock: the chosen slot is neither the one waiting to be
  // written nor the one the writer holds, so nobody else touches it. The rows
  // are repacked because a staging texture's pitch is rarely width * 4, and
  // rawvideo has no concept of padding.
  const size_t rowBytes = (size_t)width_ * 4;
  uint8_t* dst = slots_[slot].data();
  for (int y = 0; y < height_; ++y) {
    memcpy(dst + (size_t)y * rowBytes, pixels + (size_t)y * (size_t)stride, rowBytes);
  }

  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    // A frame still waiting here never made it into the file: the timeline did
    // not need it, because the source runs faster than the recording rate.
    if (readyIdx_ >= 0) dropped_.fetch_add(1, std::memory_order_relaxed);
    readyIdx_ = slot;
  }
}

bool Recorder::WriteAll(HANDLE pipe, const uint8_t* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    DWORD chunk = 0;
    const DWORD want = (DWORD)std::min<size_t>(size - written, 1u << 20);
    if (!::WriteFile(pipe, data + written, want, &chunk, nullptr) || chunk == 0) return false;
    written += chunk;
  }
  bytesWritten_.fetch_add(size, std::memory_order_relaxed);
  return true;
}

// ------------------------------------------------------------ writer threads

void Recorder::VideoThread() {
  while (running_.load(std::memory_order_relaxed)) {
    // Take the newest frame and keep holding it: when the timeline needs more
    // frames than actually arrived, this is what gets repeated.
    const uint8_t* held = nullptr;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lock(frameMutex_);
      if (readyIdx_ >= 0) {
        heldIdx_ = readyIdx_;
        readyIdx_ = -1;
        fresh = true;
      }
      if (heldIdx_ >= 0 && slots_[heldIdx_].size() == frameBytes_) {
        held = slots_[heldIdx_].data();
      }
    }
    if (!held) {
      ::Sleep(2);
      continue;
    }

    // How many frames the timeline should contain by now.
    //
    // The audio is the better master once it flows, because the card's sample
    // clock is steadier than the PC's. But it cannot be the master from the
    // start: ffmpeg opens its inputs in order and blocks reading video before it
    // ever opens the audio pipe, so waiting for audio samples before writing the
    // first frame deadlocks both sides. Until audio appears -- and again if it
    // stops appearing -- the wall clock stands in.
    const uint64_t audioFrames = audioFramesWritten_.load(std::memory_order_relaxed);
    const int64_t now = QpcNow();
    if (audioFrames != lastAudioSeen_) {
      lastAudioSeen_ = audioFrames;
      lastAudioProgressQpc_ = now;
    }
    const bool audioStalled =
        lastAudioProgressQpc_ != 0 && QpcSeconds(now - lastAudioProgressQpc_) > 2.0;
    const bool audioIsMaster = audioRate_ > 0 && audioFrames > 0 && !audioStalled;

    if (audioStalled && !audioStallLogged_) {
      audioStallLogged_ = true;
      CAP_WARN("Aufnahme: kein Ton mehr, Bildtakt läuft auf der Systemuhr weiter");
    }

    uint64_t target = 0;
    if (audioIsMaster) {
      target = (uint64_t)((double)audioFrames / (double)audioRate_ * fps_);
    } else {
      target = (uint64_t)(QpcSeconds(now - startQpc_) * fps_);
    }

    uint64_t written = videoFramesWritten_.load(std::memory_order_relaxed);
    if (written >= target) {
      ::Sleep(1);
      continue;
    }

    // Every frame in the timeline has to be written -- skipping one would make
    // the video shorter than the audio and desync the file for good. If the
    // encoder stalls, the pipe blocks here, which is exactly where the waiting
    // belongs.
    for (uint64_t i = written; i < target && running_.load(std::memory_order_relaxed); ++i) {
      if (!WriteAll(videoPipe_, held, frameBytes_)) {
        Fail(T("Verbindung zu ffmpeg abgebrochen.", "Lost the connection to ffmpeg."));
        running_.store(false, std::memory_order_relaxed);
        return;
      }
      videoFramesWritten_.fetch_add(1, std::memory_order_relaxed);
      if (!fresh || i > written) duplicated_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Recorder::AudioThread() {
  // Wait for ffmpeg to open its end. Overlapped, so a ffmpeg that never starts
  // cannot leave this thread stuck and hang the whole shutdown.
  HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event) return;

  OVERLAPPED ov = {};
  ov.hEvent = event;
  bool connected = false;
  if (::ConnectNamedPipe(audioPipe_, &ov)) {
    connected = true;
  } else if (::GetLastError() == ERROR_PIPE_CONNECTED) {
    connected = true;
  } else if (::GetLastError() == ERROR_IO_PENDING) {
    while (running_.load(std::memory_order_relaxed)) {
      if (::WaitForSingleObject(event, 100) == WAIT_OBJECT_0) {
        connected = true;
        break;
      }
    }
  }
  if (!connected) {
    ::CancelIo(audioPipe_);
    ::CloseHandle(event);
    return;
  }

  std::vector<float> buffer(4096 * 2);
  while (running_.load(std::memory_order_relaxed)) {
    const size_t got = pullAudio_ ? pullAudio_(buffer.data(), 4096) : 0;
    if (got == 0) {
      ::Sleep(2);
      continue;
    }

    const size_t bytes = got * 2 * sizeof(float);
    ::ResetEvent(event);
    OVERLAPPED wov = {};
    wov.hEvent = event;
    DWORD wrote = 0;
    if (!::WriteFile(audioPipe_, buffer.data(), (DWORD)bytes, &wrote, &wov)) {
      if (::GetLastError() != ERROR_IO_PENDING) {
        Fail(T("Audiopipe abgebrochen.", "The audio pipe broke."));
        running_.store(false, std::memory_order_relaxed);
        break;
      }
      if (!::GetOverlappedResult(audioPipe_, &wov, &wrote, TRUE)) {
        Fail(T("Audiopipe abgebrochen.", "The audio pipe broke."));
        running_.store(false, std::memory_order_relaxed);
        break;
      }
    }
    bytesWritten_.fetch_add(wrote, std::memory_order_relaxed);
    audioFramesWritten_.fetch_add(got, std::memory_order_relaxed);
  }

  ::CloseHandle(event);
}

void Recorder::StderrThread() {
  std::string pending;
  char buffer[1024];
  DWORD read = 0;
  while (::ReadFile(stderrPipe_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    pending.append(buffer, read);
    size_t nl;
    while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
      const std::string line = Trim(pending.substr(0, nl));
      pending.erase(0, nl + 1);
      if (!line.empty()) {
        CAP_WARN("ffmpeg: %s", line.c_str());
        std::lock_guard<std::mutex> lock(infoMutex_);
        if (error_.empty()) error_ = line;
      }
    }
    if (pending.size() > 4096) pending.clear();
  }
}

uint64_t Recorder::outputFileSize() const {
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (file_.empty()) return 0;
    path = ToWide(file_);
  }
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

RecordStats Recorder::stats() const {
  RecordStats s;
  s.running = running_.load(std::memory_order_relaxed);
  s.seconds = startQpc_ ? QpcSeconds(QpcNow() - startQpc_) : 0.0;
  s.videoFrames = videoFramesWritten_.load(std::memory_order_relaxed);
  s.duplicated = duplicated_.load(std::memory_order_relaxed);
  s.dropped = dropped_.load(std::memory_order_relaxed);
  s.bytesWritten = bytesWritten_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(infoMutex_);
    s.file = file_;
    s.encoder = encoderLabel_;
    s.error = error_;
  }
  return s;
}

}  // namespace cap
