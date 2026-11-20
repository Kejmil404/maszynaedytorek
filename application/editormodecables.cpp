/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

// the field of work of the cables (experimental, F7): the lines of the scenery, power lines and the like. A cable of the editor is
// hung between its points and sags in each span between them; it's written on each save as a plain line_strip, of the colour and
// the width in pixels it's given, to an include of the editor's own, <scenery>_kable.scm, with a line of a comment before it that
// keeps what the editor reads back. The lines of the scenery files are read from them and can be taken over: they become cables of
// the editor, and their definitions go from their files on save. The game keeps the lines in its binary terrain file too, so that
// file goes when they change, made again on the next load

#include "stdafx.h"
#include "application/editormode.h"
#include "application/editormodeevents.h"
#include "application/editormodeshared.h"
#include "application/editorprojection.h"
#include "application/editoruilayer.h"
#include "editor/editorFormat.hpp"
#include "model/AnimModel.h"
#include "rendering/renderer.h"
#include "scene/scenelayers.h"
#include "simulation/simulation.h"
#include "utilities/Globals.h"
#include "utilities/translation.h"

#include "imgui/imgui.h"
#include "imgui/ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

using editor_events::fields_of;
using editor_events::name_of_typed;

char const *const kCablesHeader{"// made by the scenery editor on each save: the cables of the scenery (Cables)"};
std::string_view constexpr kCablesSuffix{"_kable.scm"};
// the line of each cable the editor reads back
std::string_view constexpr kCableMark{"//$k "};
std::size_t constexpr kCableFields{9};
// px: how near the cursor a point or a cable is picked, or snapped to
float constexpr kPickPixels{10.0f};
float constexpr kSnapPixels{14.0f};
// m: the farthest the lines of the scenery files are picked and snapped to
double constexpr kSceneryReach{1500.0};

// the colours a cable is given most often
struct colour_preset
{
	char const *name;
	std::array<int, 3> colour;
};
std::array const kColourPresets{colour_preset{"Black##cable", {25, 25, 25}}, colour_preset{"Copper##cable", {150, 95, 50}}, colour_preset{"Aluminium##cable", {165, 165, 170}}, colour_preset{"Grey##cable", {90, 90, 90}}};

std::string cables_file()
{
	return editor_events::editor_file(kCablesSuffix);
}

bool press_in_view()
{
	return false == ImGui::GetIO().MouseDownOwned[0];
}

// the points the game draws the cable with: each span sagging as a parabola, the deepest at its middle
std::vector<glm::dvec3> drawn_points(editor_mode_cable const &Cable)
{
	std::vector<glm::dvec3> points;
	if (Cable.points.size() < 2)
		return points;
	for (std::size_t span = 0; span + 1 < Cable.points.size(); ++span)
	{
		auto const &from{Cable.points[span]};
		auto const &to{Cable.points[span + 1]};
		auto const sag{span < Cable.sags.size() && Cable.sags[span] >= 0.0f ? Cable.sags[span] : Cable.sag};
		auto const pieces{sag > 0.0f ? std::clamp(Cable.segments, 1, 64) : 1};
		for (int piece = 0; piece < pieces; ++piece)
		{
			auto const t{static_cast<double>(piece) / pieces};
			auto point{glm::mix(from, to, t)};
			point.y -= sag * 4.0 * t * (1.0 - t);
			points.push_back(point);
		}
	}
	points.push_back(Cable.points.back());
	return points;
}

std::string point_text(glm::dvec3 const &Point)
{
	return format("%.3f %.3f %.3f", Point.x, Point.y, Point.z);
}

std::string cable_line(editor_mode_cable const &Cable)
{
	std::string points;
	for (auto const &point : Cable.points)
		points += (points.empty() ? "" : ";") + format("%.3f,%.3f,%.3f", point.x, point.y, point.z);
	std::string sags;
	for (std::size_t span = 0; span + 1 < Cable.points.size(); ++span)
		sags += (sags.empty() ? "" : ";") + format("%.2f", span < Cable.sags.size() ? Cable.sags[span] : -1.0f);
	return std::string{kCableMark} + Cable.name + '|' + format("%.1f|%.1f|%d,%d,%d|%.2f|%.2f|%d", Cable.range, Cable.range_min, Cable.colour[0], Cable.colour[1], Cable.colour[2], Cable.width, Cable.sag, Cable.segments) + '|' + points + '|' + sags + "\r\n";
}

// the cable as the simulator reads it: a line_strip of its points drawn
std::string cable_text(editor_mode_cable const &Cable)
{
	if (Cable.points.size() < 2)
		return {};
	std::string text{cable_line(Cable)};
	auto const range{Cable.range > 0.0f ? format("%.1f", Cable.range) : std::string{"-1"}};
	text += "node " + range + ' ' + format("%.1f", Cable.range_min) + ' ' + (Cable.name.empty() ? std::string{"none"} : Cable.name) + " line_strip " + format("%d %d %d %.2f", Cable.colour[0], Cable.colour[1], Cable.colour[2], Cable.width) + "\r\n";
	for (auto const &point : drawn_points(Cable))
		text += point_text(point) + "\r\n";
	return text + "endline\r\n";
}

