/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

// the field of work of the events, among the experimental functions of the editor. for now the ambient sounds: each is a sound
// node, the sound events which play it and an event launcher which sets them off, all of the plain kinds any build reads,
// written on each save to an include of the editor's own, <scenery>_dzwieki.scm. a line of a comment before each keeps what the
// editor needs to read it back

#include "stdafx.h"
#include "application/editormode.h"
#include "application/editormodeshared.h"
#include "application/editorprojection.h"
#include "application/editoruilayer.h"
#include "audio/sound.h"
#include "rendering/renderer.h"
#include "editor/editorFormat.hpp"
#include "scene/scenelayers.h"
#include "simulation/simulation.h"
#include "utilities/Globals.h"
#include "utilities/Logs.h"
#include "utilities/translation.h"
#include "utilities/utilities.h"

#include "imgui/imgui.h"
#include "imgui/ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <numbers>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

namespace
{

char const *const kAmbientHeader{"// made by the scenery editor on each save: the ambient sounds of the scenery (Events, Ambient sounds)"};
// the line of each sound the editor reads back: name|x|y|z|file|range|play|every|from|to, then the other settings. the starters
// read //$a in a scenery file in a way of their own: it's read back from the files written by the editor before, and written no more
std::string_view constexpr kAmbientMark{"//$u "};
std::string_view constexpr kAmbientMarkBefore{"//$a "};
// px, how near the cursor a sound is picked
float constexpr kPickPixels{14.0f};
// of the line of the editor: the ten fields of the first sounds, and the others which came after them
std::size_t constexpr kAmbientFields{10};
std::size_t constexpr kAmbientFieldsAll{24};

std::string text_of(std::filesystem::path const &Path)
{
	auto const text{Path.u8string()};
	return {text.begin(), text.end()};
}

std::filesystem::path path_of(std::string_view const Text)
{
	return std::filesystem::path{std::u8string{Text.begin(), Text.end()}};
}

// the name of the file of the sounds: after the scenery, as the other files of the editor
std::string ambient_file()
{
	if (scene::Layers.empty())
		return {};
	std::string root{scene::Layers.layer(scene::layer_handle{1}).name};
	if (auto const slash{root.find_last_of("/\\")}; slash != std::string::npos)
		root.erase(0, slash + 1);
	if (auto const dot{root.rfind('.')}; dot != std::string::npos)
		root.erase(dot);
	return root.empty() ? std::string{} : root + "_dzwieki.scm";
}

std::string hhmm(int const Time)
{
	// 0000 is read by the launcher as no hour at all: a minute past midnight then
	auto const hours{std::clamp(Time / 100, 0, 23)};
	auto const minutes{std::clamp(Time % 100, 0, 59)};
	return format("%02d%02d", hours, hours == 0 && minutes == 0 ? 1 : minutes);
}

// a field of the line written as a word of the scenery: without the separators of the line, and * where there's nothing
std::string word_of(std::string_view const Text)
{
	std::string word;
	for (auto const character : Text)
		if (character != '|' && character != ' ' && character != '\t' && character != '\r' && character != '\n')
			word += character;
	return word.empty() ? std::string{"*"} : word;
}

// the files of the node: one, or a set it picks one of at random each time the scenery loads
std::string sound_set(editor_mode_ambient const &Sound)
{
	if (Sound.others.empty())
		return Sound.file;
	std::string text{"[ " + Sound.file};
	for (auto const &other : Sound.others)
		text += ' ' + other;
	return text + " ]";
}

// what goes between the place of the node and endsound: the files alone, as before, or a block with the other keys
std::string sound_definition(editor_mode_ambient const &Sound)
{
	auto const set{sound_set(Sound)};
	if (Sound.begin.empty() && Sound.end.empty() && Sound.pitch <= 0.0f && Sound.start <= 0.0f)
		return set;
	std::string text{"{ soundmain: " + set};
	if (false == Sound.begin.empty())
		text += " soundbegin: " + Sound.begin;
	if (false == Sound.end.empty())
		text += " soundend: " + Sound.end;
	if (Sound.pitch > 0.0f)
		text += format(" pitchvariation: %.2f", Sound.pitch);
	if (Sound.start > 0.0f)
		text += format(" startoffset: %.2f", Sound.start);
	return text + " }";
}

// a sound event of the node: 1 plays it once, -1 in a loop, 0 stops it
std::string event_text(editor_mode_ambient const &Sound, std::string_view const Suffix, int const Mode)
{
	std::string text{"event " + Sound.name + std::string{Suffix} + " sound " + format("%.1f", std::max(0.0f, Sound.delay)) + ' ' + Sound.name + ' ' + std::to_string(Mode)};
	// a channel makes the sound played once go out on the radio of the trains instead
	if (Mode == 1 && Sound.channel > 0)
		text += ' ' + std::to_string(Sound.channel);
	if (Sound.random > 0.0f)
		text += format(" randomdelay %.1f", Sound.random);
	return text + " endevent\r\n";
}

struct launch
{
	std::string_view suffix; // of the name of the launcher
	std::string radius; // m, or -1 for anywhere
	bool keyed{true}; // the key of the sound sets it off too
	std::string time; // the interval, or the hour
	std::string_view event; // the suffix of the event it sets off
};

std::string launcher_text(editor_mode_ambient const &Sound, std::string const &Where, launch const &Launch)
{
	auto const key{Launch.keyed && false == Sound.key.empty() ? Sound.key : std::string{"none"}};
	std::string text{"node -1 0 " + Sound.name + std::string{Launch.suffix} + " eventlauncher " + Where + ' ' + Launch.radius + ' ' + key + ' ' + Launch.time + ' ' + Sound.name + std::string{Launch.event}};
	// the memory cell it waits for, then the word which ends the condition
	if (false == Sound.cell.empty())
		text += " condition " + Sound.cell + ' ' + word_of(Sound.text) + ' ' + word_of(Sound.value1) + ' ' + word_of(Sound.value2);
	return text + " end\r\n";
}

// how far the launchers of the sound near the camera reach
std::string reach_of(editor_mode_ambient const &Sound)
{
	if (Sound.reach < 0.0f)
		return "-1";
	return format("%.1f", Sound.reach > 0.0f ? Sound.reach : Sound.range);
}

// the line of the editor of a sound
std::string ambient_line(editor_mode_ambient const &Sound)
{
	auto const &at{Sound.location};
	std::string others;
	for (auto const &other : Sound.others)
		others += (others.empty() ? "" : ";") + other;
	std::string line{std::string{kAmbientMark} + Sound.name + '|' + format("%.3f|%.3f|%.3f", at.x, at.y, at.z) + '|' + Sound.file + '|' + format("%.1f", Sound.range) + '|' + std::to_string(Sound.play) + '|' + format("%.1f", Sound.every) + '|' +
	                 std::to_string(Sound.from) + '|' + std::to_string(Sound.to)};
	line += '|' + others + '|' + Sound.begin + '|' + Sound.end + '|' + format("%.2f|%.2f|%.1f|%.1f", Sound.pitch, Sound.start, Sound.delay, Sound.random) + '|' + std::to_string(Sound.channel) + '|' + format("%.1f", Sound.reach) + '|' + Sound.key + '|' +
	        Sound.cell + '|' + word_of(Sound.text) + '|' + word_of(Sound.value1) + '|' + word_of(Sound.value2);
	return line + "\r\n";
}

// the nodes and events of a sound, as the simulator reads them
std::string ambient_text(editor_mode_ambient const &Sound)
{
	auto const &at{Sound.location};
	auto const where{format("%.3f %.3f %.3f", at.x, at.y, at.z)};
	std::string text{ambient_line(Sound)};
	text += "node " + format("%.1f", Sound.range) + " 0 " + Sound.name + " sound " + where + ' ' + sound_definition(Sound) + " endsound\r\n";
	switch (Sound.play)
	{
	case 1:
		// once every so many seconds while the camera is near
		text += event_text(Sound, "_play", 1);
		text += launcher_text(Sound, where, {"_launcher", reach_of(Sound), true, format("%.0f", -std::max(1.0f, Sound.every)), "_play"});
		break;
	case 2:
		// in a loop from one hour to the other, anywhere in the scenery
		text += event_text(Sound, "_on", -1);
		text += event_text(Sound, "_off", 0);
		text += launcher_text(Sound, where, {"_from", "-1", false, hhmm(Sound.from), "_on"});
		text += launcher_text(Sound, where, {"_to", "-1", false, hhmm(Sound.to), "_off"});
		break;
	case 3:
		// once at an hour, anywhere in the scenery
		text += event_text(Sound, "_play", 1);
		text += launcher_text(Sound, where, {"_launcher", "-1", true, hhmm(Sound.from), "_play"});
		break;
	case 4:
		// once on a key, while the camera is near: an interval of 0 and no hour leave the key alone
		text += event_text(Sound, "_play", 1);
		text += launcher_text(Sound, where, {"_launcher", reach_of(Sound), true, "0", "_play"});
		break;
	default:
		// in a loop from when the camera first comes near: an interval of -10000 sets it off once
		text += event_text(Sound, "_on", -1);
		text += launcher_text(Sound, where, {"_launcher", reach_of(Sound), true, "-10000", "_on"});
		break;
	}
	return text;
}

// the fields of the line, the empty ones and the one at the end included
std::vector<std::string> fields_of(std::string_view Line)
{
	std::vector<std::string> fields;
	while (true)
	{
		auto const bar{Line.find('|')};
		fields.emplace_back(Line.substr(0, bar));
		if (bar == std::string_view::npos)
			return fields;
		Line.remove_prefix(bar + 1);
	}
}

void read_ambient_more(std::vector<std::string> const &Fields, editor_mode_ambient &Sound)
{
	// written by the editor before these were: they keep what they were made with
	if (Fields.size() < kAmbientFieldsAll)
		return;
	std::istringstream others{Fields[10]};
	std::string other;
	while (std::getline(others, other, ';'))
		if (false == other.empty())
			Sound.others.push_back(other);
	Sound.begin = Fields[11];
	Sound.end = Fields[12];
	Sound.pitch = std::stof(Fields[13]);
	Sound.start = std::stof(Fields[14]);
	Sound.delay = std::stof(Fields[15]);
	Sound.random = std::stof(Fields[16]);
	Sound.channel = std::stoi(Fields[17]);
	Sound.reach = std::stof(Fields[18]);
	Sound.key = Fields[19];
	Sound.cell = Fields[20];
	Sound.text = Fields[21];
	Sound.value1 = Fields[22];
	Sound.value2 = Fields[23];
}

// a sound of the line of the editor, false if it isn't one
bool read_ambient(std::string_view Line, editor_mode_ambient &Sound)
{
	if (Line.starts_with(kAmbientMark))
		Line.remove_prefix(kAmbientMark.size());
	else if (Line.starts_with(kAmbientMarkBefore))
		Line.remove_prefix(kAmbientMarkBefore.size());
	else
		return false;
	auto const fields{fields_of(Line)};
	if (fields.size() < kAmbientFields)
		return false;
	try
	{
		Sound.name = fields[0];
		Sound.location = {std::stod(fields[1]), std::stod(fields[2]), std::stod(fields[3])};
		Sound.file = fields[4];
		Sound.range = std::stof(fields[5]);
		Sound.play = std::clamp(std::stoi(fields[6]), 0, 4);
		Sound.every = std::stof(fields[7]);
		Sound.from = std::stoi(fields[8]);
		Sound.to = std::stoi(fields[9]);
		read_ambient_more(fields, Sound);
	}
	catch (std::exception const &)
	{
		return false;
	}
	return false == Sound.name.empty() && false == Sound.file.empty();
}

// a name of a node and its events: letters, digits, _ and -, as the scenery reads them
std::string name_of_typed(std::string_view const Typed)
{
	std::string name;
	for (auto const character : Typed)
	{
		auto const letter{static_cast<unsigned char>(character)};
		if (std::isalnum(letter) || character == '_' || character == '-')
			name += static_cast<char>(std::tolower(letter));
	}
	return name;
}

// the file as the sound node gives it, from the sounds folder of the game; a file from elsewhere is copied to sounds/<scenery>/
std::string sound_file(std::string const &Chosen, std::string &Error)
{
	auto path{path_of(Chosen)};
	std::error_code error;
	auto const sounds{std::filesystem::absolute(paths::sounds, error)};
	if (path.is_relative())
	{
		// typed as the scenery would have it
		if (std::filesystem::exists(sounds / path, error))
			return text_of(path.generic_u8string());
		Error = STR("There's no such file in the sounds folder of the game");
		return {};
	}
	if (auto const relative{std::filesystem::relative(path, sounds, error)}; false == static_cast<bool>(error) && false == relative.empty() && false == text_of(relative).starts_with(".."))
		return text_of(relative.generic_u8string());
	auto stem{ambient_file()};
	if (auto const suffix{stem.rfind("_dzwieki.scm")}; suffix != std::string::npos)
		stem.erase(suffix);
	auto const folder{sounds / path_of(stem.empty() ? std::string{"scenery"} : stem)};
	std::filesystem::create_directories(folder, error);
	auto const target{folder / path.filename()};
	if (false == std::filesystem::exists(target, error))
		std::filesystem::copy_file(path, target, error);
	if (error)
	{
		Error = STR("The file couldn't be copied to the sounds folder of the game");
		return {};
	}
	return text_of(std::filesystem::relative(target, sounds, error).generic_u8string());
}

} // namespace

