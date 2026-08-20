#include "i18n.h"

#include <atomic>

namespace cap {
namespace {

// Read from the UI thread and written only when the setting changes, so a
// plain atomic is enough.
std::atomic<Language> g_language{Language::German};

}  // namespace

void SetLanguage(Language language) {
  g_language.store(language, std::memory_order_relaxed);
}

Language CurrentLanguage() {
  return g_language.load(std::memory_order_relaxed);
}

const char* T(const char* de, const char* en) {
  return g_language.load(std::memory_order_relaxed) == Language::German ? de : en;
}

}  // namespace cap
