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
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

// m, a height changed by less than this isn't a change
float constexpr least_change{0.001f};
// the files read for one mosaic hold at most this many heights: 1.5 GB of them
std::size_t constexpr largest_mosaic{400'000'000};

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

std::vector<std::string> const &model_extensions()
{
	static std::vector<std::string> const extensions{"asc", "xyz", "txt", "csv", "tif", "tiff"};
	return extensions;
}

bool model_file(std::filesystem::path const &Path)
{
	auto extension{text_of(Path.extension())};
	if (false == extension.empty())
		extension.erase(0, 1);
	std::ranges::transform(extension, extension.begin(), [](unsigned char const Character) { return static_cast<char>(std::tolower(Character)); });
	return std::ranges::find(model_extensions(), extension) != model_extensions().end();
}

// the files to read: those chosen, or the one typed, or the files of the model in the folder typed, by their names
std::vector<std::filesystem::path> files_to_read(std::vector<std::filesystem::path> const &Chosen, std::filesystem::path const &Typed)
{
	std::vector<std::filesystem::path> files;
	auto const add = [&files](std::filesystem::path const &Path) {
		std::error_code error;
		if (false == std::filesystem::is_directory(Path, error))
		{
			files.push_back(Path);
			return;
		}
		std::vector<std::filesystem::path> found;
		for (auto const &entry : std::filesystem::directory_iterator{Path, error})
			if (entry.is_regular_file(error) && model_file(entry.path()))
				found.push_back(entry.path());
		std::ranges::sort(found);
		files.insert(files.end(), found.begin(), found.end());
	};
	if (Chosen.empty())
		add(Typed);
	else
		for (auto const &path : Chosen)
			add(path);
	return files;
}

struct read_request
{
	std::vector<std::filesystem::path> files;
	editor_dem::axes axes{editor_dem::axes::automatic};
	glm::dvec2 origin{0.0};
};

