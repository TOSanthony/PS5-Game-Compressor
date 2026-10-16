// Game Compressor TV app - Interface languages (English and French).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strings are written in English in the code and looked up by their exact text:
// an entry missing from the French table is shown in English. {name} marks a
// value filled in by tr().

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace gc::i18n
{

enum class Language : int
{
    automatic = 0, // follow the console's language
    english = 1,
    french = 2,
};

// The console's SCE system language id (1 = English, 2 = French, ...).
void set_system_language(int system_language);
void set_language(Language language);
Language language();
// True when the interface is currently shown in French.
bool french();

using Args = std::initializer_list<std::pair<const char *, std::string>>;
std::string tr(std::string_view english, Args args = {});

} // namespace gc::i18n

namespace gc
{
using i18n::tr;
}
