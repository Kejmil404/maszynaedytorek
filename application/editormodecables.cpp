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
#include "editor/editorIncludeInfo.hpp"
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
#include <filesystem>
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

std::filesystem::path path_of_text(std::string_view const Text)
{
	return std::filesystem::path{std::u8string{Text.begin(), Text.end()}};
}

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


// --- the wiring: the blocks of the templates of the poles, as druty.exe read them ---

// a line of the template, its comment marks taken off: the block may be commented out, so the simulator doesn't read it
std::string_view uncommented(std::string_view Line)
{
	while (false == Line.empty() && (Line.front() == ' ' || Line.front() == '\t'))
		Line.remove_prefix(1);
	while (Line.starts_with("//"))
	{
		Line.remove_prefix(2);
		while (false == Line.empty() && (Line.front() == ' ' || Line.front() == '\t'))
			Line.remove_prefix(1);
	}
	while (false == Line.empty() && (Line.back() == '\r' || Line.back() == ' ' || Line.back() == '\t'))
		Line.remove_suffix(1);
	return Line;
}

std::vector<std::string> line_words(std::string_view const Line)
{
	std::vector<std::string> words;
	std::istringstream input{std::string{Line}};
	for (std::string word; input >> word;)
		words.push_back(word);
	return words;
}

bool numeric(std::string const &Word, double &Value)
{
	char *end{nullptr};
	Value = std::strtod(Word.c_str(), &end);
	return end != Word.c_str() && *end == '\0';
}

// the blocks: each a line with the name of a template, then the numbers of the parameters of the place and the turns; a line of three
// numbers for each point, empty lines between them left out; Obrot=... at the end, or not
editor_mode_wiring_block read_block(std::string const &Text)
{
	editor_mode_wiring_block block;
	std::istringstream input{Text};
	std::string label;
	editor_mode_wiring_set *set{nullptr};
	for (std::string raw; std::getline(input, raw);)
	{
		std::string_view const plain{uncommented(raw)};
		auto const commented{uncommented(raw).size() != std::string_view{raw}.size() && std::string_view{raw}.find("//") != std::string_view::npos};
		auto const words{line_words(plain)};
		if (words.empty())
			continue;
		// a header: the start of a set
		if (words.size() >= 4 && ToLower(words[0]).ends_with(".inc"))
		{
			std::vector<int> parameters;
			for (std::size_t index = 1; index < words.size(); ++index)
				if (double value; numeric(words[index], value) && value >= 1.0)
					parameters.push_back(static_cast<int>(value));
			if (parameters.size() >= 3)
			{
				if (block.parameters.empty())
					block.parameters = std::move(parameters);
				block.sets.push_back({label, {}});
				set = &block.sets.back();
				continue;
			}
		}
		if (ToLower(std::string{plain}).starts_with("obrot"))
		{
			block.turned = ToLower(std::string{plain}).find('y') != std::string::npos;
			set = nullptr;
			continue;
		}
		std::array<double, 3> point{};
		if (set != nullptr && false == commented && words.size() >= 3 && numeric(words[0], point[0]) && numeric(words[1], point[1]) && numeric(words[2], point[2]))
		{
			set->points.emplace_back(point[0], point[1], point[2]);
			continue;
		}
		// anything else ends the set; a comment names the next one
		set = nullptr;
		if (commented)
			label = std::string{plain};
	}
	std::erase_if(block.sets, [](editor_mode_wiring_set const &Set) { return Set.points.empty(); });
	block.found = false == block.sets.empty();
	return block;
}

// the point of the block in the frame of the scenery, by one of the ways its numbers may stand for the axes: the height last, as
// in 3ds Max, or in the middle, as in the scenery, and the other two either way
glm::dvec3 axes_of(glm::dvec3 const &Point, int const Axes)
{
	auto const flip_x{(Axes & 1) != 0 ? -1.0 : 1.0};
	auto const flip_z{(Axes & 2) != 0 ? -1.0 : 1.0};
	if ((Axes & 4) == 0)
		return {Point.x * flip_x, Point.z, Point.y * flip_z};
	return {Point.x * flip_x, Point.y, Point.z * flip_z};
}