// every file of the request, aside of the frame; those which can't be read are told and left out
editor_dem::read_files read_files(read_request const &Request)
{
	editor_dem::read_files result;
	std::vector<std::shared_ptr<editor_dem::grid const>> sheets;
	std::size_t heights{0};
	for (auto const &path : Request.files)
	{
		auto &listed{result.sheets.emplace_back()};
		listed.name = text_of(path.filename());
		auto sheet{std::make_shared<editor_dem::grid>()};
		std::string error;
		if (false == editor_dem::load(path, Request.axes, Request.origin, *sheet, error))
		{
			listed.note = STR(error);
			continue;
		}
		heights += sheet->heights.size();
		if (heights > largest_mosaic)
		{
			result.error = STR("The files hold too many heights to be put in at once: put them in by parts, a few sheets at a time");
			return result;
		}
		listed.read = true;
		listed.note = format(STR_C("%s, %d x %d points of %.2f m"), STR(sheet->format).c_str(), sheet->width, sheet->height, sheet->pixel);
		if (sheet->swapped)
			listed.note += ", " + STR("the coordinates the other way round");
		sheets.push_back(std::move(sheet));
	}
	if (sheets.empty())
	{
		if (result.error.empty())
			result.error = Request.files.empty() ? STR("There are no files of the terrain model there (.asc, .xyz, .txt, .csv, .tif)") : STR("None of the files could be read");
		return result;
	}
	auto mosaic{std::make_shared<editor_dem::mosaic>()};
	mosaic->build(std::move(sheets));
	result.mosaic = std::move(mosaic);
	return result;
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

std::pair<glm::dvec2, glm::dvec2> editor_mode::dem_import_area(editor_dem::mosaic const &Mosaic) const
{
	// the scenery lies on PUWG 1992 by its point (0,0,0): easting = east - x, northing = north + z
	auto const &config{m_orthophoto.settings()};
	glm::dvec2 const low{config.east - Mosaic.high().x, Mosaic.low().y - config.north};
	glm::dvec2 const high{config.east - Mosaic.low().x, Mosaic.high().y - config.north};
	return {low, high};
}

void editor_mode::dem_import_browse(bool const Folder)
{
	auto &state{m_dem_import};
	if (state.dialog.valid() || state.loading.valid())
		return;
	editor_file_dialog::request request;
	request.title = Folder ? STR("A folder of the files of the terrain model") : STR("The files of the terrain model");
	request.folder = Folder;
	request.many = true;
	request.filter_name = STR("Terrain model");
	request.extensions = model_extensions();
	request.owner = ImGui::GetMainViewport()->PlatformHandleRaw;
	// the dialog waits for the user: aside of the frame, so the editor goes on drawing
	state.dialog = std::async(std::launch::async, [request]() { return editor_file_dialog::open(request); });
}

void editor_mode::dem_import_read()
{
	auto &state{m_dem_import};
	if (state.loading.valid())
		return;
	auto const typed{path_of(state.path)};
	state.mosaic.reset();
	state.sheets.clear();
	state.seam.reset();
	state.error.clear();
	state.status.clear();
	if (state.chosen.empty() && typed.empty())
	{
		state.error = STR("Give the path of the file first");
		return;
	}
	auto const &config{m_orthophoto.settings()};
	read_request request;
	request.files = files_to_read(state.chosen, typed);
	request.axes = static_cast<editor_dem::axes>(std::clamp(state.axes, 0, 2));
	request.origin = {config.east, config.north};
	state.file = request.files.size() == 1 ? text_of(request.files.front().filename()) : format("%zu files", request.files.size());
	state.status = format(STR_C("Reading %zu files..."), request.files.size());
	// a sheet of 1 m is a hundred megabytes of text: read aside, the editor doesn't stop for it
	state.loading = std::async(std::launch::async, [request]() { return read_files(request); });
}

void editor_mode::dem_import_poll()
{
	auto &state{m_dem_import};
	// what the dialog chose: the path shows the first of it, the files are read at once
	if (state.dialog.valid() && state.dialog.wait_for(std::chrono::seconds{0}) == std::future_status::ready)
	{
		auto const chosen{state.dialog.get()};
		if (false == chosen.available)
			state.error = STR("The system has no file dialog the editor can open: type the path of the file or of the folder");
		else if (false == chosen.paths.empty())
		{
			state.chosen = chosen.paths;
			auto const shown{text_of(chosen.paths.front())};
			state.path.fill('\0');
			std::copy_n(shown.begin(), std::min(shown.size(), state.path.size() - 1), state.path.begin());
			dem_import_read();
		}
	}
	if (false == state.loading.valid() || state.loading.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
		return;
	auto result{state.loading.get()};
	state.status.clear();
	state.sheets = std::move(result.sheets);
	for (auto const &sheet : state.sheets)
		WriteLog("Editor: the terrain model of " + sheet.name + (sheet.read ? " read: " : " not read: ") + sheet.note, logtype::generic);
	if (result.mosaic == nullptr)
	{
		state.error = result.error;
		return;
	}
	state.mosaic = std::move(result.mosaic);
	state.error = result.error;
	dem_import_measure();
}

void editor_mode::dem_import_measure()
{
	auto &state{m_dem_import};
	state.seam.reset();
	state.seam_points = 0;
	if (state.mosaic == nullptr || false == m_streamer.active())
		return;
	auto const &mosaic{*state.mosaic};
	auto const &config{m_orthophoto.settings()};
	// the chunks under the edges into the memory, so the terrain there is known
	auto const [low, high]{dem_import_area(mosaic)};
	m_streamer.reshape(low, high, [](double, double, float const &) { return false; });
	// along the outer edge of all the files: between them there's no edge to meet
	std::vector<double> differences;
	for (auto const &point : mosaic.seam())
	{
		auto const x{config.east - point.x};
		auto const z{point.y - config.north};
		if (double terrain, model; m_streamer.height_at(x, z, terrain) && mosaic.sample(point.x, point.y, model))
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
	if (state.mosaic == nullptr)
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
	auto const mosaic{state.mosaic};
	auto const &config{m_orthophoto.settings()};
	auto const offset{state.offset + (state.match && state.seam.has_value() ? *state.seam : 0.0)};
	auto const feather{std::max(0.0, state.feather)};
	auto const [low, high]{dem_import_area(*mosaic)};
	// the height of the files at a point of the scenery; beyond them, of their nearest edge
	auto const height_at = [mosaic, &config, offset](double const X, double const Z, double &Height) {
		auto const east{std::clamp(config.east - X, mosaic->low().x, mosaic->high().x)};
		auto const north{std::clamp(config.north + Z, mosaic->low().y, mosaic->high().y)};
		if (false == mosaic->sample(east, north, Height))
			return false;
		Height += offset;
		return true;
	};

	// one step of the history: all the files are taken back at once
	terrain_history_step();
	auto const spacing{heightmap::valid_spacing(static_cast<float>(mosaic->pixel()))};
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
	m_streamer.reshape(low, high, [&mosaic, &config, &height_at, &changed, feather](double const X, double const Z, float &Height) {
		auto const share{editor_dem::blend(mosaic->inside(config.east - X, config.north + Z), feather)};
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
	ImGui::TextDisabled("%s", STR_C("Sheets of the terrain model downloaded from geoportal.gov.pl (.asc, .xyz, .tif in PUWG 1992), one or a folder of them, laid into the terrain there is; their outer edge blends into it"));
	ImGui::PopTextWrapPos();
	auto const busy{state.loading.valid() || state.dialog.valid()};
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(STR_C("Choose files...")))
		dem_import_browse(false);
	item_tooltip("The file dialog of the system: one file or many, read as one terrain");
	ImGui::SameLine();
	if (ImGui::Button(STR_C("Choose a folder...")))
		dem_import_browse(true);
	item_tooltip("All the files of the terrain model in the folder (.asc, .xyz, .txt, .csv, .tif), without its subfolders, read as one terrain");
	ImGui::SetNextItemWidth(-FLT_MIN);
	// typing a path forgets the files chosen in the dialog
	auto const entered{ImGui::InputTextWithHint("##demfile", STR_C("Path of a file or a folder"), state.path.data(), state.path.size(), ImGuiInputTextFlags_EnterReturnsTrue)};
	if (ImGui::IsItemEdited())
		state.chosen.clear();
	item_tooltip("The whole path of a file or of a folder of them, as copied from the file manager");
	if (state.chosen.size() > 1)
		ImGui::TextDisabled(STR_C("%zu files chosen"), state.chosen.size());
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
	if (state.dialog.valid())
		ImGui::TextDisabled("%s", STR_C("The file dialog is open..."));
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
	if (state.sheets.size() > 1 || (state.sheets.size() == 1 && false == state.sheets.front().read))
		render_dem_import_sheets();
	if (state.mosaic != nullptr && false == state.loading.valid())
		render_dem_import_grid();
	ImGui::TreePop();
}

void editor_mode::render_dem_import_sheets() const
{
	auto const &state{m_dem_import};
	auto const read{std::ranges::count_if(state.sheets, [](editor_dem::listed_file const &Sheet) { return Sheet.read; })};
	if (false == ImGui::TreeNode(format(STR_C("Files: %zu read of %zu###demsheets"), static_cast<std::size_t>(read), state.sheets.size()).c_str()))
		return;
	for (auto const &sheet : state.sheets)
	{
		if (sheet.read)
			ImGui::Text("%s: %s", sheet.name.c_str(), sheet.note.c_str());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s: %s", sheet.name.c_str(), sheet.note.c_str());
	}
	ImGui::TreePop();
}

void editor_mode::render_dem_import_grid()
{
	auto &state{m_dem_import};
	auto const &mosaic{*state.mosaic};
	auto const range{mosaic.range()};
	auto const [low, high]{dem_import_area(mosaic)};
	auto const size{mosaic.high() - mosaic.low()};
	ImGui::Separator();
	if (mosaic.grids().size() == 1)
	{
		auto const &grid{*mosaic.grids().front()};
		ImGui::TextUnformatted(state.file.c_str());
		ImGui::Text(STR_C("%s, %d x %d points of %.2f m, %.0f x %.0f m"), STR(grid.format).c_str(), grid.width, grid.height, grid.pixel, grid.width * grid.pixel, grid.height * grid.pixel);
		if (grid.swapped)
			ImGui::TextDisabled("%s", STR_C("The coordinates of the file were read the other way round"));
	}
	else
		ImGui::Text(STR_C("%zu files as one terrain, %.0f x %.0f m, points from %.2f m"), mosaic.grids().size(), size.x, size.y, mosaic.pixel());
	ImGui::Text(STR_C("Heights from %.2f to %.2f m"), range.x, range.y);
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
		state.mosaic.reset();
		state.sheets.clear();
		state.seam.reset();
		state.status.clear();
	}
}