void editor_mode::events_workspace(bool const Open)
{
	m_events_open = Open;
	if (Open)
	{
		events_load();
		// the sounds are selected and moved as the models are: the edit mode of the tracks would take the Inspector and the gizmo
		if (ui()->nodebank().mode == nodebank_panel::TRACK)
			ui()->nodebank().mode = nodebank_panel::MODIFY;
	}
	else
	{
		m_events.placing = false;
		events_listen(-1);
	}
}

void editor_mode::events_load()
{
	auto &state{m_events};
	if (state.loaded)
		return;
	state.loaded = true;
	auto const file{ambient_file()};
	if (file.empty())
		return;
	std::ifstream input{Global.asCurrentSceneryPath + file, std::ios_base::binary};
	std::string line;
	while (std::getline(input, line))
	{
		if (false == line.empty() && line.back() == '\r')
			line.pop_back();
		if (ambient_sound sound; read_ambient(line, sound))
			state.sounds.push_back(std::move(sound));
	}
	// read, not changed: no step of the history
	state.last = state.sounds;
}

bool editor_mode::ambient_sounds(std::vector<std::string> &Rootstatements)
{
	auto const file{ambient_file()};
	if (file.empty())
		return false;
	// a scenery the sounds were never opened for: whatever its file holds stays as it is
	if (false == m_events.loaded)
		return false;
	auto const path{Global.asCurrentSceneryPath + file};
	std::error_code error;
	auto const exists{std::filesystem::exists(path, error)};
	if (m_events.sounds.empty() && false == exists)
		return false;
	std::string content{std::string{kAmbientHeader} + "\r\n"};
	for (auto const &sound : m_events.sounds)
		content += ambient_text(sound);
	std::string current;
	{
		std::ifstream input{path, std::ios_base::binary};
		current.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}
	if (current != content)
	{
		std::ofstream output{path, std::ios_base::binary | std::ios_base::trunc};
		output << content;
		WriteLog("Editor: " + std::to_string(m_events.sounds.size()) + " ambient sounds written to " + file);
	}
	// the include once, at the end of the scenery
	if (m_events.included)
		return false;
	for (std::size_t handle = 1; handle <= scene::Layers.size(); ++handle)
	{
		auto name{scene::Layers.layer(static_cast<scene::layer_handle>(handle)).name};
		std::ranges::replace(name, '\\', '/');
		if (ToLower(name) == ToLower(file) || ToLower(name).ends_with("/" + ToLower(file)))
			return false;
	}
	Rootstatements.push_back("include " + file + " end");
	return true;
}