// with the turn of the pole either way
int constexpr kAxesWays{16};

// a number of the include, by the number of its parameter
double parameter_value(std::vector<std::string> const &Values, std::vector<int> const &Parameters, std::size_t const Which)
{
	if (Which >= Parameters.size())
		return 0.0;
	auto const index{static_cast<std::size_t>(Parameters[Which] - 1)};
	double value{0.0};
	if (index < Values.size())
		numeric(Values[index], value);
	return value;
}

// a number in 0..1 of the seed and the span: the same on each build and each system, unlike the distributions of the library
double random_of(std::uint64_t Value)
{
	Value += 0x9e3779b97f4a7c15ULL;
	Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebULL;
	Value ^= Value >> 31;
	return static_cast<double>(Value >> 11) / static_cast<double>(1ULL << 53);
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
	if (state.wiring)
	{
		// the pole under the cursor, as the renderer tells it
		GfxRenderer->Pick_Node_Callback([this](scene::basic_node *Node) {
			if (press_in_view())
				wiring_pick(dynamic_cast<TAnimModel const *>(Node));
		});
		return true;
	}
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
	if (state.wiring)
		render_wiring_options();
	else if (state.laying)
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
	// the poles of the wiring, their points and the wires to be led
	if (state.wiring)
	{
		for (std::size_t index = 0; index < state.poles.size(); ++index)
		{
			auto const &pole{state.poles[index]};
			if (ImVec2 screen; projection.project(pole.origin, screen))
			{
				drawlist->AddCircleFilled(screen, 7.0f, pole.sets.empty() ? IM_COL32(255, 120, 60, 230) : IM_COL32(80, 220, 120, 230), 12);
				drawlist->AddText(ImVec2(screen.x + 9.0f, screen.y - 8.0f), IM_COL32(255, 255, 255, 230), std::to_string(index + 1).c_str());
			}
			for (auto const &set : pole.sets)
				for (auto const &point : set)
					if (ImVec2 screen; projection.project(point, screen))
						drawlist->AddCircle(screen, 4.0f, IM_COL32(80, 255, 120, 255), 8, 1.5f);
		}
		for (auto const &wire : wiring_cables())
		{
			auto const points{drawn_points(wire)};
			for (std::size_t at = 0; at + 1 < points.size(); ++at)
				piece(points[at], points[at + 1], IM_COL32(255, 220, 60, 220), 1.5f);
		}
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

// the pole of the model picked: the include of its template, the block of the template, the points where the wires hang
bool editor_mode::wiring_pole(TAnimModel const &Model, editor_mode_wiring_pole &Pole)
{
	auto &state{m_cables};
	Pole = {};
	Pole.model = &Model;
	std::vector<std::string> values;
	scene::layer_context context;
	glm::dvec3 local{0.0};
	// the include of the template, when the scenery was opened for editing; else the template named as the model is, placed where
	// the model stands, turned as it's turned
	auto const instance{Model.m_instance};
	auto const tracked{scene::Layers.tracked(instance) && scene::Layers.instance(instance).file != nullptr};
	if (tracked)
	{
		auto const &include{scene::Layers.instance(instance)};
		Pole.instance = instance;
		Pole.file = *include.file;
		context = include.context;
		std::string file;
		editor_includes::parse_directive(scene::Layers.directive(instance), file, values);
	}
	else
		Pole.file = wiring_template(Model);
	if (Pole.file.empty())
		return false;
	auto lookup{state.blocks.find(Pole.file)};
	if (lookup == state.blocks.end())
	{
		std::ifstream input{Global.asCurrentSceneryPath + Pole.file, std::ios_base::binary};
		std::string const text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		lookup = state.blocks.emplace(Pole.file, read_block(text)).first;
	}
	auto &block{lookup->second};
	double turn{0.0};
	if (tracked)
	{
		local = {parameter_value(values, block.parameters, 0), parameter_value(values, block.parameters, 1), parameter_value(values, block.parameters, 2)};
		Pole.origin = context.to_world(local);
		turn = (block.turned ? parameter_value(values, block.parameters, 3) : 0.0) + static_cast<double>(context.rotation.y);
	}
	else
	{
		Pole.origin = Model.location();
		turn = block.turned ? static_cast<double>(Model.Angles().y) : 0.0;
	}
	if (false == block.found)
		return false;
	auto const yaw{glm::radians(turn)};
	auto const place = [&Pole, yaw](glm::dvec3 const &Point, int const Axes) { return Pole.origin + glm::rotateY(axes_of(Point, Axes), (Axes & 8) != 0 ? -yaw : yaw); };
	// the way the numbers stand for the axes and the turn goes, worked out once a template: the one whose points lie nearest to the
	// model of the pole
	if (block.axes < 0)
	{
		auto const corners{model_points(Model)};
		auto best{std::numeric_limits<double>::max()};
		std::size_t count{0};
		block.axes = 0;
		for (int axes = 0; axes < kAxesWays && false == corners.empty(); ++axes)
		{
			double sum{0.0};
			count = 0;
			for (auto const &set : block.sets)
				for (auto const &point : set.points)
				{
					auto const at{place(point, axes)};
					auto nearest{std::numeric_limits<double>::max()};
					for (auto const &corner : corners)
						nearest = std::min(nearest, glm::distance(at, corner));
					sum += nearest;
					++count;
				}
			if (sum < best)
			{
				best = sum;
				block.axes = axes;
			}
		}
		if (count > 0 && best / static_cast<double>(count) > 1.0)
			state.status = format(STR_C("The points of %s lie %.1f m from its model on average: check the block of its template"), Pole.file.c_str(), best / static_cast<double>(count));
	}
	for (auto const &set : block.sets)
	{
		std::vector<glm::dvec3> points;
		for (auto const &point : set.points)
			points.push_back(place(point, block.axes));
		Pole.main = std::max(Pole.main, points.size());
		Pole.sets.push_back(std::move(points));
		Pole.labels.push_back(set.label);
	}
	return true;
}

void editor_mode::wiring_pick(TAnimModel const *Model)
{
	auto &state{m_cables};
	if (Model == nullptr)
		return;
	// a pole clicked again goes from the list
	if (std::erase_if(state.poles, [Model](editor_mode_wiring_pole const &Pole) { return Pole.model == Model || (Pole.instance != 0 && Pole.instance == Model->m_instance); }) > 0)
		return;
	editor_mode_wiring_pole pole;
	if (false == wiring_pole(*Model, pole) && pole.file.empty())
	{
		state.status = STR("No template of this model was found: the wires are led along the poles whose template (*.inc) is named as their model");
		return;
	}
	if (pole.sets.empty())
		state.status = format(STR_C("The template %s has no block for the automatons of the wires: the wires are led along poles which all have it"), pole.file.c_str());
	state.poles.push_back(std::move(pole));
}

// the poles one after another along the line: from the one at an end, each to the nearest one left
void editor_mode::wiring_order()
{
	auto &poles{m_cables.poles};
	if (poles.size() < 3)
		return;
	glm::dvec3 middle{0.0};
	for (auto const &pole : poles)
		middle += pole.origin;
	middle /= static_cast<double>(poles.size());
	auto const first{std::ranges::max_element(poles, [&middle](auto const &Left, auto const &Right) { return glm::distance(Left.origin, middle) < glm::distance(Right.origin, middle); })};
	std::iter_swap(poles.begin(), first);
	for (auto at{poles.begin()}; std::next(at) != poles.end(); ++at)
	{
		auto const from{at->origin};
		auto const next{std::min_element(std::next(at), poles.end(), [&from](auto const &Left, auto const &Right) { return glm::distance(Left.origin, from) < glm::distance(Right.origin, from); })};
		std::iter_swap(std::next(at), next);
	}
}

namespace
{

// the set of the pole a span hangs from: of those of its kind, the one nearest to the pole at the other end of the span, as a strain
// pole has one set facing each way
std::vector<glm::dvec3> const *set_towards(editor_mode_wiring_pole const &Pole, glm::dvec3 const &Towards, bool const Main)
{
	std::vector<glm::dvec3> const *chosen{nullptr};
	auto best{std::numeric_limits<double>::max()};
	for (auto const &set : Pole.sets)
	{
		if ((set.size() == Pole.main) != Main)
			continue;
		glm::dvec3 middle{0.0};
		for (auto const &point : set)
			middle += point;
		middle /= static_cast<double>(set.size());
		if (auto const distance{glm::distance(glm::dvec2{middle.x, middle.z}, glm::dvec2{Towards.x, Towards.z})}; distance < best)
		{
			best = distance;
			chosen = &set;
		}
	}
	return chosen;
}

// the points across the span, from its left to its right and from the bottom up: the same order at both ends, so the wires don't
// cross, whichever way the poles stand
std::vector<glm::dvec3> across(std::vector<glm::dvec3> Points, glm::dvec3 const &From, glm::dvec3 const &To)
{
	glm::dvec2 along{To.x - From.x, To.z - From.z};
	along /= std::max(1e-6, glm::length(along));
	glm::dvec2 const right{along.y, -along.x};
	glm::dvec2 const base{From.x, From.z};
	std::ranges::sort(Points, [&right, &base](glm::dvec3 const &Left, glm::dvec3 const &Right) {
		auto const left_side{std::round(glm::dot(glm::dvec2{Left.x, Left.z} - base, right) * 4.0)};
		auto const right_side{std::round(glm::dot(glm::dvec2{Right.x, Right.z} - base, right) * 4.0)};
		return left_side != right_side ? left_side < right_side : Left.y < Right.y;
	});
	return Points;
}

} // namespace

// the wires along the poles: each span hangs from the set of each pole facing the other one, the points of the two in the same order
// across the span; a strain pole joins the set the wires come in at with the one they go out at, as its jumpers do. A sag is picked
// at random for each span, the same for its wires; the sets of fewer points, as the earth wire, are led apart, sagging less
std::vector<editor_mode::cable> editor_mode::wiring_cables() const
{
	auto const &state{m_cables};
	std::vector<cable> wires;
	auto const &poles{state.poles};
	if (poles.size() < 2 || std::ranges::any_of(poles, [](editor_mode_wiring_pole const &Pole) { return Pole.sets.empty(); }))
		return wires;
	for (auto const main : {true, false})
	{
		if (false == main && false == state.earth)
			continue;
		// the ends of each span: the set at the pole before, and the one at the pole after
		std::vector<std::pair<std::vector<glm::dvec3>, std::vector<glm::dvec3>>> spans;
		std::size_t count{std::numeric_limits<std::size_t>::max()};
		for (std::size_t span = 0; span + 1 < poles.size(); ++span)
		{
			auto const &from{poles[span]};
			auto const &to{poles[span + 1]};
			auto const *start{set_towards(from, to.origin, main)};
			auto const *end{set_towards(to, from.origin, main)};
			if (start == nullptr || end == nullptr)
			{
				count = 0;
				break;
			}
			spans.emplace_back(across(*start, from.origin, to.origin), across(*end, from.origin, to.origin));
			count = std::min({count, start->size(), end->size()});
		}
		for (std::size_t wire = 0; wire < count; ++wire)
		{
			cable made{state.model};
			double sum{0.0};
			for (std::size_t span = 0; span < spans.size(); ++span)
			{
				auto const &start{spans[span].first[wire]};
				// over a strain pole the jumper, from the set the wires came in at to the one they go out at, hangs a little
				if (made.points.empty())
					made.points.push_back(start);
				else if (glm::distance(made.points.back(), start) > 0.05)
				{
					made.points.push_back(start);
					made.sags.push_back(std::min(0.3f, static_cast<float>(glm::distance(made.points[made.points.size() - 2], start)) * 0.1f));
				}
				// the same span sags the same for all its wires, as it's hung
				auto const random{random_of((static_cast<std::uint64_t>(state.seed) << 32) ^ (span * 0x632be59bd9b4e019ULL) ^ static_cast<std::uint64_t>(std::llround(std::abs(poles[span].origin.x * 10.0 + poles[span].origin.z))))};
				auto sag{static_cast<double>(state.sag_min) + random * static_cast<double>(std::max(0.0f, state.sag_max - state.sag_min))};
				if (false == main)
					sag *= state.earth_share;
				made.points.push_back(spans[span].second[wire]);
				made.sags.push_back(static_cast<float>(sag));
				sum += sag;
			}
			made.sag = static_cast<float>(sum / static_cast<double>(spans.size()));
			wires.push_back(std::move(made));
		}
	}
	return wires;
}

void editor_mode::wiring_insert()
{
	auto &state{m_cables};
	auto wires{wiring_cables()};
	if (wires.empty())
		return;
	int number{1};
	auto const taken = [&state](std::string const &Name) { return std::ranges::any_of(state.cables, [&Name](cable const &Other) { return Other.name == Name; }); };
	for (auto &wire : wires)
	{
		auto name{format("przewod_%d", number)};
		while (taken(name))
			name = format("przewod_%d", ++number);
		wire.name = name;
		state.cables.push_back(std::move(wire));
	}
	state.status = format(STR_C("Wires led: %zu, over %zu poles; they go into the scenery with the next save"), wires.size(), state.poles.size());
	state.poles.clear();
	state.wiring = false;
	cables_deselect();
	state.selected = static_cast<int>(state.cables.size()) - 1;
}

void editor_mode::render_wiring_options()
{
	auto &state{m_cables};
	ImGui::SeparatorText(STR_C("Wiring"));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", STR_C("LMB on the poles one after another (again: out of the list). The templates of the poles tell where the wires hang, in the block for the automatons of the wires (druty.exe); the wires are led along them as the points of each pole go across the line"));
	ImGui::PopTextWrapPos();
	if (ImGui::SmallButton(STR_C("The selected models of the surroundings")))
		for (auto *node : m_selection)
			if (auto const *model{dynamic_cast<TAnimModel const *>(node)}; model != nullptr && std::ranges::none_of(state.poles, [model](editor_mode_wiring_pole const &Pole) { return Pole.model == model || (Pole.instance != 0 && Pole.instance == model->m_instance); }))
				wiring_pick(model);
	item_tooltip("The models selected in the surroundings (F2) taken as the poles, each of them once");
	ImGui::SameLine();
	ImGui::BeginDisabled(state.poles.size() < 3);
	if (ImGui::SmallButton(STR_C("Order along the line")))
		wiring_order();
	ImGui::EndDisabled();
	item_tooltip("From the pole at an end, each to the nearest one left: for poles picked out of order");
	ImGui::SameLine();
	if (ImGui::SmallButton(STR_C("Clear##wiring")))
		state.poles.clear();
	for (std::size_t index = 0; index < state.poles.size(); ++index)
	{
		auto const &pole{state.poles[index]};
		auto const described{false == pole.sets.empty()};
		auto const others{static_cast<std::size_t>(std::ranges::count_if(pole.sets, [&pole](std::vector<glm::dvec3> const &Set) { return Set.size() != pole.main; }))};
		ImGui::TextColored(described ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f), STR_C("%zu. %s  (wires: %zu, sets: %zu, other: %zu)"), index + 1, pole.file.c_str(), pole.main, pole.sets.size(), others);
		if (ImGui::IsItemHovered() && described)
		{
			std::string labels;
			for (std::size_t set = 0; set < pole.sets.size(); ++set)
				labels += format("%zu: %s (%zu)\n", set + 1, pole.labels[set].empty() ? "-" : pole.labels[set].c_str(), pole.sets[set].size());
			ImGui::SetTooltip("%s", labels.c_str());
		}
	}
	if (std::ranges::any_of(state.poles, [](editor_mode_wiring_pole const &Pole) { return Pole.sets.empty(); }))
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", STR_C("A pole of a template without the block: click it again to take it out of the list"));
	if (begin_fields("##wiringsag", 0.45f))
	{
		field_row("Sag from (m)##wiring");
		ImGui::DragFloat("##sagmin", &state.sag_min, 0.05f, 0.0f, 30.0f, "%.2f");
		field_row("Sag to (m)##wiring");
		ImGui::DragFloat("##sagmax", &state.sag_max, 0.05f, 0.0f, 30.0f, "%.2f");
		state.sag_max = std::max(state.sag_max, state.sag_min);
		field_row("Earth wire too##wiring");
		ImGui::Checkbox("##earth", &state.earth);
		item_tooltip("The sets of fewer points the templates have apart, as the earth wire (OPGW), led too where all the poles have one; they sag this part of the span");
		if (state.earth)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##earthshare", &state.earth_share, 0.3f, 1.0f, "%.2f");
		}
		ImGui::EndTable();
	}
	render_cable_model(state.model);
	if (ImGui::Button(STR_C("Pick the sags again")))
		++state.seed;
	item_tooltip("Each span gets another sag between the two above; the same picks give the same sags");
	ImGui::SameLine();
	auto const wires{wiring_cables()};
	ImGui::BeginDisabled(wires.empty());
	if (main_button(format(STR_C("Lead %zu wires"), wires.size()).c_str(), ImVec2(-FLT_MIN, 0.0f)))
		wiring_insert();
	ImGui::EndDisabled();
}

