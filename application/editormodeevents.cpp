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
#include "audio/sound.h"
#include "editor/editorFormat.hpp"
#include "scene/scenelayers.h"
#include "simulation/simulation.h"
#include "utilities/Globals.h"
#include "utilities/Logs.h"
#include "utilities/translation.h"
#include "utilities/utilities.h"

#include "imgui/imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace
{

char const *const kAmbientHeader{"// made by the scenery editor on each save: the ambient sounds of the scenery (Events, Ambient sounds)"};
// the line of each sound the editor reads back: name|x|y|z|file|range|play|every|from|to
std::string_view constexpr kAmbientMark{"//$a "};
// px, how near the cursor a sound is picked
float constexpr kPickPixels{14.0f};

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

// the nodes and events of a sound, as the simulator reads them
std::string ambient_text(editor_mode_ambient const &Sound)
{
	std::ostringstream text;
	text.imbue(std::locale::classic());
	auto const &at{Sound.location};
	auto const where{format("%.3f %.3f %.3f", at.x, at.y, at.z)};
	text << kAmbientMark << Sound.name << '|' << format("%.3f|%.3f|%.3f", at.x, at.y, at.z) << '|' << Sound.file << '|' << format("%.1f", Sound.range) << '|' << Sound.play << '|'
	     << format("%.1f", Sound.every) << '|' << Sound.from << '|' << Sound.to << "\r\n";
	text << "node " << format("%.1f", Sound.range) << " 0 " << Sound.name << " sound " << where << ' ' << Sound.file << " endsound\r\n";
	switch (Sound.play)
	{
	case 1:
		// once every so many seconds while the camera is near
		text << "event " << Sound.name << "_play sound 0 " << Sound.name << " 1 endevent\r\n";
		text << "node -1 0 " << Sound.name << "_launcher eventlauncher " << where << ' ' << format("%.1f", Sound.range) << " none " << format("%.0f", -std::max(1.0f, Sound.every)) << ' ' << Sound.name << "_play end\r\n";
		break;
	case 2:
		// in a loop from one hour to the other, anywhere in the scenery
		text << "event " << Sound.name << "_on sound 0 " << Sound.name << " -1 endevent\r\n";
		text << "event " << Sound.name << "_off sound 0 " << Sound.name << " 0 endevent\r\n";
		text << "node -1 0 " << Sound.name << "_from eventlauncher " << where << " -1 none " << hhmm(Sound.from) << ' ' << Sound.name << "_on end\r\n";
		text << "node -1 0 " << Sound.name << "_to eventlauncher " << where << " -1 none " << hhmm(Sound.to) << ' ' << Sound.name << "_off end\r\n";
		break;
	default:
		// in a loop from when the camera first comes near: an interval of -10000 sets it off once
		text << "event " << Sound.name << "_on sound 0 " << Sound.name << " -1 endevent\r\n";
		text << "node -1 0 " << Sound.name << "_launcher eventlauncher " << where << ' ' << format("%.1f", Sound.range) << " none -10000 " << Sound.name << "_on end\r\n";
		break;
	}
	return text.str();
}

// a sound of the line of the editor, false if it isn't one
bool read_ambient(std::string_view Line, editor_mode_ambient &Sound)
{
	if (false == Line.starts_with(kAmbientMark))
		return false;
	Line.remove_prefix(kAmbientMark.size());
	std::vector<std::string> fields;
	std::string field;
	std::istringstream stream{std::string{Line}};
	while (std::getline(stream, field, '|'))
		fields.push_back(field);
	if (fields.size() < 10)
		return false;
	try
	{
		Sound.name = fields[0];
		Sound.location = {std::stod(fields[1]), std::stod(fields[2]), std::stod(fields[3])};
		Sound.file = fields[4];
		Sound.range = std::stof(fields[5]);
		Sound.play = std::stoi(fields[6]);
		Sound.every = std::stof(fields[7]);
		Sound.from = std::stoi(fields[8]);
		Sound.to = std::stoi(fields[9]);
	}
	catch (std::exception const &)
	{
		return false;
	}
	return false == Sound.name.empty() && false == Sound.file.empty();
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
		events_load();
	else
	{
		m_events.placing = false;
		m_events.moving = false;
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
	preview->deserialize(sound.file, sound_type::single);
	preview->offset(glm::vec3{sound.location});
	preview->play(sound_flags::exclusive | sound_flags::looping);
	state.preview = std::move(preview);
	state.previewing = Index;
}

void editor_mode::events_place(glm::dvec3 const &Where)
{
	auto &state{m_events};
	glm::dvec3 const at{Where.x, Where.y + state.height, Where.z};
	if (state.moving && state.selected >= 0 && state.selected < static_cast<int>(state.sounds.size()))
	{
		state.sounds[state.selected].location = at;
		state.moving = false;
		if (state.previewing == state.selected)
			events_listen(state.selected);
		state.status = STR("The sound moved; it goes into the scenery with the next save");
		return;
	}
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

bool editor_mode::events_click()
{
	auto &state{m_events};
	if (ImGui::GetIO().WantCaptureMouse)
		return false;
	if (state.placing || state.moving)
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
	// what the file dialog chose
	if (state.dialog.valid() && state.dialog.wait_for(std::chrono::seconds{0}) == std::future_status::ready)
	{
		auto const chosen{state.dialog.get()};
		if (false == chosen.paths.empty())
		{
			auto const shown{text_of(chosen.paths.front())};
			state.file.fill('\0');
			std::copy_n(shown.begin(), std::min(shown.size(), state.file.size() - 1), state.file.begin());
		}
		else if (false == chosen.available)
			state.status = STR("The system has no file dialog the editor can open: type the path of the file");
	}
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
	{
		editor_file_dialog::request request;
		request.title = STR("A sound file");
		request.many = false;
		request.filter_name = STR("Sounds");
		request.extensions = {"wav", "ogg", "flac"};
		request.owner = ImGui::GetMainViewport()->PlatformHandleRaw;
		state.dialog = std::async(std::launch::async, [request]() { return editor_file_dialog::open(request); });
	}
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat(STR_C("Over the ground (m)"), &state.height, 0.1f, 0.0f, 100.0f, "%.1f");
	ImGui::BeginDisabled(state.file[0] == '\0');
	if (main_button(state.placing ? STR_C("Click where it's to be heard...") : STR_C("Put a sound"), ImVec2(-FLT_MIN, tall_button_height())))
	{
		state.placing = false == state.placing;
		state.moving = false;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", STR_C("Then LMB on the ground puts it there; Shift+LMB puts one more"));
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
	ImGui::TextUnformatted(Sound.file.c_str());
	ImGui::SetNextItemWidth(160.0f);
	ImGui::DragFloat(STR_C("Heard as far as (m)"), &Sound.range, 1.0f, 5.0f, 3000.0f, "%.0f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", STR_C("The sound fades out over this distance, and it's set off when the camera comes this near"));
	std::array const plays{STR_C("In a loop, from when the camera comes near"), STR_C("Once every so many seconds, while the camera is near"), STR_C("In a loop between two hours")};
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::Combo("##ambientplay", &Sound.play, plays.data(), static_cast<int>(plays.size()));
	if (Sound.play == 1)
	{
		ImGui::SetNextItemWidth(160.0f);
		ImGui::DragFloat(STR_C("Every (s)"), &Sound.every, 1.0f, 1.0f, 3600.0f, "%.0f");
	}
	if (Sound.play == 2)
	{
		ImGui::SetNextItemWidth(80.0f);
		ImGui::InputInt(STR_C("From (HHMM)"), &Sound.from, 0);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		ImGui::InputInt(STR_C("To (HHMM)"), &Sound.to, 0);
		// 0000 is read by the launcher as no hour at all
		Sound.from = std::clamp(Sound.from, 1, 2359);
		Sound.to = std::clamp(Sound.to, 1, 2359);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", STR_C("It starts at the first hour and stops at the second; a scenario which starts between them stays silent until the first hour comes"));
		ImGui::PopTextWrapPos();
	}
	ImGui::DragScalarN(STR_C("Place##ambient"), ImGuiDataType_Double, &Sound.location.x, 3, 0.1f, nullptr, nullptr, "%.1f");
	auto const listening{state.previewing == state.selected};
	if (ImGui::Button(listening ? STR_C("Stop") : STR_C("Listen")))
		events_listen(listening ? -1 : state.selected);
	ImGui::SameLine();
	if (ImGui::Button(state.moving ? STR_C("Click the new place...") : STR_C("Move")))
	{
		state.moving = false == state.moving;
		state.placing = false;
	}
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Fly to##ambient")))
		Camera.Pos = Sound.location + glm::dvec3{0.0, 15.0, 25.0};
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Delete##ambient")))
	{
		events_listen(-1);
		state.sounds.erase(state.sounds.begin() + state.selected);
		state.selected = -1;
		state.status = STR("The sound deleted; it goes out of the scenery with the next save");
	}
}