void editor_mode::events_listen(int const Index)
{
	auto &state{m_events};
	if (state.preview != nullptr)
		state.preview->stop();
	state.preview.reset();
	state.previewing = -1;
	if (Index < 0 || Index >= static_cast<int>(state.sounds.size()))
		return;
	auto const &sound{state.sounds[Index]};
	// heard where it's put, in a loop, until stopped: as near the camera as it's to be heard
	auto preview{std::make_shared<sound_source>(sound_placement::external, sound.range)};
	preview->deserialize(sound_definition(sound), sound_type::single);
	preview->offset(glm::vec3{sound.location});
	preview->play(sound_flags::exclusive | sound_flags::looping);
	state.preview = std::move(preview);
	state.previewing = Index;
}

void editor_mode::events_place(glm::dvec3 const &Where)
{
	auto &state{m_events};
	glm::dvec3 const at{Where.x, Where.y + state.height, Where.z};
	std::string error;
	auto const file{sound_file(state.file.data(), error)};
	if (file.empty())
	{
		state.status = error;
		return;
	}
	// a name no node of the scenery and no sound of the editor has
	auto const taken = [&state](std::string const &Name) {
		return simulation::Sounds.find(Name) != nullptr || std::ranges::any_of(state.sounds, [&Name](ambient_sound const &Sound) { return Sound.name == Name; });
	};
	int number{1};
	auto name{format("ambient_%d", number)};
	while (taken(name))
		name = format("ambient_%d", ++number);
	ambient_sound sound;
	sound.name = name;
	sound.location = at;
	sound.file = file;
	state.sounds.push_back(std::move(sound));
	state.selected = static_cast<int>(state.sounds.size()) - 1;
	state.status = format(STR_C("%s put; it goes into the scenery with the next save, and plays in it after the scenery is loaded again"), name.c_str());
}

