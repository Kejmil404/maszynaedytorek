/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#pragma once

#include <filesystem>
#include <string>
#include <vector>

// the file dialog of the system, for the editor to ask for files of the user: the one of Windows, elsewhere that of zenity or
// kdialog when one is there. it waits for the user, so it's called aside of the frame, as in std::async
namespace editor_file_dialog
{

struct request
{
	std::string title;
	bool folder{false}; // a folder is chosen, not files
	bool many{true}; // more than one file can be chosen
	std::string filter_name; // as "Terrain model"
	std::vector<std::string> extensions; // without the dot, as "asc"; empty: all files
	void *owner{nullptr}; // the window the dialog belongs to (HWND on Windows), or none
};

struct result
{
	std::vector<std::filesystem::path> paths; // empty when the user gave up
	bool available{true}; // false: the system has no dialog the editor can open, the path is to be typed
};

result open(request const &Request);

} // namespace editor_file_dialog
