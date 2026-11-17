/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

// the terrain model of a file of the user (.asc, .xyz, .tif from geoportal.gov.pl and the like) put into the terrain of the
// scenery: a sheet laid into the terrain there is, its edges blended into it. only the heights of the chunks of the heightmap
// change, as with the brushes, so the scenery is read by every build of the simulator as before; the file isn't kept with it

#include "stdafx.h"
#include "application/editormode.h"
#include "application/editormodeshared.h"
#include "editor/editorFormat.hpp"
#include "utilities/Globals.h"
#include "utilities/Logs.h"
#include "utilities/translation.h"

#include "imgui/imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

// m, between the points along the edges of the file the terrain is compared with it at
double constexpr seam_step{5.0};
// m, a height changed by less than this isn't a change
float constexpr least_change{0.001f};

std::filesystem::path path_of(std::array<char, 1024> const &Text)
{
	std::string text{Text.data()};
	// a path copied from the file manager may come in quotes
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
		text = text.substr(1, text.size() - 2);
	// the text typed is UTF-8 on every system
	return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

std::string text_of(std::filesystem::path const &Path)
{
	auto const text{Path.u8string()};
	return {text.begin(), text.end()};
}

double median(std::vector<double> Values)
{
	if (Values.empty())
		return 0.0;
	auto const middle{Values.begin() + static_cast<std::ptrdiff_t>(Values.size() / 2)};
	std::ranges::nth_element(Values, middle);
	return *middle;
}

// the points along the edges of the grid, a pixel inside them, in PUWG 1992 (easting, northing)
std::vector<glm::dvec2> seam_of(editor_dem::grid const &Grid)
{
	std::vector<glm::dvec2> points;
	auto const inset{Grid.pixel};
	glm::dvec2 const low{Grid.west_edge() + inset, Grid.south_edge() + inset};
	glm::dvec2 const high{Grid.east_edge() - inset, Grid.north_edge() - inset};
	if (high.x <= low.x || high.y <= low.y)
		return points;
	auto const along = [&points](glm::dvec2 const &From, glm::dvec2 const &To) {
		auto const steps{std::max(1, static_cast<int>(std::ceil(glm::distance(From, To) / seam_step)))};
		for (int i = 0; i < steps; ++i)
			points.push_back(glm::mix(From, To, static_cast<double>(i) / steps));
	};
	along(low, {high.x, low.y});
	along({high.x, low.y}, high);
	along(high, {low.x, high.y});
	along({low.x, high.y}, low);
	return points;
}

char const *axes_name(int const Axes)
{
	switch (Axes)
	{
	case 1:
		return STRN("As written: easting, northing");
	case 2:
		return STRN("Swapped: northing (X), easting (Y)");
	default:
		return STRN("By the place of the scenery");
	}
}

} // namespace

std::pair<glm::dvec2, glm::dvec2> editor_mode::dem_import_area(editor_dem::grid const &Grid) const
{
	// the scenery lies on PUWG 1992 by its point (0,0,0): easting = east - x, northing = north + z
	auto const &config{m_orthophoto.settings()};
	glm::dvec2 const low{config.east - Grid.east_edge(), Grid.south_edge() - config.north};
	glm::dvec2 const high{config.east - Grid.west_edge(), Grid.north_edge() - config.north};
	return {low, high};
}

void editor_mode::dem_import_read()
{
	auto &state{m_dem_import};
	if (state.loading.valid())
		return;
	auto const path{path_of(state.path)};
	state.grid.reset();
	state.seam.reset();
	state.error.clear();
	state.status.clear();
	if (path.empty())
	{
		state.error = STR("Give the path of the file first");
		return;
	}
	auto const &config{m_orthophoto.settings()};
	glm::dvec2 const origin{config.east, config.north};
	auto const axes{static_cast<editor_dem::axes>(std::clamp(state.axes, 0, 2))};
	state.file = text_of(path.filename());
	state.status = STR("Reading the file...");
	// a sheet of 1 m is a hundred megabytes of text: read aside, the editor doesn't stop for it
	state.loading = std::async(std::launch::async, [path, axes, origin]() {
		dem_import_state::loaded result;
		auto grid{std::make_shared<editor_dem::grid>()};
		if (editor_dem::load(path, axes, origin, *grid, result.error))
			result.grid = std::move(grid);
		return result;
	});
}

