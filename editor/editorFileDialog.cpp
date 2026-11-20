/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#include "stdafx.h"
#include "editor/editorFileDialog.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#else
#include <cstdio>
#include <cstdlib>
#endif

#include <array>
#include <string_view>

namespace editor_file_dialog
{

namespace
{

#ifdef _WIN32

std::wstring wide(std::string_view const Text)
{
	if (Text.empty())
		return {};
	auto const size{::MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0)};
	std::wstring result(static_cast<std::size_t>(size), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), result.data(), size);
	return result;
}

// releases a COM object as it goes out of scope
template <class Type_> struct released
{
	Type_ *pointer{nullptr};
	released() = default;
	released(released const &) = delete;
	released &operator=(released const &) = delete;
	~released()
	{
		if (pointer != nullptr)
			pointer->Release();
	}
};

std::filesystem::path path_of(IShellItem &Item)
{
	PWSTR name{nullptr};
	if (FAILED(Item.GetDisplayName(SIGDN_FILESYSPATH, &name)) || name == nullptr)
		return {};
	std::filesystem::path result{name};
	::CoTaskMemFree(name);
	return result;
}

void set_filter(IFileOpenDialog &Dialog, request const &Request)
{
	if (Request.folder || Request.extensions.empty())
		return;
	std::wstring pattern;
	for (auto const &extension : Request.extensions)
		pattern += (pattern.empty() ? L"*." : L";*.") + wide(extension);
	auto const name{wide(Request.filter_name) + L" (" + pattern + L")"};
	std::array const filters{COMDLG_FILTERSPEC{name.c_str(), pattern.c_str()}, COMDLG_FILTERSPEC{L"*.*", L"*.*"}};
	Dialog.SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
}

std::vector<std::filesystem::path> chosen(IFileOpenDialog &Dialog)
{
	std::vector<std::filesystem::path> paths;
	released<IShellItemArray> items;
	if (FAILED(Dialog.GetResults(&items.pointer)) || items.pointer == nullptr)
		return paths;
	DWORD count{0};
	items.pointer->GetCount(&count);
	for (DWORD index = 0; index < count; ++index)
	{
		released<IShellItem> item;
		if (SUCCEEDED(items.pointer->GetItemAt(index, &item.pointer)) && item.pointer != nullptr)
			if (auto path{path_of(*item.pointer)}; false == path.empty())
				paths.push_back(std::move(path));
	}
	return paths;
}

result open_windows(request const &Request)
{
	result answer;
	// the dialog runs aside of the frame: this thread gets COM of its own
	auto const initialised{::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};
	{
		released<IFileOpenDialog> dialog;
		if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog.pointer))) || dialog.pointer == nullptr)
			answer.available = false;
		else
		{
			FILEOPENDIALOGOPTIONS options{0};
			dialog.pointer->GetOptions(&options);
			options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
			if (Request.folder)
				options |= FOS_PICKFOLDERS;
			else if (Request.many)
				options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
			dialog.pointer->SetOptions(options);
			dialog.pointer->SetTitle(wide(Request.title).c_str());
			set_filter(*dialog.pointer, Request);
			if (SUCCEEDED(dialog.pointer->Show(static_cast<HWND>(Request.owner))))
				answer.paths = chosen(*dialog.pointer);
		}
	}
	if (SUCCEEDED(initialised))
		::CoUninitialize();
	return answer;
}

#else

std::filesystem::path path_of_utf8(std::string_view const Text)
{
	return std::filesystem::path{std::u8string{Text.begin(), Text.end()}};
}

// a word for the shell, in single quotes
std::string shell_word(std::string_view const Text)
{
	std::string result{"'"};
	for (auto const character : Text)
		result += character == '\'' ? std::string{"'\\''"} : std::string(1, character);
	return result + "'";
}

bool have(char const *Program)
{
	return std::system((std::string{"command -v "} + Program + " >/dev/null 2>&1").c_str()) == 0;
}