glm::dvec3 point_of(std::string_view const Text)
{
	glm::dvec3 point{0.0};
	std::istringstream input{std::string{Text}};
	input.imbue(std::locale::classic());
	char comma{0};
	input >> point.x >> comma >> point.y >> comma >> point.z;
	return point;
}

bool read_cable(std::string_view Line, editor_mode_cable &Cable)
{
	if (false == Line.starts_with(kCableMark))
		return false;
	Line.remove_prefix(kCableMark.size());
	auto const fields{fields_of(Line)};
	if (fields.size() < kCableFields)
		return false;
	try
	{
		Cable.name = fields[0];
		Cable.range = std::stof(fields[1]);
		Cable.range_min = std::stof(fields[2]);
		std::istringstream colour{fields[3]};
		std::string component;
		for (std::size_t index = 0; index < 3 && std::getline(colour, component, ','); ++index)
			Cable.colour[index] = std::clamp(std::stoi(component), 0, 255);
		Cable.width = std::stof(fields[4]);
		Cable.sag = std::stof(fields[5]);
		Cable.segments = std::stoi(fields[6]);
		std::istringstream points{fields[7]};
		for (std::string point; std::getline(points, point, ';');)
			if (false == point.empty())
				Cable.points.push_back(point_of(point));
		std::istringstream sags{fields[8]};
		for (std::string sag; std::getline(sags, sag, ';');)
			if (false == sag.empty())
				Cable.sags.push_back(std::stof(sag));
	}
	catch (std::exception const &)
	{
		return false;
	}
	Cable.sags.resize(Cable.points.size() > 1 ? Cable.points.size() - 1 : 0, -1.0f);
	return Cable.points.size() >= 2;
}

// the words of a definition of lines as the scenery holds it: the comments left out
std::vector<std::string> words_of(std::string const &Text)
{
	std::vector<std::string> words;
	std::string word;
	auto const done = [&words, &word]() {
		if (false == word.empty())
			words.push_back(std::move(word));
		word.clear();
	};
	for (std::size_t at = 0; at < Text.size(); ++at)
	{
		if (Text.compare(at, 2, "//") == 0)
		{
			done();
			at = Text.find('\n', at);
			if (at == std::string::npos)
				break;
			continue;
		}
		if (Text.compare(at, 2, "/*") == 0)
		{
			done();
			at = Text.find("*/", at);
			if (at == std::string::npos)
				break;
			++at;
			continue;
		}
		auto const character{static_cast<unsigned char>(Text[at])};
		if (std::isspace(character) || Text[at] == ';' || Text[at] == ',')
			done();
		else
			word += Text[at];
	}
	done();
	return words;
}

double number_of(std::string const &Word)
{
	return std::atof(Word.c_str());
}

// the point of the file in the world, moved as the loader moves lines: turned about z, x and y, then offset
glm::dvec3 world_of(glm::dvec3 Point, scene::layer_context const &Context)
{
	if (Context.rotation != glm::vec3{0.0f})
	{
		auto const rotation{glm::radians(glm::dvec3{Context.rotation})};
		Point = glm::rotateZ<double>(Point, rotation.z);
		Point = glm::rotateX<double>(Point, rotation.x);
		Point = glm::rotateY<double>(Point, rotation.y);
	}
	return Point + Context.offset;
}

// the chains of points of the lines: a strip is one, a loop one closed, the pairs of lines joined where one starts at the end of
// the one before
std::vector<std::vector<glm::dvec3>> chains_of(std::string const &Type, std::vector<glm::dvec3> const &Points)
{
	std::vector<std::vector<glm::dvec3>> chains;
	if (Type != "lines")
	{
		if (Points.size() < 2)
			return chains;
		chains.push_back(Points);
		if (Type == "line_loop" && Points.size() > 2)
			chains.back().push_back(Points.front());
		return chains;
	}
	for (std::size_t index = 0; index + 1 < Points.size(); index += 2)
	{
		if (false == chains.empty() && glm::distance(chains.back().back(), Points[index]) < 1e-3)
		{
			chains.back().push_back(Points[index + 1]);
			continue;
		}
		chains.push_back({Points[index], Points[index + 1]});
	}
	return chains;
}

// the lines of a scenery file, as the editor shows them; false if the definition can't be read
bool read_scenery_cable(std::string const &Text, scene::layer_context const &Context, editor_mode_scenery_cable &Lines)
{
	auto const words{words_of(Text)};
	// node <range> <range min> <name> <type> r g b <width> x y z ... endline
	if (words.size() < 9 || words[0] != "node")
		return false;
	Lines.range = static_cast<float>(number_of(words[1]));
	Lines.range_min = static_cast<float>(number_of(words[2]));
	Lines.name = words[3];
	auto const &type{words[4]};
	for (std::size_t index = 0; index < 3; ++index)
		Lines.colour[index] = std::clamp(static_cast<int>(number_of(words[5 + index])), 0, 255);
	Lines.width = static_cast<float>(number_of(words[8]));
	std::vector<glm::dvec3> points;
	for (std::size_t index = 9; index + 2 < words.size() && words[index] != "endline"; index += 3)
		points.push_back(world_of({number_of(words[index]), number_of(words[index + 1]), number_of(words[index + 2])}, Context));
	Lines.chains = chains_of(type, points);
	if (Lines.chains.empty())
		return false;
	glm::dvec3 sum{0.0};
	std::size_t count{0};
	for (auto const &chain : Lines.chains)
		for (auto const &point : chain)
		{
			sum += point;
			++count;
		}
	Lines.centre = sum / static_cast<double>(count);
	for (auto const &chain : Lines.chains)
		for (auto const &point : chain)
			Lines.radius = std::max(Lines.radius, glm::distance(point, Lines.centre));
	return true;
}