void editor_mode::events_delete()
{
	auto &state{m_events};
	if (state.selected < 0 || state.selected >= static_cast<int>(state.sounds.size()))
		return;
	events_listen(-1);
	state.sounds.erase(state.sounds.begin() + state.selected);
	state.selected = -1;
	state.status = STR("The sound deleted; it goes out of the scenery with the next save");
}

bool editor_mode::events_rename(ambient_sound &Sound, std::string const &Name)
{
	auto &state{m_events};
	auto const name{name_of_typed(Name)};
	if (name.empty() || name == Sound.name)
		return false;
	// a sound node of the scenery of that name, which isn't one of the editor read back with the scenery
	auto const ours{std::ranges::any_of(state.sounds, [&name](ambient_sound const &Other) { return Other.name == name; })};
	if (ours || simulation::Sounds.find(name) != nullptr)
	{
		state.status = format(STR_C("The name %s is taken"), name.c_str());
		return false;
	}
	Sound.name = name;
	state.status = STR("The sound renamed, its events and launchers with it");
	return true;
}

void editor_mode::events_browse(int const For)
{
	auto &state{m_events};
	if (state.dialog.valid())
		return;
	state.dialog_for = For;
	editor_file_dialog::request request;
	request.title = STR("A sound file");
	request.many = false;
	request.filter_name = STR("Sounds");
	request.extensions = {"wav", "ogg", "flac"};
	request.owner = ImGui::GetMainViewport()->PlatformHandleRaw;
	state.dialog = std::async(std::launch::async, [request]() { return editor_file_dialog::open(request); });
}

