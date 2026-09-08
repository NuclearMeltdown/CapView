#include "update/release_source.h"

#include <windows.h>

#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "json.h"

namespace cap {
namespace {

// 1340564357 is this repository's number. It was CapView when the number was
// issued and it will keep the number whatever the repository is called next,
// including after being handed to a different account. Asking GitHub for a
// repository that has moved answers with a redirect to exactly this form.
const ReleaseSource kSource = {
    L"api.github.com",
    L"/repositories/1340564357/releases/latest",
    L"/repos/NuclearMeltdown/qBlank/releases/latest",
    "https://github.com/NuclearMeltdown/qBlank/releases",
    "https://nuclearmeltdown.github.io/qBlank/",
};

// Named after the account rather than the program, because the account is the
// part of this that has never changed. GitHub only insists that there is one.
const wchar_t kAgent[] = L"NuclearMeltdown-Updater";

#if defined(_M_ARM64)
const char kArchLabel[] = "app-arm64";
#elif defined(_M_X64)
const char kArchLabel[] = "app-x64";
#else
const char kArchLabel[] = "app-x86";
#endif

// Kept local rather than taken from common.h: the migrator compiles this file
// too, and it has no business pulling in the rest of the program.
std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

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

bool EndsWith(const std::string& text, const char* tail) {
  const size_t n = std::char_traits<char>::length(tail);
  if (text.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower((unsigned char)text[text.size() - n + i]) !=
        std::tolower((unsigned char)tail[i])) {
      return false;
    }
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

// "2026-09-08T12:34:56Z" -> 20260908. Zero when it does not look like a date.
long DayFromIso(const std::string& text) {
  if (text.size() < 10 || text[4] != '-' || text[7] != '-') return 0;
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
    if (!isdigit((unsigned char)text[i])) return 0;
  }
  return std::atol(text.substr(0, 4).c_str()) * 10000 +
         std::atol(text.substr(5, 2).c_str()) * 100 + std::atol(text.substr(8, 2).c_str());
}

// The day this file was compiled, in the same shape. __DATE__ is "Sep  8 2026".
long BuildDay() {
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;
  const char* found = strstr(months, std::string(date, 3).c_str());
  if (!found) return 0;
  const long month = (long)((found - months) / 3) + 1;
  return std::atol(date + 7) * 10000 + month * 100 + std::atol(date + 4);
}

void ReadAssets(const json::Value& root, Release* out) {
  const json::Value& assets = root["assets"];
  for (size_t i = 0; i < assets.Size(); ++i) {
    ReleaseAsset asset;
    asset.name = assets.At(i)["name"].AsString();
    asset.label = assets.At(i)["label"].AsString();
    asset.url = assets.At(i)["browser_download_url"].AsString();
    asset.size = (long long)assets.At(i)["size"].AsNumber();
    if (!asset.url.empty()) out->assets.push_back(asset);
  }
}

bool FetchFrom(const wchar_t* path, Release* out, FetchError* error, int* httpStatus) {
  std::string body;
  if (!HttpGet(kSource.host, path, true, &body, error, httpStatus)) return false;

  std::string parseError;
  const json::Value root = json::Parse(body, &parseError);
  if (!root.IsObject()) {
    if (error) *error = FetchError::Unreadable;
    return false;
  }
  out->tag = root["tag_name"].AsString();
  out->notes = root["body"].AsString();
  out->pageUrl = root["html_url"].AsString();
  out->publishedAt = root["published_at"].AsString();
  out->assets.clear();
  ReadAssets(root, out);
  return true;
}

}  // namespace

const ReleaseSource& Releases() { return kSource; }

// GitHub refuses requests without a user agent, and the API wants to be told
// which version of itself to speak.
bool HttpGet(const std::wstring& host, const std::wstring& path, bool api, std::string* out,
             FetchError* error, int* httpStatus) {
  Handles h;
  h.session = ::WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h.session) {
    if (error) *error = FetchError::NoNetwork;
    return false;
  }
  const DWORD timeout = 20000;
  ::WinHttpSetTimeouts(h.session, timeout, timeout, timeout, timeout);

  h.connect = ::WinHttpConnect(h.session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!h.connect) {
    if (error) *error = FetchError::NoServer;
    return false;
  }
  h.request = ::WinHttpOpenRequest(h.connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!h.request) {
    if (error) *error = FetchError::NoRequest;
    return false;
  }
  const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n"
                               : L"Accept: application/octet-stream\r\n";
  if (!::WinHttpSendRequest(h.request, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(h.request, nullptr)) {
    if (error) *error = FetchError::NoAnswer;
    return false;
  }

  DWORD status = 0, size = sizeof(status);
  ::WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    if (error) *error = FetchError::HttpStatus;
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
      if (error) *error = FetchError::Transfer;
      return false;
    }
    out->resize(offset + read);
    if (read == 0) break;
  }
  return true;
}

bool FetchLatestRelease(Release* out, FetchError* error, int* httpStatus) {
  if (FetchFrom(kSource.byNumber, out, error, httpStatus)) return true;
  // The number is the route that survives renaming, so it is tried first. The
  // name is here for the day GitHub stops answering to numbers.
  return FetchFrom(kSource.byName, out, error, httpStatus);
}

const ReleaseAsset* PickLabelled(const Release& release, const char* label) {
  for (const ReleaseAsset& asset : release.assets) {
    if (asset.label == label) return &asset;
  }
  return nullptr;
}

const ReleaseAsset* PickProgram(const Release& release) {
  if (const ReleaseAsset* exact = PickLabelled(release, kArchLabel)) return exact;
  if (const ReleaseAsset* plain = PickLabelled(release, "app")) return plain;

  // No labels: take the largest executable that is not the migrator. The
  // migrator is padded to a few hundred kilobytes and the program is megabytes,
  // so this lands on the right one even if the labels were forgotten entirely.
  const ReleaseAsset* best = nullptr;
  for (const ReleaseAsset& asset : release.assets) {
    if (asset.label == "migrator") continue;
    if (!EndsWith(asset.name, ".exe")) continue;
    if (!best || asset.size > best->size) best = &asset;
  }
  return best;
}

bool IsNewerRelease(const Release& release, const std::string& current) {
  const std::vector<int> a = Parts(release.tag);
  const std::vector<int> b = Parts(current);
  if (!a.empty()) {
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
      const int x = i < a.size() ? a[i] : 0;
      const int y = i < b.size() ? b[i] : 0;
      if (x != y) return x > y;
    }
    return false;
  }

  // A tag with no numbers in it at all -- a naming scheme this build has never
  // seen. Nothing sensible can be compared, so fall back to the calendar: a
  // release published after this was built is newer than this.
  const long published = DayFromIso(release.publishedAt);
  const long built = BuildDay();
  return published != 0 && built != 0 && published > built;
}

}  // namespace cap