std::string zenity_command(request const &Request)
{
	std::string command{"zenity --file-selection --title=" + shell_word(Request.title)};
	if (Request.folder)
		return command + " --directory";
	if (Request.many)
		command += " --multiple --separator='\n'";
	if (false == Request.extensions.empty())
	{
		std::string patterns;
		for (auto const &extension : Request.extensions)
			patterns += " *." + extension;
		command += " --file-filter=" + shell_word(Request.filter_name + " |" + patterns) + " --file-filter=" + shell_word("* | *");
	}
	return command;
}

std::string kdialog_command(request const &Request)
{
	if (Request.folder)
		return "kdialog --title " + shell_word(Request.title) + " --getexistingdirectory .";
	std::string patterns;
	for (auto const &extension : Request.extensions)
		patterns += (patterns.empty() ? "*." : " *.") + extension;
	return "kdialog --title " + shell_word(Request.title) + " --getopenfilename . " + shell_word(patterns + " | " + Request.filter_name) + (Request.many ? " --multiple --separate-output" : "");
}

// the dialog of GTK through python3 and its gi, which Cinnamon and GNOME have even where zenity isn't installed: the same window
// the file managers of these desktops open
char const *const kGtkDialog{R"(import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk
title, folder, many, name = sys.argv[1], sys.argv[2] == '1', sys.argv[3] == '1', sys.argv[4]
action = Gtk.FileChooserAction.SELECT_FOLDER if folder else Gtk.FileChooserAction.OPEN
dialog = Gtk.FileChooserDialog(title=title, action=action)
dialog.add_buttons('_Cancel', Gtk.ResponseType.CANCEL, '_Open', Gtk.ResponseType.OK)
dialog.set_select_multiple(many and not folder)
if len(sys.argv) > 5 and not folder:
    chosen = Gtk.FileFilter()
    chosen.set_name(name)
    for extension in sys.argv[5:]:
        chosen.add_pattern('*.' + extension.lower())
        chosen.add_pattern('*.' + extension.upper())
    dialog.add_filter(chosen)
    every = Gtk.FileFilter()
    every.set_name('*')
    every.add_pattern('*')
    dialog.add_filter(every)
if dialog.run() == Gtk.ResponseType.OK:
    for path in dialog.get_filenames():
        print(path)
)"};

bool have_gtk()
{
	return have("python3") && std::system("python3 -c \"import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk\" >/dev/null 2>&1") == 0;
}

std::string gtk_command(request const &Request)
{
	std::string command{"python3 -c " + shell_word(kGtkDialog) + ' ' + shell_word(Request.title) + (Request.folder ? " 1" : " 0") + (Request.many ? " 1 " : " 0 ") + shell_word(Request.filter_name)};
	for (auto const &extension : Request.extensions)
		command += ' ' + shell_word(extension);
	return command;
}

// the first of the dialogs the system has: zenity, kdialog, yad and qarma read the words of zenity or of their own, GTK through python3
std::string dialog_command(request const &Request)
{
	if (have("zenity"))
		return zenity_command(Request);
	if (have("kdialog"))
		return kdialog_command(Request);
	if (have("yad"))
		return "yad" + zenity_command(Request).substr(6);
	if (have("qarma"))
		return "qarma" + zenity_command(Request).substr(6);
	if (have_gtk())
		return gtk_command(Request);
	return {};
}

result open_other(request const &Request)
{
	result answer;
	auto const command{dialog_command(Request)};
	if (command.empty())
	{
		answer.available = false;
		return answer;
	}
	auto *const pipe{::popen((command + " 2>/dev/null").c_str(), "r")};
	if (pipe == nullptr)
	{
		answer.available = false;
		return answer;
	}
	std::string output;
	std::array<char, 4096> buffer{};
	while (auto const read{std::fread(buffer.data(), 1, buffer.size(), pipe)})
		output.append(buffer.data(), read);
	::pclose(pipe);
	// a path a line
	std::size_t at{0};
	while (at < output.size())
	{
		auto end{output.find('\n', at)};
		if (end == std::string::npos)
			end = output.size();
		if (end > at)
			answer.paths.push_back(path_of_utf8(std::string_view{output}.substr(at, end - at)));
		at = end + 1;
	}
	return answer;
}

#endif

} // namespace

result open(request const &Request)
{
#ifdef _WIN32
	return open_windows(Request);
#else
	return open_other(Request);
#endif
}

} // namespace editor_file_dialog