void editor_mode::events_chosen()
{
	auto &state{m_events};
	if (false == state.dialog.valid() || state.dialog.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
		return;
	auto const chosen{state.dialog.get()};
	if (chosen.paths.empty())
	{
		if (false == chosen.available)
			state.status = STR("The system has no file dialog the editor can open: type the path of the file");
		return;
	}
	auto const shown{text_of(chosen.paths.front())};
	if (state.dialog_for == 0)
	{
		state.file.fill('\0');
		std::copy_n(shown.begin(), std::min(shown.size(), state.file.size() - 1), state.file.begin());
		return;
	}
	if (state.selected < 0 || state.selected >= static_cast<int>(state.sounds.size()))
		return;
	std::string error;
	auto const file{sound_file(shown, error)};
	if (file.empty())
	{
		state.status = error;
		return;
	}
	auto &sound{state.sounds[state.selected]};
	switch (state.dialog_for)
	{
	case 1:
		sound.file = file;
		break;
	case 2:
		sound.others.push_back(file);
		break;
	case 3:
		sound.begin = file;
		break;
	default:
		sound.end = file;
		break;
	}
	if (state.previewing == state.selected)
		events_listen(state.selected);
}

bool editor_mode::events_click()
{
	auto &state{m_events};
	if (ImGui::GetIO().WantCaptureMouse)
		return false;
	if (state.placing)
	{
		glm::dvec3 ground;
		if (false == cursor_on_ground(ground))
			return true;
		events_place(ground);
		// Shift+LMB puts one more
		if (false == Global.shiftState)
			state.placing = false;
		return true;
	}
	// a press on the gizmo of the selected one moves it, and keeps it selected
	if (state.selected >= 0 && gizmo_shown() && (ImGuizmo::IsOver() || ImGuizmo::IsUsing()))
		return true;
	// the sound nearest to the cursor on the screen
	screen_projection const projection;
	auto const &mouse{ImGui::GetIO().MousePos};
	auto best{kPickPixels};
	state.selected = -1;
	for (std::size_t index = 0; index < state.sounds.size(); ++index)
	{
		ImVec2 at;
		if (false == projection.project(state.sounds[index].location, at))
			continue;
		if (auto const distance{std::hypot(at.x - mouse.x, at.y - mouse.y)}; distance < best)
		{
			best = distance;
			state.selected = static_cast<int>(index);
		}
	}
	return true;
}

void editor_mode::events_track_changes()
{
	auto &state{m_events};
	if (false == state.loaded)
		return;
	if (state.sounds != state.last && false == state.base.has_value())
		state.base = state.last;
	state.last = state.sounds;
	// done when nothing is held: a field typed in, a value dragged, the gizmo
	if (false == state.base.has_value() || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing())
		return;
	auto before{std::move(*state.base)};
	state.base.reset();
	if (before == state.sounds)
		return;
	if (m_max_history_size >= 0 && static_cast<int>(m_history.size()) >= m_max_history_size)
		m_history.erase(m_history.begin(), m_history.begin() + (static_cast<int>(m_history.size()) - m_max_history_size + 1));
	EditorSnapshot step;
	step.action = EditorSnapshot::Action::Sounds;
	step.node_name = STR("Ambient sounds");
	step.sounds = std::move(before);
	m_history.push_back(std::move(step));
	g_redo.clear();
}

void editor_mode::restore_sounds(EditorSnapshot &Snapshot)
{
	auto &state{m_events};
	if (false == Snapshot.sounds.has_value())
		return;
	events_listen(-1);
	// what they are now goes to the step, for the way back
	std::swap(*Snapshot.sounds, state.sounds);
	state.loaded = true;
	state.last = state.sounds;
	state.base.reset();
	state.placing = false;
	if (state.selected >= static_cast<int>(state.sounds.size()))
		state.selected = -1;
}

void editor_mode::render_events_gizmo()
{
	auto &state{m_events};
	m_gizmo_using = false;
	if (state.selected < 0 || state.selected >= static_cast<int>(state.sounds.size()))
		return;
	auto &sound{state.sounds[state.selected]};
	ImGuizmo::BeginFrame();
	ImGuizmo::SetOrthographic(Global.EditorOrtho);
	ImGuiIO const &io{ImGui::GetIO()};
	ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
	glm::mat4 const view{GfxRenderer->Camera_View_Matrix()};
	glm::dvec3 const camerapos{GfxRenderer->Camera_Position()};
	float const aspect{io.DisplaySize.y > 0.0f ? io.DisplaySize.x / io.DisplaySize.y : 1.0f};
	glm::mat4 const projection{editor_mode::projection_matrix(aspect)};
	// a sound has no turn and no scale: it's moved only, along the axes of the world
	glm::vec3 const relative{sound.location - camerapos};
	std::array const translation{relative.x, relative.y, relative.z};
	std::array const angles{0.0f, 0.0f, 0.0f};
	std::array const scale{1.0f, 1.0f, 1.0f};
	glm::mat4 matrix{1.0f};
	ImGuizmo::RecomposeMatrixFromComponents(translation.data(), angles.data(), scale.data(), glm::value_ptr(matrix));
	glm::vec3 const snapvalue{m_gizmo_snap};
	float const *snap{Global.ctrlState && m_gizmo_snap > 0.0f ? glm::value_ptr(snapvalue) : nullptr};
	ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(matrix), nullptr, snap);
	if (false == ImGuizmo::IsUsing())
		return;
	m_gizmo_using = true;
	std::array<float, 3> moved{};
	std::array<float, 3> turned{};
	std::array<float, 3> scaled{};
	ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(matrix), moved.data(), turned.data(), scaled.data());
	sound.location = camerapos + glm::dvec3{moved[0], moved[1], moved[2]};
	if (state.previewing == state.selected && state.preview != nullptr)
		state.preview->offset(glm::vec3{sound.location});
}

