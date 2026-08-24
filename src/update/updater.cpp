#include "update/updater.h"

// common.h pulls in windows.h, and both of the headers below need it to have
// been seen first -- shellapi.h in particular is a wall of errors without it.
#include "common.h"

#include <shellapi.h>
#include <winhttp.h>

#include <cstdlib>
#include <vector>

#include "i18n.h"
#include "json.h"

namespace cap {
namespace {

const wchar_t kApiHost[] = L"api.github.com";
const wchar_t kApiPath[] = L"/repos/NuclearMeltdown/CapView/releases/latest";
const char kAssetName[] = "CapView.exe";

struct Handles {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  ~Handles() {
    if (request) ::WinHttpCloseHandle(request);
    if (connect) ::WinHttpCloseHandle(connect);
    if (session) ::WinHttpCloseHandle(session);
  }
};

// GitHub refuses requests without a user agent, and the API wants to be told
// which version of itself to speak.
bool Fetch(const std::wstring& host, const std::wstring& path, bool api,
           std::string* out, UpdateError* error, int* httpStatus) {
  Handles h;
  h.session = ::WinHttpOpen(L"CapView-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h.session) {
    if (error) *error = UpdateError::NoNetwork;
    return false;
  }
  const DWORD timeout = 20000;
  ::WinHttpSetTimeouts(h.session, timeout, timeout, timeout, timeout);

  h.connect = ::WinHttpConnect(h.session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!h.connect) {
    if (error) *error = UpdateError::NoServer;
    return false;
  }
  h.request = ::WinHttpOpenRequest(h.connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!h.request) {
    if (error) *error = UpdateError::NoRequest;
    return false;
  }
  const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n"
                               : L"Accept: application/octet-stream\r\n";
  if (!::WinHttpSendRequest(h.request, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(h.request, nullptr)) {
    if (error) *error = UpdateError::NoAnswer;
    return false;
  }

  DWORD status = 0, size = sizeof(status);
  ::WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    if (error) *error = UpdateError::HttpStatus;
    if (httpStatus) *httpStatus = (int)status;
    return false;
  }

  out->clear();
  for (;;) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(h.request, &available) || available == 0) break;
    const size_t offset = out->size();
    out->resize(offset + available);
    DWORD read = 0;
    if (!::WinHttpReadData(h.request, out->data() + offset, available, &read)) {
      if (error) *error = UpdateError::Transfer;
      return false;
    }
    out->resize(offset + read);
    if (read == 0) break;
  }
  return true;
}

// "v1.2.3" against "1.2" and so on. Missing parts count as zero, so v1.1 is
// newer than v1 and the same as v1.1.0.
std::vector<int> Parts(const std::string& text) {
  std::vector<int> parts;
  size_t i = 0;
  while (i < text.size() && !isdigit((unsigned char)text[i])) ++i;
  int value = 0;
  bool any = false;
  for (; i < text.size(); ++i) {
    if (isdigit((unsigned char)text[i])) {
      value = value * 10 + (text[i] - '0');
      any = true;
    } else if (text[i] == '.') {
      parts.push_back(value);
      value = 0;
      any = false;
    } else {
      break;
    }
  }
  if (any) parts.push_back(value);
  return parts;
}

bool IsNewer(const std::string& candidate, const std::string& current) {
  const std::vector<int> a = Parts(candidate);
  const std::vector<int> b = Parts(current);
  if (a.empty()) return false;
  for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

std::wstring ExePath() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, n);
}

bool SplitUrl(const std::string& url, std::wstring* host, std::wstring* path) {
  const std::wstring wide = ToWide(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;
  if (!::WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts)) return false;
  *host = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
  *path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) {
    path->append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  return true;
}

}  // namespace

std::string UpdateErrorText(const UpdateStatus& status) {
  switch (status.error) {
    case UpdateError::NoNetwork:
      return T("Keine Netzwerkverbindung möglich.", "No network connection available.");
    case UpdateError::NoServer:
      return T("Server nicht erreichbar.", "Could not reach the server.");
    case UpdateError::NoRequest:
      return T("Anfrage konnte nicht gestellt werden.", "The request could not be made.");
    case UpdateError::NoAnswer:
      return T("Keine Antwort vom Server.", "No answer from the server.");
    case UpdateError::HttpStatus:
      return std::string(T("Der Server antwortete mit ", "The server answered with ")) +
             std::to_string(status.httpStatus) + ".";
    case UpdateError::Transfer:
      return T("Übertragung abgebrochen.", "The transfer broke off.");
    case UpdateError::Unreadable:
      return T("Die Antwort war nicht lesbar.", "The answer could not be read.");
    case UpdateError::NoAsset:
      return T("Die neue Version enthält keine CapView.exe zum Herunterladen.",
               "That release carries no CapView.exe to download.");
    case UpdateError::NoUrl:
      return T("Keine Download-Adresse.", "No download address.");
    case UpdateError::NotAProgram:
      return T("Die heruntergeladene Datei ist kein Programm.",
               "What came back is not a program.");
    case UpdateError::WriteFailed:
      return T("Konnte nicht in den Programmordner schreiben.",
               "Could not write to the program folder.");
    case UpdateError::MoveAsideFailed:
      return T("Die alte Version ließ sich nicht beiseite legen.",
               "The old build could not be moved aside.");
    case UpdateError::InsertFailed:
      return T("Die neue Version ließ sich nicht einsetzen.",
               "The new build could not be put in place.");
    default:
      return "";
  }
}