// px, how far the segment from A to B on the screen passes from the mouse; a large number where it can't be seen
float screen_distance(screen_projection const &Projection, glm::dvec3 const &A, glm::dvec3 const &B, ImVec2 const &Mouse)
{
	ImVec2 a;
	ImVec2 b;
	if (false == Projection.project(A, a) || false == Projection.project(B, b))
		return std::numeric_limits<float>::max();
	glm::vec2 const from{a.x, a.y};
	glm::vec2 const along{b.x - a.x, b.y - a.y};
	glm::vec2 const mouse{Mouse.x, Mouse.y};
	auto const length2{glm::dot(along, along)};
	auto const t{length2 > 0.0f ? std::clamp(glm::dot(mouse - from, along) / length2, 0.0f, 1.0f) : 0.0f};
	return glm::distance(mouse, from + along * t);
}

ImU32 colour_of(std::array<int, 3> const &Colour, int const Alpha)
{
	return IM_COL32(Colour[0], Colour[1], Colour[2], Alpha);
}

editor_mode_cables content_of(std::vector<editor_mode_cable> const &Cables, std::vector<std::uint32_t> const &Taken)
{
	return {Cables, Taken};
}

} // namespace

void editor_mode::cables_workspace(bool const Open)
{
	m_cables_open = Open;
	if (Open)
	{
		cables_load();
		// the cables are selected and moved as the models are: the edit mode of the tracks would take the Inspector and the gizmo
		if (ui()->nodebank().mode == nodebank_panel::TRACK)
			ui()->nodebank().mode = nodebank_panel::MODIFY;
	}
	else
	{
		cables_cancel();
		m_cables.laying = false;
	}
}

void editor_mode::cables_load()
{
	auto &state{m_cables};
	if (state.loaded)
		return;
	state.loaded = true;
	cables_scan();
	auto const file{cables_file()};
	if (file.empty())
		return;
	std::ifstream input{Global.asCurrentSceneryPath + file, std::ios_base::binary};
	std::string line;
	while (std::getline(input, line))
	{
		if (false == line.empty() && line.back() == '\r')
			line.pop_back();
		if (cable read; read_cable(line, read))
			state.cables.push_back(std::move(read));
	}
	// read, not changed: no step of the history
	state.last = content_of(state.cables, state.taken);
}

void editor_mode::cables_scan()
{
	auto &state{m_cables};
	state.scanned = true;
	state.scenery.clear();
	state.scenery_selected = -1;
	auto const own{cables_file()};
	for (auto const &lines : scene::Layers.lines())
	{
		auto const &file{scene::Layers.layer(lines.layer).name};
		// the cables of the editor are its own, not lines of the scenery to take over
		if (false == own.empty() && (file == own || file.ends_with("/" + own) || file.ends_with("\\" + own)))
			continue;
		scenery_cable read;
		if (false == read_scenery_cable(scene::Layers.lines_text(lines), lines.context, read))
			continue;
		read.id = lines.id;
		read.file = file;
		read.taken = lines.erased;
		state.scenery.push_back(std::move(read));
	}
}

bool editor_mode::cable_lines(std::vector<std::string> &Rootstatements)
{
	auto &state{m_cables};
	auto const file{cables_file()};
	// a scenery the cables were never opened for: whatever its file holds stays as it is
	if (file.empty() || false == state.loaded)
		return false;
	std::string content{std::string{kCablesHeader} + "\r\n"};
	for (auto const &each : state.cables)
		content += cable_text(each);
	// the game keeps the lines in its binary terrain file as well: it's made again with the ones changed on the next load
	std::string current;
	{
		std::ifstream input{Global.asCurrentSceneryPath + file, std::ios_base::binary};
		current.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}
	if (current != content && (false == current.empty() || false == state.cables.empty()))
		scene::Layers.lines_changed();
	return editor_events::write_include(file, content, state.cables.empty(), state.included, Rootstatements);
}

void editor_mode::cables_deselect()
{
	auto &state{m_cables};
	state.selected = -1;
	state.point = -1;
	state.scenery_selected = -1;
}

void editor_mode::cables_cancel()
{
	auto &state{m_cables};
	state.laid.clear();
	state.extending = -1;
}