void editor_mode::draw_events_overlay()
{
	if (false == m_events_open)
		return;
	auto const &state{m_events};
	screen_projection const projection;
	auto *drawlist{ImGui::GetBackgroundDrawList(ImGui::GetMainViewport())};
	// the sound nodes of the scenery, in grey: those of the editor are drawn over them
	for (auto const *sound : simulation::Sounds.sequence())
	{
		ImVec2 at;
		if (sound == nullptr || false == projection.project(glm::dvec3{sound->location()}, at))
			continue;
		drawlist->AddCircle(at, 5.0f, IM_COL32(170, 170, 170, 160), 12, 1.5f);
	}
	for (std::size_t index = 0; index < state.sounds.size(); ++index)
	{
		auto const &sound{state.sounds[index]};
		auto const chosen{static_cast<int>(index) == state.selected};
		auto const colour{chosen ? IM_COL32(255, 190, 60, 240) : IM_COL32(90, 200, 255, 220)};
		ImVec2 at;
		if (false == projection.project(sound.location, at))
			continue;
		drawlist->AddCircleFilled(at, chosen ? 7.0f : 5.0f, colour);
		drawlist->AddText(ImVec2(at.x + 9.0f, at.y - 8.0f), colour, sound.name.c_str());
		if (false == chosen)
			continue;
		// how far it's heard, on the ground around it
		std::vector<ImVec2> ring;
		for (int i = 0; i < 64; ++i)
		{
			auto const angle{2.0 * std::numbers::pi * i / 64.0};
			if (ImVec2 point; projection.project(sound.location + glm::dvec3{std::cos(angle) * sound.range, 0.0, std::sin(angle) * sound.range}, point))
				ring.push_back(point);
		}
		if (ring.size() > 2)
			drawlist->AddPolyline(ring.data(), static_cast<int>(ring.size()), IM_COL32(255, 190, 60, 160), ImDrawFlags_Closed, 1.5f);
	}
}


void editor_mode::render_events_tool_options()
{
	auto &state{m_events};
	events_load();
	events_chosen();
	ImGui::SeparatorText(STR_C("Ambient sounds"));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", STR_C("A sound heard around a place of the scenery: birds, a stream, a works. Each is a sound node with its events and an event launcher, written on each save to an include of the editor"));
	ImGui::PopTextWrapPos();
	ImGui::SetNextItemWidth(-90.0f);
	ImGui::InputTextWithHint("##ambientfile", STR_C("A sound file: .wav, .ogg, .flac"), state.file.data(), state.file.size());
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", STR_C("From the sounds folder of the game, as sounds/birds.wav is birds.wav; a file from elsewhere is copied to sounds/<scenery>/"));
	ImGui::SameLine();
	ImGui::BeginDisabled(state.dialog.valid());
	if (ImGui::Button(STR_C("Choose...##ambient"), ImVec2(-FLT_MIN, 0.0f)))
		events_browse(0);
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat(STR_C("Over the ground (m)"), &state.height, 0.1f, 0.0f, 100.0f, "%.1f");
	ImGui::BeginDisabled(state.file[0] == '\0');
	if (main_button(state.placing ? STR_C("Click where it's to be heard...") : STR_C("Put a sound"), ImVec2(-FLT_MIN, tall_button_height())))
		state.placing = false == state.placing;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", STR_C("Then LMB on the ground puts it there; Shift+LMB puts one more"));
	ImGui::TextDisabled("%s", STR_C("LMB: select a sound   Gizmo: move it   Del: delete   Esc: deselect"));
	if (false == state.status.empty())
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", state.status.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::Text(STR_C("Sounds of the editor: %zu, sound nodes of the scenery: %zu"), state.sounds.size(), simulation::Sounds.sequence().size());
	for (std::size_t index = 0; index < state.sounds.size(); ++index)
		if (ImGui::Selectable(format("%s  (%s)##ambient%zu", state.sounds[index].name.c_str(), state.sounds[index].file.c_str(), index).c_str(), static_cast<int>(index) == state.selected))
			state.selected = static_cast<int>(index);
}

void editor_mode::render_events_selection()
{
	auto &state{m_events};
	events_chosen();
	if (state.selected < 0 || state.selected >= static_cast<int>(state.sounds.size()))
	{
		ImGui::TextDisabled("%s", STR_C("Click a sound in the view, or in the list, to change it"));
		return;
	}
	render_ambient_sound(state.sounds[state.selected]);
}