void editor_mode::dem_import_poll()
{
	auto &state{m_dem_import};
	if (false == state.loading.valid() || state.loading.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
		return;
	auto result{state.loading.get()};
	state.status.clear();
	if (result.grid == nullptr)
	{
		state.error = STR(result.error);
		WriteLog("Editor: the terrain model of " + state.file + " not read: " + result.error, logtype::generic);
		return;
	}
	state.grid = std::move(result.grid);
	WriteLog(format("Editor: the terrain model of %s read, %s of %dx%d pixels of %.2f m%s", state.file.c_str(), state.grid->format.c_str(), state.grid->width, state.grid->height, state.grid->pixel,
	                state.grid->swapped ? ", the coordinates swapped" : ""),
	         logtype::generic);
	dem_import_measure();
}

void editor_mode::dem_import_measure()
{
	auto &state{m_dem_import};
	state.seam.reset();
	state.seam_points = 0;
	if (state.grid == nullptr || false == m_streamer.active())
		return;
	auto const &grid{*state.grid};
	auto const &config{m_orthophoto.settings()};
	// the chunks under the edges into the memory, so the terrain there is known
	auto const [low, high]{dem_import_area(grid)};
	m_streamer.reshape(low, high, [](double, double, float const &) { return false; });
	std::vector<double> differences;
	for (auto const &point : seam_of(grid))
	{
		auto const x{config.east - point.x};
		auto const z{point.y - config.north};
		if (double terrain, model; m_streamer.height_at(x, z, terrain) && grid.sample(point.x, point.y, model))
			differences.push_back(terrain - model);
	}
	state.seam_points = differences.size();
	if (false == differences.empty())
		state.seam = median(std::move(differences));
}

void editor_mode::dem_import_apply()
{
	auto &state{m_dem_import};
	state.error.clear();
	if (state.grid == nullptr)
		return;
	if (nmt_busy())
	{
		state.error = STR("Wait until the terrain model of the service is in");
		return;
	}
	if (false == ensure_terrain())
	{
		state.error = STR("The heightmap terrain can't be opened");
		return;
	}
	nmt_locate();
	dem_import_measure();
	auto const grid{state.grid};
	auto const &config{m_orthophoto.settings()};
	auto const offset{state.offset + (state.match && state.seam.has_value() ? *state.seam : 0.0)};
	auto const feather{std::max(0.0, state.feather)};
	auto const [low, high]{dem_import_area(*grid)};
	// the height of the file at a point of the scenery; beyond the file, of its nearest edge
	auto const height_at = [grid, &config, offset](double const X, double const Z, double &Height) {
		auto const east{std::clamp(config.east - X, grid->west_edge(), grid->east_edge())};
		auto const north{std::clamp(config.north + Z, grid->south_edge(), grid->north_edge())};
		if (false == grid->sample(east, north, Height))
			return false;
		Height += offset;
		return true;
	};

	// one step of the history: the whole file is taken back at once
	terrain_history_step();
	auto const spacing{heightmap::valid_spacing(static_cast<float>(grid->pixel))};
	auto const first{heightmap::chunk_at(low.x, low.y)};
	auto const last{heightmap::chunk_at(high.x, high.y)};
	std::size_t made{0};
	std::size_t finer{0};
	for (int cz = first.second; cz <= last.second; ++cz)
		for (int cx = first.first; cx <= last.first; ++cx)
		{
			heightmap::chunk_key const key{cx, cz};
			if (m_streamer.exists(key))
			{
				finer += dem_import_spacing(key, spacing) ? 1 : 0;
				continue;
			}
			if (false == state.create)
				continue;
			auto const made_spacing{state.finer ? std::min(m_terrain_spacing, spacing) : m_terrain_spacing};
			auto const made_height = [&height_at, this](double const X, double const Z) {
				double value{m_terrain_baseheight};
				height_at(X, Z, value);
				return static_cast<float>(value);
			};
			if (m_streamer.add_chunk(cx, cz, made_spacing, made_height))
				++made;
		}

	// the ground under the formations of the tracks changes, the formations stay as they are, as with the service
	auto const touchup{m_streamer.touch_up()};
	m_streamer.touch_up(false);
	std::size_t changed{0};
	m_streamer.reshape(low, high, [&grid, &config, &height_at, &changed, feather](double const X, double const Z, float &Height) {
		auto const share{editor_dem::blend(grid->inside(config.east - X, config.north + Z), feather)};
		double model{0.0};
		if (share <= 0.0 || false == height_at(X, Z, model))
			return false;
		auto const target{static_cast<float>(std::lerp(static_cast<double>(Height), model, share))};
		if (std::abs(target - Height) < least_change)
			return false;
		Height = target;
		++changed;
		return true;
	});
	m_streamer.touch_up(touchup);
	forget_ground_in(low, high);
	terrain_history_step();

	state.status = format(STR_C("%zu points of the terrain changed, %zu chunks made, %zu made finer; the heights of the file moved by %+.2f m"), changed, made, finer, offset);
	WriteLog(format("Editor: the terrain model of %s put into the terrain: %zu points changed, %zu chunks made, %zu made finer, moved by %+.2f m, edges blended over %.0f m", state.file.c_str(), changed, made, finer,
	                offset, feather),
	         logtype::generic);
}

bool editor_mode::dem_import_spacing(heightmap::chunk_key const &Key, float const Spacing)
{
	if (false == m_dem_import.finer)
		return false;
	auto const middle{heightmap::chunk_corner(Key) + glm::dvec2{0.5 * heightmap::chunk_size}};
	// the chunk into the memory first, to know its spacing
	m_streamer.reshape(middle, middle, [](double, double, float const &) { return false; });
	auto const *terrain{m_streamer.terrain_at(middle.x, middle.y)};
	if (terrain == nullptr || terrain->key() != Key || terrain->data().spacing() <= Spacing + 1e-3)
		return false;
	return m_streamer.spacing(Key.first, Key.second, Spacing);
}

void editor_mode::render_dem_import_ui()
{
	auto &state{m_dem_import};
	dem_import_poll();
	if (false == ImGui::TreeNode(STR_C("Terrain model from a file")))
		return;
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", STR_C("A sheet of the terrain model downloaded from geoportal.gov.pl (.asc, .xyz, .tif in PUWG 1992) laid into the terrain there is; its edges blend into it"));
	ImGui::PopTextWrapPos();
	auto const reading{state.loading.valid()};
	ImGui::BeginDisabled(reading);
	ImGui::SetNextItemWidth(-FLT_MIN);
	auto const entered{ImGui::InputTextWithHint("##demfile", STR_C("Path of the file"), state.path.data(), state.path.size(), ImGuiInputTextFlags_EnterReturnsTrue)};
	item_tooltip("The whole path of the file, as copied from the file manager");
	if (begin_fields("##demread", 0.5f))
	{
		field_row("Coordinates of the file");
		if (ImGui::BeginCombo("##demaxes", STR_C(axes_name(state.axes))))
		{
			for (int axes = 0; axes <= 2; ++axes)
				if (ImGui::Selectable(STR_C(axes_name(axes)), state.axes == axes))
					state.axes = axes;
			ImGui::EndCombo();
		}
		item_tooltip("The Polish maps name the northing X, so some files give it first; by the place of the scenery the one is taken which lies nearer it");
		ImGui::EndTable();
	}
	if (ImGui::Button(STR_C("Read the file"), ImVec2(-FLT_MIN, 0.0f)) || entered)
		dem_import_read();
	ImGui::EndDisabled();
	if (false == state.status.empty())
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", state.status.c_str());
		ImGui::PopTextWrapPos();
	}
	if (false == state.error.empty())
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", state.error.c_str());
		ImGui::PopTextWrapPos();
	}
	if (state.grid != nullptr && false == reading)
		render_dem_import_grid();
	ImGui::TreePop();
}

