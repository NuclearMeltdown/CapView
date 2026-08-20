#pragma once

// Language switching.
//
// Strings are written inline at the call site as T("deutsch", "english")
// instead of living in a table behind numeric ids. With a UI this size that
// trade is worth it: the two languages cannot drift apart, every string is
// greppable where it is used, and there is no id bookkeeping to get wrong.

namespace cap {

enum class Language { German, English };

void SetLanguage(Language language);
Language CurrentLanguage();

// Picks the matching variant for the language currently set.
const char* T(const char* de, const char* en);

// Same, for places that build a std::string.
inline const char* Tr(const char* de, const char* en) { return T(de, en); }

}  // namespace cap
