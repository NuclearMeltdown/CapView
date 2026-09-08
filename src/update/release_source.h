#pragma once

// Finding the newest release without relying on anything that can be renamed.
//
// Three things about a project on GitHub can change without notice: the name of
// the repository, the account holding it, and the name of the file inside a
// release. None of the three is used here to find anything.
//
//   * The repository is addressed by its number. A repository keeps its number
//     when it is renamed and when it is handed to another account -- it is the
//     address GitHub's own redirect points at when you ask for one that moved.
//     The owner/name path is kept as a fallback, in case that route ever goes.
//   * The program inside a release is found by its label, not its file name, so
//     the executable can be called anything later on. Upload it with
//     `gh release upload <tag> "Name.exe#app"`.
//   * What comes down is saved under the name the release gives it, so nothing
//     on this side has to know that name in advance.
//
// Between them, an ordinary update survives a rename of the program: the new
// build arrives under the old file name, and corrects it on its first start --
// see AdoptOwnName in app_identity.h. That is the whole reason a rename after
// this one needs no migrator.
//
// This file deliberately depends on nothing but Windows and the JSON reader:
// the migrator shipped for the old name compiles it too, and must stay small.

#include <string>
#include <vector>

namespace cap {

enum class FetchError {
  None,
  NoNetwork,
  NoServer,
  NoRequest,
  NoAnswer,
  HttpStatus,  // see the status argument
  Transfer,
  Unreadable,
};

struct ReleaseAsset {
  std::string name;   // the file name, whatever it happens to be
  std::string label;  // what it is: "app", "migrator", ...
  std::string url;
  long long size = 0;
};

struct Release {
  std::string tag;
  std::string notes;
  std::string pageUrl;      // html_url -- correct even after a rename
  std::string publishedAt;  // ISO 8601, as GitHub writes it
  std::vector<ReleaseAsset> assets;
};

// Every address the program knows, in one place. The two paths say the same
// thing in two ways; the fallbacks are for when there is no answer at all and
// somebody has to be told where to look by hand.
struct ReleaseSource {
  const wchar_t* host;
  const wchar_t* byNumber;
  const wchar_t* byName;
  const char* releasePage;
  const char* website;
};
const ReleaseSource& Releases();

// A plain GET. `api` only picks the Accept header.
bool HttpGet(const std::wstring& host, const std::wstring& path, bool api, std::string* out,
             FetchError* error, int* httpStatus);

// The newest release: by number first, by name if that fails.
bool FetchLatestRelease(Release* out, FetchError* error, int* httpStatus);

// The asset carrying the program itself, or null. Prefers the label, falls back
// to the largest executable that is not the migrator -- the migrator is a few
// hundred kilobytes and the program is megabytes, so the two guards are
// independent of each other.
const ReleaseAsset* PickProgram(const Release& release);

// The asset carrying a given label, or null.
const ReleaseAsset* PickLabelled(const Release& release, const char* label);

// Whether that release is newer than the version given. Compares the numbers in
// the tag; if a future tag carries no numbers at all, falls back to comparing
// the day it was published against the day this was built.
bool IsNewerRelease(const Release& release, const std::string& current);

}  // namespace cap