void editor_mode::render_dem_import_grid()
{
	auto &state{m_dem_import};
	auto const &grid{*state.grid};
	auto const range{grid.range()};
	auto const [low, high]{dem_import_area(grid)};
	ImGui::Separator();
	ImGui::TextUnformatted(state.file.c_str());
	ImGui::Text(STR_C("%s, %d x %d points of %.2f m, %.0f x %.0f m"), STR(grid.format).c_str(), grid.width, grid.height, grid.pixel, grid.width * grid.pixel, grid.height * grid.pixel);
	ImGui::Text(STR_C("Heights from %.2f to %.2f m"), range.x, range.y);
	if (grid.swapped)
		ImGui::TextDisabled("%s", STR_C("The coordinates of the file were read the other way round"));
	ImGui::Text(STR_C("In the scenery: x from %.0f to %.0f, z from %.0f to %.0f"), low.x, high.x, low.y, high.y);
	auto const near_scenery{std::abs(low.x) < 100'000.0 && std::abs(low.y) < 100'000.0};
	if (false == near_scenery)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", STR_C("The file lies more than 100 km from the scenery: check its location and the coordinates of the file"));
		ImGui::PopTextWrapPos();
	}
	if (state.seam.has_value())
		ImGui::Text(STR_C("Along its edges the terrain is %+.2f m over the file (%zu points)"), *state.seam, state.seam_points);
	else
		ImGui::TextDisabled("%s", STR_C("There's no terrain along the edges of the file to compare it with"));

	if (begin_fields("##demput", 0.5f))
	{
		field_row("Edges blended over (m)");
		ImGui::InputDouble("##demfeather", &state.feather, 5.0, 25.0, "%.0f");
		item_tooltip("Within this band along the edges of the file its heights go over smoothly into the terrain there is; 0: the file as it is up to its edge");
		field_row("Height offset of the file (m)");
		ImGui::InputDouble("##demoffset", &state.offset, 0.1, 1.0, "%.2f");
		item_tooltip("Added to the heights of the file, as between the height systems Kronsztadt 86 and EVRF2007, or for a scenery built at heights of its own");
		ImGui::EndTable();
	}
	state.feather = std::clamp(state.feather, 0.0, 500.0);
	toggle_chips("demoptions", {{STR_C("Meet the terrain along the edges"), &state.match, "The whole file moved up or down by how much the terrain along its edges is higher, so there's no step there"},
	                            {STR_C("Point spacing of the file"), &state.finer, "The chunks under the file get its point spacing where theirs is coarser, so its detail isn't lost"},
	                            {STR_C("Chunks where there are none"), &state.create, "Where the file reaches beyond the terrain, chunks are made of it; off, only the chunks there are change"}});
	ImGui::BeginDisabled(nmt_busy() || false == near_scenery);
	if (main_button(STR_C("Put into the terrain"), ImVec2(-FLT_MIN, tall_button_height())))
		dem_import_apply();
	ImGui::EndDisabled();
	item_tooltip("The heights of the chunks under the file change, in one step of the history; the tracks keep their formations");
	if (ImGui::SmallButton(STR_C("Compare again")))
		dem_import_measure();
	ImGui::SameLine();
	if (ImGui::SmallButton(STR_C("Forget the file")))
	{
		state.grid.reset();
		state.seam.reset();
		state.status.clear();
	}
}