void editor_mode::cables_finish()
{
	auto &state{m_cables};
	if (state.extending >= 0)
	{
		// the points went onto the cable as they were laid
		state.extending = -1;
		state.laid.clear();
		return;
	}
	if (state.laid.size() < 2)
	{
		state.laid.clear();
		return;
	}
	auto made{state.model};
	made.points = std::move(state.laid);
	made.sags.assign(made.points.size() - 1, -1.0f);
	int number{1};
	auto const taken = [&state](std::string const &Name) { return std::ranges::any_of(state.cables, [&Name](cable const &Other) { return Other.name == Name; }); };
	made.name = format("kabel_%d", number);
	while (taken(made.name))
		made.name = format("kabel_%d", ++number);
	state.cables.push_back(std::move(made));
	state.laid.clear();
	cables_deselect();
	state.selected = static_cast<int>(state.cables.size()) - 1;
	state.status = format(STR_C("%s laid; it goes into the scenery with the next save"), state.cables.back().name.c_str());
}

void editor_mode::cables_delete()
{
	auto &state{m_cables};
	if (state.selected < 0 || state.selected >= static_cast<int>(state.cables.size()))
		return;
	auto &chosen{state.cables[state.selected]};
	// a point of a cable which keeps two at least, else the cable
	if (state.point >= 0 && state.point < static_cast<int>(chosen.points.size()) && chosen.points.size() > 2)
	{
		chosen.points.erase(chosen.points.begin() + state.point);
		auto const span{std::min(state.point, static_cast<int>(chosen.sags.size()) - 1)};
		if (span >= 0)
			chosen.sags.erase(chosen.sags.begin() + span);
		state.point = -1;
		return;
	}
	state.cables.erase(state.cables.begin() + state.selected);
	cables_deselect();
	state.status = STR("The cable deleted; it goes out of the scenery with the next save");
}

void editor_mode::cables_take_over(scenery_cable &Lines)
{
	auto &state{m_cables};
	if (false == scene::Layers.erase_lines(Lines.id, true))
	{
		state.status = format(STR_C("The lines can't be taken over: the file %s can't be changed"), Lines.file.c_str());
		return;
	}
	Lines.taken = true;
	state.taken.push_back(Lines.id);
	// a cable of each chain, its points as they are: no sag added, the lines look the same
	auto const base{Lines.name.empty() || Lines.name == "none" ? std::string{"kabel"} : name_of_typed(Lines.name)};
	for (std::size_t index = 0; index < Lines.chains.size(); ++index)
	{
		cable made;
		made.name = index == 0 ? base : format("%s_%zu", base.c_str(), index + 1);
		made.points = Lines.chains[index];
		made.sags.assign(made.points.size() - 1, -1.0f);
		made.sag = 0.0f;
		made.segments = 1;
		made.colour = Lines.colour;
		made.width = Lines.width;
		made.range = Lines.range;
		made.range_min = Lines.range_min;
		state.cables.push_back(std::move(made));
	}
	cables_deselect();
	state.selected = static_cast<int>(state.cables.size()) - 1;
	state.status = STR("Taken over: on save the lines go from their file to the file of the cables; the game shows them as they were until the scenery is loaded again");
}

// where the next point goes, from where the cursor is: a point of a model or of a cable near it on the screen with the magnet on,
// else the ground under it
void editor_mode::cables_snap()
{
	auto &state{m_cables};
	state.snap.reset();
	state.snap_kind = 0;
	if (ImGui::GetIO().WantCaptureMouse)
		return;
	auto const &mouse{ImGui::GetIO().MousePos};
	if (m_snap_objects)
	{
		// the model under the cursor, asked of the renderer now and then as the mouse moves
		if (--state.pick_wait <= 0 && (state.pick_mouse.x != mouse.x || state.pick_mouse.y != mouse.y))
		{
			state.pick_wait = 6;
			state.pick_mouse = {mouse.x, mouse.y};
			GfxRenderer->Pick_Node_Callback([this](scene::basic_node *Node) {
				auto const *model{dynamic_cast<TAnimModel const *>(Node)};
				if (model == m_cables.hover)
					return;
				m_cables.hover = model;
				m_cables.hover_points = model != nullptr ? model_points(*model) : std::vector<glm::dvec3>{};
			});
		}
		screen_projection const projection;
		auto best{kSnapPixels};
		auto const consider = [&projection, &mouse, &best, &state](glm::dvec3 const &Point, int const Kind) {
			ImVec2 at;
			if (false == projection.project(Point, at))
				return;
			auto const distance{std::hypot(at.x - mouse.x, at.y - mouse.y)};
			if (distance >= best)
				return;
			best = distance;
			state.snap = Point;
			state.snap_kind = Kind;
		};
		for (auto const &point : state.hover_points)
			consider(point, 1);
		for (std::size_t index = 0; index < state.cables.size(); ++index)
			for (std::size_t point = 0; point < state.cables[index].points.size(); ++point)
				// the point dragged doesn't snap to itself
				if (static_cast<int>(index) != state.selected || static_cast<int>(point) != state.point || false == ImGuizmo::IsUsing())
					consider(state.cables[index].points[point], 2);
		for (auto const &point : state.laid)
			consider(point, 2);
		glm::dvec3 const camera{Camera.Pos};
		for (auto const &lines : state.scenery)
			if (false == lines.taken && glm::distance(lines.centre, camera) < kSceneryReach + lines.radius)
				for (auto const &chain : lines.chains)
					for (auto const &point : chain)
						consider(point, 2);
		if (state.snap.has_value())
			return;
	}
	if (glm::dvec3 ground; cursor_on_ground(ground))
		state.snap = ground + glm::dvec3{0.0, state.height, 0.0};
}