void editor_mode::render_ambient_sound(ambient_sound &Sound)
{
	auto &state{m_events};
	ImGui::SeparatorText(Sound.name.c_str());
	if (state.renaming != state.selected)
	{
		state.renaming = state.selected;
		state.rename.fill('\0');
		std::copy_n(Sound.name.begin(), std::min(Sound.name.size(), state.rename.size() - 1), state.rename.begin());
	}
	if (begin_fields("##ambientfields"))
	{
		field_row("Name##ambient");
		if (ImGui::InputText("##ambientname", state.rename.data(), state.rename.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
		{
			events_rename(Sound, state.rename.data());
			state.renaming = -1;
		}
		item_tooltip("The name of the sound node; its events and launchers are named after it. Letters, digits, _ and -");
		field_row("Heard as far as (m)");
		ImGui::DragFloat("##ambientrange", &Sound.range, 1.0f, 5.0f, 3000.0f, "%.0f");
		item_tooltip("The sound fades out over this distance, and it's set off when the camera comes this near");
		field_row("Place##ambient");
		ImGui::DragScalarN("##ambientplace", ImGuiDataType_Double, &Sound.location.x, 3, 0.1f, nullptr, nullptr, "%.1f");
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader(STR_C("Files##ambient"), ImGuiTreeNodeFlags_DefaultOpen))
		render_ambient_files(Sound);
	if (ImGui::CollapsingHeader(STR_C("Playing##ambient"), ImGuiTreeNodeFlags_DefaultOpen))
		render_ambient_play(Sound);
	if (ImGui::CollapsingHeader(STR_C("Launcher##ambient")))
		render_ambient_launcher(Sound);
	ImGui::Separator();
	auto const listening{state.previewing == state.selected};
	if (ImGui::Button(listening ? STR_C("Stop") : STR_C("Listen")))
		events_listen(listening ? -1 : state.selected);
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Fly to##ambient")))
		Camera.Pos = Sound.location + glm::dvec3{0.0, 15.0, 25.0};
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Delete##ambient")))
		events_delete();
}

namespace
{

// a file of the sound, with the buttons which change and clear it; true if it's to be cleared
bool file_row(char const *Label, char const *Id, std::string const &File, bool const Clearable)
{
	field_row(Label);
	ImGui::PushID(Id);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(File.empty() ? STR_C("(none)") : File.c_str());
	auto cleared{false};
	if (Clearable && false == File.empty())
	{
		ImGui::SameLine();
		cleared = ImGui::SmallButton("x");
	}
	ImGui::PopID();
	return cleared;
}

} // namespace

void editor_mode::render_ambient_files(ambient_sound &Sound)
{
	auto &state{m_events};
	ImGui::BeginDisabled(state.dialog.valid());
	if (begin_fields("##ambientfiles"))
	{
		file_row("Sound##ambientmain", "main", Sound.file, false);
		ImGui::SameLine();
		if (ImGui::SmallButton(STR_C("Change...##ambientmain")))
			events_browse(1);
		for (std::size_t index = 0; index < Sound.others.size(); ++index)
		{
			if (file_row("Or##ambientother", format("other%zu", index).c_str(), Sound.others[index], true))
			{
				Sound.others.erase(Sound.others.begin() + static_cast<std::ptrdiff_t>(index));
				break;
			}
		}
		field_row("##ambientmore");
		if (ImGui::SmallButton(STR_C("One more file...##ambient")))
			events_browse(2);
		item_tooltip("With more files the scenery picks one of them at random each time it's loaded");
		if (file_row("Before the loop##ambient", "begin", Sound.begin, true))
			Sound.begin.clear();
		ImGui::SameLine();
		if (ImGui::SmallButton(STR_C("Choose...##ambientbegin")))
			events_browse(3);
		if (file_row("After the loop##ambient", "end", Sound.end, true))
			Sound.end.clear();
		ImGui::SameLine();
		if (ImGui::SmallButton(STR_C("Choose...##ambientend")))
			events_browse(4);
		field_row("Pitch varies by##ambient");
		ImGui::SliderFloat("##ambientpitch", &Sound.pitch, 0.0f, 1.0f, "%.2f");
		item_tooltip("How far the pitch may differ, picked at random as the scenery loads; 0 leaves the slight difference the game gives every sound");
		field_row("Starts at##ambient");
		ImGui::SliderFloat("##ambientstart", &Sound.start, 0.0f, 1.0f, "%.2f");
		item_tooltip("Where in the file it starts to play, as a part of its length: 0 the beginning");
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
}

void editor_mode::render_ambient_play(ambient_sound &Sound)
{
	std::array const plays{STR_C("In a loop, from when the camera comes near"), STR_C("Once every so many seconds, while the camera is near"), STR_C("In a loop between two hours"), STR_C("Once at an hour"),
	                       STR_C("Once on a key, while the camera is near")};
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::Combo("##ambientplay", &Sound.play, plays.data(), static_cast<int>(plays.size()));
	if (false == begin_fields("##ambientplaying"))
		return;
	if (Sound.play == 1)
	{
		field_row("Every (s)");
		ImGui::DragFloat("##ambientevery", &Sound.every, 1.0f, 1.0f, 3600.0f, "%.0f");
	}
	if (Sound.play == 2 || Sound.play == 3)
	{
		field_row(Sound.play == 2 ? "From (HHMM)" : "At (HHMM)##ambient");
		ImGui::InputInt("##ambientfrom", &Sound.from, 0);
		// 0000 is read by the launcher as no hour at all
		Sound.from = std::clamp(Sound.from, 1, 2359);
	}
	if (Sound.play == 2)
	{
		field_row("To (HHMM)");
		ImGui::InputInt("##ambientto", &Sound.to, 0);
		Sound.to = std::clamp(Sound.to, 1, 2359);
	}
	field_row("Delay (s)##ambient");
	ImGui::DragFloat("##ambientdelay", &Sound.delay, 0.5f, 0.0f, 3600.0f, "%.1f");
	item_tooltip("How long after the launcher the sound starts (or stops)");
	field_row("Random delay up to (s)##ambient");
	ImGui::DragFloat("##ambientrandom", &Sound.random, 0.5f, 0.0f, 3600.0f, "%.1f");
	item_tooltip("A delay picked at random each time, added to the one above: a sound played every so often doesn't come like a clock");
	if (Sound.play == 1 || Sound.play == 3 || Sound.play == 4)
	{
		field_row("Radio channel##ambient");
		ImGui::InputInt("##ambientchannel", &Sound.channel, 1);
		Sound.channel = std::clamp(Sound.channel, 0, 10);
		item_tooltip("0: heard at its place. A channel sends it over the radio of the trains tuned in to it instead");
	}
	ImGui::EndTable();
	if (Sound.play == 2)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", STR_C("It starts at the first hour and stops at the second; a scenario which starts between them stays silent until the first hour comes"));
		ImGui::PopTextWrapPos();
	}
}

void editor_mode::render_ambient_launcher(ambient_sound &Sound)
{
	if (false == begin_fields("##ambientlauncher"))
		return;
	// the launchers of the hours are set off anywhere in the scenery
	if (Sound.play == 0 || Sound.play == 1 || Sound.play == 4)
	{
		field_row("Set off within##ambient");
		int within{0};
		if (Sound.reach > 0.0f)
			within = 1;
		else if (Sound.reach < 0.0f)
			within = 2;
		std::array const reaches{STR_C("As far as it's heard"), STR_C("A radius of its own"), STR_C("Anywhere around the camera")};
		if (ImGui::Combo("##ambientwithin", &within, reaches.data(), static_cast<int>(reaches.size())))
		{
			std::array const values{0.0f, std::max(5.0f, Sound.range), -1.0f};
			Sound.reach = values[static_cast<std::size_t>(within)];
		}
		if (within == 1)
		{
			field_row("Radius (m)##ambient");
			ImGui::DragFloat("##ambientreach", &Sound.reach, 1.0f, 1.0f, 5000.0f, "%.0f");
		}
	}
	if (Sound.play != 2)
	{
		field_row("Key##ambient");
		std::array<char, 2> key{};
		if (false == Sound.key.empty())
			key[0] = Sound.key.front();
		if (ImGui::InputText("##ambientkey", key.data(), key.size(), ImGuiInputTextFlags_CharsNoBlank))
			Sound.key = std::isalnum(static_cast<unsigned char>(key[0])) ? std::string(1, key[0]) : std::string{};
		item_tooltip("A letter or a digit which sets it off too, as long as the camera is near; a capital letter is the key with Shift");
	}
	field_row("Memory cell##ambient");
	std::array<char, 64> cell{};
	std::copy_n(Sound.cell.begin(), std::min(Sound.cell.size(), cell.size() - 1), cell.begin());
	if (ImGui::InputText("##ambientcell", cell.data(), cell.size(), ImGuiInputTextFlags_CharsNoBlank))
		Sound.cell = name_of_typed(cell.data());
	item_tooltip("It's set off only while this memory cell holds what's below; empty: no condition");
	if (false == Sound.cell.empty())
	{
		auto const value_field = [](char const *Label, char const *Id, std::string &Value) {
			field_row(Label);
			std::array<char, 64> text{};
			std::copy_n(Value.begin(), std::min(Value.size(), text.size() - 1), text.begin());
			if (ImGui::InputText(Id, text.data(), text.size(), ImGuiInputTextFlags_CharsNoBlank))
				Value = word_of(text.data());
		};
		value_field("Text (* any)##ambient", "##ambienttext", Sound.text);
		value_field("Value 1 (* any)##ambient", "##ambientvalue1", Sound.value1);
		value_field("Value 2 (* any)##ambient", "##ambientvalue2", Sound.value2);
	}
	ImGui::EndTable();
	if (Sound.play == 4 && Sound.key.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", STR_C("Give it a key: without one it's never set off"));
}
