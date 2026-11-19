/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#pragma once

// what the parts of the field of work of the events share: the files of the editor and the words of the scenery they write

#include <string>
#include <string_view>
#include <vector>

namespace editor_events
{

// the name of a file of the editor: the name of the scenery with the suffix in place of its extension; empty with no scenery
std::string editor_file(std::string_view const Suffix);
// writes the include of the editor anew, when what it holds changed, and adds it once at the end of the scenery. Empty: there's
// nothing in it, and a file which isn't there isn't made. true if the include was added to Rootstatements
bool write_include(std::string const &File, std::string const &Content, bool const Empty, bool &Included, std::vector<std::string> &Rootstatements);
// an hour of a launcher, HHMM
std::string hhmm(int const Time);
// a field of the line written as a word of the scenery: without the separators of the line, and * where there's nothing
std::string word_of(std::string_view const Text);
// the fields of the line, the empty ones and the one at the end included
std::vector<std::string> fields_of(std::string_view Line);
// a name of a node and its events: letters, digits, _ and -, as the scenery reads them
std::string name_of_typed(std::string_view const Typed);

} // namespace editor_events