const char* Updater::currentVersion() { return kAppVersion; }

void Updater::CleanUpPreviousBuild() {
  const std::wstring old = ExePath() + L".old";
  if (::GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (::DeleteFileW(old.c_str())) {
      CAP_LOG("Vorherige Programmversion entfernt");
    }
  }
}

Updater::~Updater() {
  if (thread_.joinable()) thread_.join();
}

UpdateStatus Updater::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void Updater::SetStatus(const UpdateStatus& s) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = s;
}

void Updater::CheckAsync(bool announce) {
  if (busy_.exchange(true, std::memory_order_acq_rel)) return;
  announceNext_ = announce;
  if (thread_.joinable()) thread_.join();
  thread_ = std::thread(&Updater::Run, this, false);
}

void Updater::InstallAsync() {
  if (busy_.exchange(true, std::memory_order_acq_rel)) return;
  if (thread_.joinable()) thread_.join();
  thread_ = std::thread(&Updater::Run, this, true);
}

void Updater::Run(bool install) {
  UpdateStatus s = status();

  if (!install) {
    s.state = UpdateStatus::State::Checking;
    s.error = UpdateError::None;
    s.announce = announceNext_;
    SetStatus(s);

    std::string body;
    UpdateError error = UpdateError::None;
    if (!Fetch(kApiHost, kApiPath, true, &body, &error, &s.httpStatus)) {
      s.state = UpdateStatus::State::Failed;
      s.error = error;
      SetStatus(s);
      busy_.store(false, std::memory_order_release);
      return;
    }

    std::string parseError;
    const json::Value root = json::Parse(body, &parseError);
    if (!root.IsObject()) {
      s.state = UpdateStatus::State::Failed;
      s.error = UpdateError::Unreadable;
      SetStatus(s);
      busy_.store(false, std::memory_order_release);
      return;
    }

    s.latestVersion = root["tag_name"].AsString();
    s.notes = root["body"].AsString();
    if (s.notes.size() > 1200) s.notes = s.notes.substr(0, 1200) + " ...";

    // The asset carrying the program itself. A release without one is a release
    // this cannot install, and saying so beats pretending otherwise.
    downloadUrl_.clear();
    const json::Value& assets = root["assets"];
    for (size_t i = 0; i < assets.Size(); ++i) {
      if (assets.At(i)["name"].AsString() == kAssetName) {
        downloadUrl_ = assets.At(i)["browser_download_url"].AsString();
        break;
      }
    }

    const bool newer = IsNewer(s.latestVersion, currentVersion());
    s.state = newer ? UpdateStatus::State::Available : UpdateStatus::State::UpToDate;
    if (newer && downloadUrl_.empty()) {
      s.state = UpdateStatus::State::Failed;
      s.error = UpdateError::NoAsset;
    }
    CAP_LOG("Update-Prüfung: installiert %s, neueste %s -> %s", currentVersion(),
            s.latestVersion.c_str(), newer ? "neuer verfügbar" : "aktuell");
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // ---- install ----
  s.state = UpdateStatus::State::Downloading;
  s.percent = 0;
  s.error = UpdateError::None;
  SetStatus(s);

  std::wstring host, path;
  if (downloadUrl_.empty() || !SplitUrl(downloadUrl_, &host, &path)) {
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::NoUrl;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  std::string data;
  UpdateError error = UpdateError::None;
  if (!Fetch(host, path, false, &data, &error, &s.httpStatus)) {
    s.state = UpdateStatus::State::Failed;
    s.error = error;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // Before anything is moved: is this actually a program? Every Windows
  // executable starts with these two bytes, and a redirect page or an error
  // document does not. Overwriting the program with an HTML page would be a
  // remarkably annoying way to find that out later.
  if (data.size() < 256 * 1024 || data[0] != 'M' || data[1] != 'Z') {
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::NotAProgram;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  const std::wstring exe = ExePath();
  const std::wstring fresh = exe + L".new";
  const std::wstring old = exe + L".old";

  HANDLE file = ::CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::WriteFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }
  DWORD written = 0;
  const bool wrote =
      ::WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) != FALSE &&
      written == data.size();
  ::CloseHandle(file);
  if (!wrote) {
    ::DeleteFileW(fresh.c_str());
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::WriteFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  // The running image cannot be overwritten, but it can be renamed out of the
  // way -- and if the second step fails, the first is put back, so a failed
  // update leaves the program exactly as it was rather than gone.
  ::DeleteFileW(old.c_str());
  if (!::MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(fresh.c_str());
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::MoveAsideFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }
  if (!::MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    ::DeleteFileW(fresh.c_str());
    s.state = UpdateStatus::State::Failed;
    s.error = UpdateError::InsertFailed;
    SetStatus(s);
    busy_.store(false, std::memory_order_release);
    return;
  }

  CAP_LOG("Update auf %s eingesetzt, Neustart steht aus", s.latestVersion.c_str());
  s.state = UpdateStatus::State::Ready;
  s.percent = 100;
  SetStatus(s);
  busy_.store(false, std::memory_order_release);
}

bool Updater::RestartIntoNewBuild() const {
  if (status().state != UpdateStatus::State::Ready) return false;
  const std::wstring exe = ExePath();
  const std::wstring dir = ExeDirectory();
  const HINSTANCE result =
      ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
  return (INT_PTR)result > 32;
}

}  // namespace cap