bool editor_mode::cables_click()
{
	auto &state{m_cables};
	if (ImGui::GetIO().WantCaptureMouse)
		return false;
	// a press on the gizmo of the point selected moves it
	if (state.point >= 0 && gizmo_shown() && (ImGuizmo::IsOver() || ImGuizmo::IsUsing()))
		return true;
	if (state.laying)
	{
		if (false == state.snap.has_value())
			return true;
		auto const point{*state.snap};
		// a click on the last point ends the cable
		if (false == state.laid.empty() && glm::distance(state.laid.back(), point) < 0.01)
		{
			cables_finish();
			return true;
		}
		if (state.extending >= 0 && state.extending < static_cast<int>(state.cables.size()))
		{
			auto &extended{state.cables[state.extending]};
			extended.points.push_back(point);
			extended.sags.push_back(-1.0f);
		}
		state.laid.push_back(point);
		return true;
	}
	screen_projection const projection;
	auto const &mouse{ImGui::GetIO().MousePos};
	// a point of the cable selected, then a cable of the editor, then lines of the scenery
	if (state.selected >= 0 && state.selected < static_cast<int>(state.cables.size()))
	{
		auto &chosen{state.cables[state.selected]};
		auto best{kPickPixels};
		int point{-1};
		for (std::size_t index = 0; index < chosen.points.size(); ++index)
			if (ImVec2 at; projection.project(chosen.points[index], at) && std::hypot(at.x - mouse.x, at.y - mouse.y) < best)
			{
				best = std::hypot(at.x - mouse.x, at.y - mouse.y);
				point = static_cast<int>(index);
			}
		if (point >= 0)
		{
			state.point = point;
			return true;
		}
		// Ctrl+LMB on a span of it: a point put in it, where the cursor is along the span
		if (Global.ctrlState)
		{
			for (std::size_t span = 0; span + 1 < chosen.points.size(); ++span)
			{
				ImVec2 a;
				ImVec2 b;
				if (screen_distance(projection, chosen.points[span], chosen.points[span + 1], mouse) >= kPickPixels * 2.0f || false == projection.project(chosen.points[span], a) || false == projection.project(chosen.points[span + 1], b))
					continue;
				glm::vec2 const along{b.x - a.x, b.y - a.y};
				auto const t{std::clamp(glm::dot(glm::vec2{mouse.x - a.x, mouse.y - a.y}, along) / std::max(1e-6f, glm::dot(along, along)), 0.05f, 0.95f)};
				chosen.points.insert(chosen.points.begin() + static_cast<std::ptrdiff_t>(span) + 1, glm::mix(chosen.points[span], chosen.points[span + 1], static_cast<double>(t)));
				auto const sag{span < chosen.sags.size() ? chosen.sags[span] : -1.0f};
				chosen.sags.insert(chosen.sags.begin() + static_cast<std::ptrdiff_t>(std::min(span, chosen.sags.size())), sag);
				state.point = static_cast<int>(span) + 1;
				return true;
			}
		}
	}
	cables_deselect();
	auto best{kPickPixels};
	for (std::size_t index = 0; index < state.cables.size(); ++index)
	{
		auto const points{drawn_points(state.cables[index])};
		for (std::size_t piece = 0; piece + 1 < points.size(); ++piece)
			if (auto const distance{screen_distance(projection, points[piece], points[piece + 1], mouse)}; distance < best)
			{
				best = distance;
				state.selected = static_cast<int>(index);
			}
	}
	if (state.selected >= 0)
		return true;
	glm::dvec3 const camera{Camera.Pos};
	for (std::size_t index = 0; index < state.scenery.size(); ++index)
	{
		auto const &lines{state.scenery[index]};
		if (lines.taken || glm::distance(lines.centre, camera) > kSceneryReach + lines.radius)
			continue;
		for (auto const &chain : lines.chains)
			for (std::size_t piece = 0; piece + 1 < chain.size(); ++piece)
				if (auto const distance{screen_distance(projection, chain[piece], chain[piece + 1], mouse)}; distance < best)
				{
					best = distance;
					state.scenery_selected = static_cast<int>(index);
				}
	}
	return true;
}

void editor_mode::cables_track_changes()
{
	auto &state{m_cables};
	if (false == state.loaded)
		return;
	auto current{content_of(state.cables, state.taken)};
	if (current != state.last && false == state.base.has_value())
		state.base = state.last;
	state.last = std::move(current);
	// done when nothing is held: a field typed in, a value dragged, the gizmo
	if (false == state.base.has_value() || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing())
		return;
	auto before{std::move(*state.base)};
	state.base.reset();
	if (before == state.last)
		return;
	if (m_max_history_size >= 0 && static_cast<int>(m_history.size()) >= m_max_history_size)
		m_history.erase(m_history.begin(), m_history.begin() + (static_cast<int>(m_history.size()) - m_max_history_size + 1));
	EditorSnapshot step;
	step.action = EditorSnapshot::Action::Cables;
	step.node_name = STR("Cables");
	step.cables = std::move(before);
	m_history.push_back(std::move(step));
	g_redo.clear();
}

