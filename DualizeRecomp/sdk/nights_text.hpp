#pragma once

// The game's text language and our own texts in each of its languages (the
// Options pages' labels, values and help lines, written in English).

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace dualize
{
class Memory;
}

namespace dualize::nights
{
// As the game numbers them (its settings' language, its Language window's order).
enum class Language
{
  JAPANESE,
  ENGLISH,
  FRENCH,
  SPANISH,
  GERMAN,
  ITALIAN,
  COUNT,
};
constexpr size_t LANGUAGE_COUNT = static_cast<size_t>(Language::COUNT);

// The language the game's settings have (English before they exist).
Language GameLanguage(const Memory& memory);

// One of our English texts in `language`; the text itself when it has none
// (numbers, sizes) or in English.
std::string_view Translate(std::string_view english, Language language);

// UTF-8 to UTF-16.
std::u16string Utf16(std::string_view text);
}  // namespace dualize::nights