// the template of a pole placed without the editor keeping track of its include: the *.inc named as its model, next to where the
// model is in the scenery folder, else of that name anywhere in it
std::string editor_mode::wiring_template(TAnimModel const &Model)
{
	auto &state{m_cables};
	if (Model.Model() == nullptr)
		return {};
	auto name{ToLower(Model.Model()->NameGet())};
	std::ranges::replace(name, '\\', '/');
	if (auto const models{name.find("models/")}; models != std::string::npos)
		name.erase(0, models + 7);
	if (auto const dot{name.rfind('.')}; dot != std::string::npos && dot > name.rfind('/') + 1)
		name.erase(dot);
	std::error_code error;
	if (std::filesystem::exists(path_of_text(Global.asCurrentSceneryPath + name + ".inc"), error))
		return name + ".inc";
	// the templates of the scenery folder by their names, gathered once
	if (false == state.templates_listed)
	{
		state.templates_listed = true;
		std::filesystem::path const root{path_of_text(Global.asCurrentSceneryPath)};
		for (std::filesystem::recursive_directory_iterator entry{root, std::filesystem::directory_options::skip_permission_denied, error}, end; entry != end; entry.increment(error))
		{
			if (error || false == entry->is_regular_file(error) || ToLower(entry->path().extension().string()) != ".inc")
				continue;
			auto const relative{std::filesystem::relative(entry->path(), root, error).generic_u8string()};
			state.templates.try_emplace(ToLower(entry->path().stem().string()), std::string{relative.begin(), relative.end()});
		}
	}
	auto const stem{name.substr(name.rfind('/') + 1)};
	auto const found{state.templates.find(stem)};
	return found != state.templates.end() ? found->second : std::string{};
}