void editor_mode::restore_cables(EditorSnapshot &Snapshot)
{
	auto &state{m_cables};
	if (false == Snapshot.cables.has_value())
		return;
	// what they are now goes to the step, for the way back
	std::swap(Snapshot.cables->cables, state.cables);
	std::swap(Snapshot.cables->taken, state.taken);
	state.loaded = true;
	state.base.reset();
	state.last = content_of(state.cables, state.taken);
	cables_cancel();
	// the lines of the scenery files go from them on save as they're taken over in what's restored
	for (auto &lines : state.scenery)
	{
		auto const taken{std::ranges::find(state.taken, lines.id) != state.taken.end()};
		if (taken != lines.taken && scene::Layers.erase_lines(lines.id, taken))
			lines.taken = taken;
	}
	if (state.selected >= static_cast<int>(state.cables.size()))
		cables_deselect();
	if (state.selected >= 0 && state.point >= static_cast<int>(state.cables[state.selected].points.size()))
		state.point = -1;
}

void editor_mode::cables_saved()
{
	auto &state{m_cables};
	if (false == state.loaded)
		return;
	if (false == state.taken.empty())
	{
		auto const cables = [](EditorSnapshot const &Step) { return Step.action == EditorSnapshot::Action::Cables; };
		std::erase_if(m_history, cables);
		std::erase_if(g_redo, cables);
		state.taken.clear();
		state.base.reset();
		state.last = content_of(state.cables, state.taken);
	}
	// the lines of the files rewritten are somewhere else in them now: read again
	cables_scan();
}

void editor_mode::render_cables_gizmo()
{
	auto &state{m_cables};
	m_gizmo_using = false;
	if (state.selected < 0 || state.selected >= static_cast<int>(state.cables.size()) || state.point < 0 || state.point >= static_cast<int>(state.cables[state.selected].points.size()))
		return;
	auto &location{state.cables[state.selected].points[state.point]};
	ImGuizmo::BeginFrame();
	ImGuizmo::SetOrthographic(Global.EditorOrtho);
	ImGuiIO const &io{ImGui::GetIO()};
	ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
	glm::mat4 const view{GfxRenderer->Camera_View_Matrix()};
	glm::dvec3 const camerapos{GfxRenderer->Camera_Position()};
	float const aspect{io.DisplaySize.y > 0.0f ? io.DisplaySize.x / io.DisplaySize.y : 1.0f};
	glm::mat4 const projection{editor_mode::projection_matrix(aspect)};
	// a point has no turn and no scale: it's moved only, along the axes of the world
	glm::vec3 const relative{location - camerapos};
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
	location = camerapos + glm::dvec3{moved[0], moved[1], moved[2]};
	// with the magnet on the point dragged near a point of a model or of a cable goes onto it
	if (m_snap_objects && state.snap_kind != 0 && state.snap.has_value())
		location = *state.snap;
}

void editor_mode::render_cable_model(cable &Model)
{
	if (false == begin_fields("##cablemodel", 0.4f))
		return;
	field_row("Colour##cable");
	std::array<float, 3> colour{Model.colour[0] / 255.0f, Model.colour[1] / 255.0f, Model.colour[2] / 255.0f};
	if (ImGui::ColorEdit3("##cablecolour", colour.data(), ImGuiColorEditFlags_NoInputs))
		for (std::size_t index = 0; index < 3; ++index)
			Model.colour[index] = std::clamp(static_cast<int>(std::lround(colour[index] * 255.0f)), 0, 255);
	for (auto const &preset : kColourPresets)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton(STR_C(preset.name)))
			Model.colour = preset.colour;
	}
	field_row("Width (px)##cable");
	ImGui::DragFloat("##cablewidth", &Model.width, 0.05f, 0.5f, 30.0f, "%.2f");
	item_tooltip("In pixels on the screen, as the game draws the lines: the same near and far. 1 to 2 for the wires of a power line");
	field_row("Sag (m)##cable");
	ImGui::DragFloat("##cablesag", &Model.sag, 0.05f, 0.0f, 30.0f, "%.2f");
	item_tooltip("How far the middle of each span hangs below the straight line between its points; a span may have a sag of its own");
	field_row("Pieces a span##cable");
	ImGui::SliderInt("##cablesegments", &Model.segments, 1, 32);
	item_tooltip("The straight pieces a sagging span is drawn with: more is rounder, and more for the game to draw");
	field_row("Seen up to (m)##cable");
	ImGui::DragFloat("##cablerange", &Model.range, 10.0f, 0.0f, 10000.0f, "%.0f");
	item_tooltip("0: from any distance");
	ImGui::EndTable();
}

void editor_mode::render_cables_tool_options()
{
	auto &state{m_cables};
	cables_load();
	if (state.laying)
	{
		ImGui::SeparatorText(STR_C("Lay a cable"));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", STR_C("LMB puts the points the cable hangs from, one after another; Enter or LMB on the last point ends it, Esc drops it. With the magnet on a point goes onto a point of a model (an insulator) or of another cable near the cursor"));
		ImGui::PopTextWrapPos();
		render_cable_model(state.model);
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat(STR_C("Over the ground (m)##cable"), &state.height, 0.1f, 0.0f, 100.0f, "%.1f");
		item_tooltip("The height of a point put on the ground, not onto a model or a cable");
		if (false == state.laid.empty())
			ImGui::Text(STR_C("Points laid: %zu"), state.laid.size());
	}
	else
	{
		ImGui::SeparatorText(STR_C("Cables"));
		ImGui::TextDisabled("%s", STR_C("LMB: select a cable or a point of it   Gizmo: move the point   Ctrl+LMB: a point into the span   Del: delete"));
		for (std::size_t index = 0; index < state.cables.size(); ++index)
			if (ImGui::Selectable(format(STR_C("%s  (points: %zu)##cable%zu"), state.cables[index].name.c_str(), state.cables[index].points.size(), index).c_str(), static_cast<int>(index) == state.selected))
			{
				cables_deselect();
				state.selected = static_cast<int>(index);
			}
		if (state.cables.empty())
			ImGui::TextDisabled("%s", STR_C("No cables yet: the Lay tool (2) lays them"));
		auto const others{static_cast<std::size_t>(std::ranges::count_if(state.scenery, [](scenery_cable const &Lines) { return false == Lines.taken; }))};
		ImGui::TextDisabled(STR_C("Lines of the scenery files: %zu; LMB on one shows it, it can be taken over"), others);
	}
	if (false == state.status.empty())
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", state.status.c_str());
		ImGui::PopTextWrapPos();
	}
}

void editor_mode::render_cables_selection()
{
	auto &state{m_cables};
	if (state.scenery_selected >= 0 && state.scenery_selected < static_cast<int>(state.scenery.size()))
	{
		render_scenery_cable(state.scenery[state.scenery_selected]);
		return;
	}
	if (state.selected < 0 || state.selected >= static_cast<int>(state.cables.size()))
	{
		ImGui::TextDisabled("%s", STR_C("Click a cable in the view, or in the list, to change it"));
		return;
	}
	render_cable(state.cables[state.selected]);
}

void editor_mode::render_cable(cable &Cable)
{
	auto &state{m_cables};
	ImGui::SeparatorText(Cable.name.c_str());
	if (state.renaming != state.selected)
	{
		state.renaming = state.selected;
		state.rename.fill('\0');
		std::copy_n(Cable.name.begin(), std::min(Cable.name.size(), state.rename.size() - 1), state.rename.begin());
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputText("##cablename", state.rename.data(), state.rename.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
	{
		if (auto const name{name_of_typed(state.rename.data())}; false == name.empty())
			Cable.name = name;
		state.renaming = -1;
	}
	item_tooltip("The name of the line_strip node; letters, digits, _ and -");
	render_cable_model(Cable);
	double length{0.0};
	for (std::size_t span = 0; span + 1 < Cable.points.size(); ++span)
		length += glm::distance(Cable.points[span], Cable.points[span + 1]);
	ImGui::TextDisabled(STR_C("Spans: %zu, %.1f m"), Cable.points.size() - 1, length);
	render_cable_points(Cable);
	ImGui::Separator();
	if (ImGui::Button(STR_C("Lay on from its end")))
	{
		state.laying = true;
		state.extending = state.selected;
		state.laid = {Cable.points.back()};
	}
	item_tooltip("The Lay tool goes on from the last point of this cable: each click adds a point to it");
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Fly to##cable")))
		Camera.Pos = Cable.points.front() + glm::dvec3{0.0, 15.0, 25.0};
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Delete##cable")))
	{
		state.point = -1;
		cables_delete();
	}
}

void editor_mode::render_cable_points(cable &Cable)
{
	auto &state{m_cables};
	Cable.sags.resize(Cable.points.size() > 1 ? Cable.points.size() - 1 : 0, -1.0f);
	if (false == ImGui::CollapsingHeader(STR_C("Points##cable"), ImGuiTreeNodeFlags_DefaultOpen))
		return;
	for (std::size_t index = 0; index < Cable.points.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::RadioButton("##point", state.point == static_cast<int>(index)))
			state.point = static_cast<int>(index);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::DragScalarN("##at", ImGuiDataType_Double, &Cable.points[index].x, 3, 0.05f, nullptr, nullptr, "%.2f");
		if (index + 1 < Cable.points.size())
		{
			// the sag of the span after the point: the one of the cable, or its own
			auto own{Cable.sags[index] >= 0.0f};
			ImGui::Indent();
			if (ImGui::Checkbox(STR_C("Sag of its own##span"), &own))
				Cable.sags[index] = own ? Cable.sag : -1.0f;
			if (own)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100.0f);
				ImGui::DragFloat("##spansag", &Cable.sags[index], 0.05f, 0.0f, 30.0f, "%.2f m");
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
}

void editor_mode::render_scenery_cable(scenery_cable &Lines)
{
	ImGui::SeparatorText(Lines.name.empty() ? STR_C("Lines of the scenery") : Lines.name.c_str());
	ImGui::TextDisabled(STR_C("File: %s"), Lines.file.c_str());
	std::size_t points{0};
	for (auto const &chain : Lines.chains)
		points += chain.size();
	ImGui::TextDisabled(STR_C("Lines drawn on: %zu, points: %zu, width %.2f px"), Lines.chains.size(), points, Lines.width);
	ImGui::ColorButton("##scenerycolour", ImVec4(Lines.colour[0] / 255.0f, Lines.colour[1] / 255.0f, Lines.colour[2] / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip);
	if (Lines.taken)
	{
		ImGui::TextDisabled("%s", STR_C("Taken over: the cables of the editor hold them now"));
		return;
	}
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", STR_C("Lines of a scenery file, drawn by the game. Taken over, they become cables of the editor, their points as they are; on save their definition goes from the file to the file of the cables"));
	ImGui::PopTextWrapPos();
	if (ImGui::Button(STR_C("Take over for editing")))
		cables_take_over(Lines);
	item_tooltip("Ctrl+Z gives them back to their file");
}

void editor_mode::draw_cables_overlay()
{
	auto &state{m_cables};
	if (false == m_cables_open)
		return;
	cables_snap();
	screen_projection const projection;
	auto *drawlist{ImGui::GetBackgroundDrawList(ImGui::GetMainViewport())};
	// a piece is drawn where it isn't under the terrain
	auto const piece = [&projection, drawlist, this](glm::dvec3 const &A, glm::dvec3 const &B, ImU32 const Colour, float const Width) {
		auto const middle{(A + B) * 0.5};
		if (double ground; m_streamer.height_at(middle.x, middle.z, ground) && middle.y < ground - 0.2)
			return;
		ImVec2 a;
		ImVec2 b;
		if (projection.project(A, a) && projection.project(B, b))
			drawlist->AddLine(a, b, Colour, Width);
	};
	for (std::size_t index = 0; index < state.cables.size(); ++index)
	{
		auto const &each{state.cables[index]};
		auto const chosen{static_cast<int>(index) == state.selected};
		auto const points{drawn_points(each)};
		for (std::size_t at = 0; at + 1 < points.size(); ++at)
			piece(points[at], points[at + 1], chosen ? IM_COL32(255, 190, 60, 255) : colour_of(each.colour, 230), chosen ? std::max(2.0f, each.width + 1.0f) : std::max(1.0f, each.width));
		if (false == state.points && false == chosen)
			continue;
		for (std::size_t at = 0; at < each.points.size(); ++at)
			if (ImVec2 screen; projection.project(each.points[at], screen))
			{
				auto const marked{chosen && static_cast<int>(at) == state.point};
				drawlist->AddCircleFilled(screen, marked ? 6.0f : 4.0f, marked ? IM_COL32(255, 120, 40, 255) : IM_COL32(255, 235, 150, 220), 10);
			}
	}
	// the lines of the scenery: the one selected, or all of them near the camera when asked
	glm::dvec3 const camera{Camera.Pos};
	for (std::size_t index = 0; index < state.scenery.size(); ++index)
	{
		auto const &lines{state.scenery[index]};
		auto const chosen{static_cast<int>(index) == state.scenery_selected};
		if (lines.taken || (false == chosen && (false == state.scenery_shown || glm::distance(lines.centre, camera) > kSceneryReach + lines.radius)))
			continue;
		for (auto const &chain : lines.chains)
			for (std::size_t at = 0; at + 1 < chain.size(); ++at)
				piece(chain[at], chain[at + 1], chosen ? IM_COL32(120, 200, 255, 255) : IM_COL32(120, 200, 255, 140), chosen ? 3.0f : 1.5f);
	}
	// the cable being laid, up to the cursor
	if (state.laying)
	{
		for (std::size_t at = 0; at + 1 < state.laid.size(); ++at)
			piece(state.laid[at], state.laid[at + 1], IM_COL32(255, 190, 60, 255), 2.0f);
		for (auto const &point : state.laid)
			if (ImVec2 screen; projection.project(point, screen))
				drawlist->AddCircleFilled(screen, 4.0f, IM_COL32(255, 190, 60, 255), 10);
		if (state.snap.has_value() && false == state.laid.empty())
			piece(state.laid.back(), *state.snap, IM_COL32(255, 190, 60, 150), 1.5f);
	}
	// where the next point goes: a ring on a point of a model or a cable, a cross on the ground
	if ((state.laying || ImGuizmo::IsUsing()) && state.snap.has_value())
		if (ImVec2 screen; projection.project(*state.snap, screen))
		{
			if (state.snap_kind != 0)
				drawlist->AddCircle(screen, 9.0f, IM_COL32(80, 255, 120, 255), 16, 2.5f);
			else
			{
				drawlist->AddLine(ImVec2(screen.x - 6.0f, screen.y), ImVec2(screen.x + 6.0f, screen.y), IM_COL32(255, 255, 255, 200), 1.5f);
				drawlist->AddLine(ImVec2(screen.x, screen.y - 6.0f), ImVec2(screen.x, screen.y + 6.0f), IM_COL32(255, 255, 255, 200), 1.5f);
			}
		}
}
