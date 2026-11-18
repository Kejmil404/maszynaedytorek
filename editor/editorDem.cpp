/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#include "stdafx.h"
#include "editor/editorDem.hpp"

#include "editor/editorGeoTiff.hpp"
#include "utilities/translation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>

namespace editor_dem
{

namespace
{

float constexpr no_data{std::numeric_limits<float>::quiet_NaN()};
// the heights of Poland are far above this; the files mark the points without data with -9999 and the like
float constexpr lowest_height{-1000.f};
// a grid bigger than this isn't made of a file of points: 50 million values are 200 MB
std::size_t constexpr largest_grid{50'000'000};

bool separator(char const Character)
{
	return Character == ' ' || Character == '\t' || Character == '\r' || Character == '\n' || Character == ';' || Character == ',';
}

std::string lowercase(std::string_view const Text)
{
	std::string result{Text};
	std::ranges::transform(result, result.begin(), [](unsigned char const Character) { return static_cast<char>(std::tolower(Character)); });
	return result;
}

std::string text_of(std::filesystem::path const &Path)
{
	auto const text{Path.u8string()};
	return {text.begin(), text.end()};
}

std::optional<double> number(std::string_view const Text)
{
	double value{0.0};
	auto const *const end{Text.data() + Text.size()};
	auto const [last, error]{std::from_chars(Text.data(), end, value)};
	if (error != std::errc{} || last != end || false == std::isfinite(value))
		return std::nullopt;
	return value;
}

// the words of a text, one after another
class words
{
  public:
	explicit words(std::string_view const Text) : m_text{Text} {}

	// the next word; empty at the end of the text
	std::string_view next()
	{
		while (m_at < m_text.size() && separator(m_text[m_at]))
			++m_at;
		auto const start{m_at};
		while (m_at < m_text.size() && false == separator(m_text[m_at]))
			++m_at;
		return m_text.substr(start, m_at - start);
	}

  private:
	std::string_view m_text;
	std::size_t m_at{0};
};

// the corner (west, south) of the grid in the coordinates of the file, and which of them is the easting
struct placement
{
	glm::dvec2 corner{0.0};
	glm::dvec2 size{0.0}; // m, along the first and the second coordinate of the file
};

// whether the coordinates of the file are to be read the other way round
bool swap_of(placement const &Place, axes const Axes, glm::dvec2 const &Origin)
{
	if (Axes != axes::automatic)
		return Axes == axes::swapped;
	if (Origin.x == 0.0 && Origin.y == 0.0)
		return false;
	auto const middle{Place.corner + Place.size * 0.5};
	auto const written{glm::distance(middle, Origin)};
	auto const swapped{glm::distance(glm::dvec2{middle.y, middle.x}, Origin)};
	return swapped < written;
}

void mark_missing(grid &Out)
{
	for (auto &value : Out.heights)
		if (false == std::isfinite(value) || value <= lowest_height)
			value = no_data;
}

// the header of an ESRI ASCII grid
struct asc_header
{
	int columns{0};
	int rows{0};
	double first{0.0};
	double second{0.0};
	bool centre{false}; // the coordinates are of the middle of the corner pixel, not of its outer corner
	double cell{0.0};
	std::optional<double> missing;
	std::string_view data; // the first value of the grid, read after the header
};

bool read_asc_entry(std::string const &Key, double const Value, asc_header &Header)
{
	if (Key == "ncols")
		Header.columns = static_cast<int>(Value);
	else if (Key == "nrows")
		Header.rows = static_cast<int>(Value);
	else if (Key == "xllcorner" || Key == "xllcenter")
	{
		Header.first = Value;
		Header.centre = Key == "xllcenter";
	}
	else if (Key == "yllcorner" || Key == "yllcenter")
		Header.second = Value;
	else if (Key == "cellsize")
		Header.cell = Value;
	else if (Key == "nodata_value")
		Header.missing = Value;
	else
		return false;
	return true;
}

bool read_asc_header(words &Words, asc_header &Header, std::string &Error)
{
	auto word{Words.next()};
	while (false == word.empty() && std::isalpha(static_cast<unsigned char>(word.front())))
	{
		auto const key{lowercase(word)};
		auto const value{number(Words.next())};
		if (false == value.has_value() || false == read_asc_entry(key, *value, Header))
		{
			Error = STRN("The header of the ASCII grid can't be read");
			return false;
		}
		word = Words.next();
	}
	Header.data = word;
	if (Header.columns < 2 || Header.rows < 2 || Header.cell <= 0.0)
	{
		Error = STRN("The ASCII grid has no ncols, nrows or cellsize");
		return false;
	}
	if (static_cast<std::size_t>(Header.columns) * static_cast<std::size_t>(Header.rows) > largest_grid)
	{
		Error = STRN("The grid of the file is too big");
		return false;
	}
	return true;
}

bool load_asc(std::string_view const Text, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error)
{
	words reader{Text};
	asc_header header;
	if (false == read_asc_header(reader, header, Error))
		return false;
	auto const half{header.centre ? header.cell * 0.5 : 0.0};
	placement const place{{header.first - half, header.second - half}, {header.columns * header.cell, header.rows * header.cell}};
	Out.swapped = swap_of(place, Axes, Origin);
	auto const corner{Out.swapped ? glm::dvec2{place.corner.y, place.corner.x} : place.corner};
	Out.pixel = header.cell;
	Out.width = header.columns;
	Out.height = header.rows;
	Out.east = corner.x;
	Out.north = corner.y + header.rows * header.cell;
	Out.heights.assign(static_cast<std::size_t>(header.columns) * header.rows, no_data);
	auto word{header.data};
	for (auto &value : Out.heights)
	{
		auto const read{number(word)};
		if (false == read.has_value())
		{
			Error = word.empty() ? STRN("The ASCII grid ends before its last value") : STRN("A value of the ASCII grid can't be read");
			return false;
		}
		if (false == header.missing.has_value() || std::abs(*read - *header.missing) > 1e-6)
			value = static_cast<float>(*read);
		word = reader.next();
	}
	mark_missing(Out);
	Out.format = STRN("ASCII grid");
	return true;
}

struct point
{
	double first{0.0};
	double second{0.0};
	float height{0.f};
};

// the three numbers of a line of a file of points; false for a line of something else, as a header
bool read_point(std::string_view const Line, point &Out)
{
	words reader{Line};
	std::array<double, 3> values{};
	for (auto &value : values)
	{
		auto const read{number(reader.next())};
		if (false == read.has_value())
			return false;
		value = *read;
	}
	Out = {values[0], values[1], static_cast<float>(values[2])};
	return true;
}

std::vector<point> read_points(std::string_view const Text)
{
	std::vector<point> points;
	std::size_t at{0};
	while (at < Text.size())
	{
		auto end{Text.find('\n', at)};
		if (end == std::string_view::npos)
			end = Text.size();
		if (point read; read_point(Text.substr(at, end - at), read))
			points.push_back(read);
		at = end + 1;
	}
	return points;
}

// the step of the grid along one coordinate: the most common distance between its neighbouring values
double step_of(std::vector<double> Values)
{
	std::ranges::sort(Values);
	auto const [first, last]{std::ranges::unique(Values)};
	Values.erase(first, last);
	std::vector<double> steps;
	steps.reserve(Values.size());
	for (std::size_t i = 1; i < Values.size(); ++i)
		steps.push_back(std::round((Values[i] - Values[i - 1]) * 1000.0) / 1000.0);
	if (steps.empty())
		return 0.0;
	std::ranges::sort(steps);
	double best{steps.front()};
	std::size_t best_count{0};
	std::size_t count{0};
	for (std::size_t i = 0; i < steps.size(); ++i)
	{
		count = i > 0 && steps[i] == steps[i - 1] ? count + 1 : 1;
		if (count > best_count)
		{
			best_count = count;
			best = steps[i];
		}
	}
	return best;
}

bool load_xyz(std::string_view const Text, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error)
{
	auto points{read_points(Text)};
	if (points.size() < 4)
	{
		Error = STRN("The file has no points of three numbers: x, y and the height");
		return false;
	}
	std::vector<double> firsts;
	std::vector<double> seconds;
	firsts.reserve(points.size());
	seconds.reserve(points.size());
	for (auto const &value : points)
	{
		firsts.push_back(value.first);
		seconds.push_back(value.second);
	}
	auto const [low_first, high_first]{std::ranges::minmax(firsts)};
	auto const [low_second, high_second]{std::ranges::minmax(seconds)};
	auto const pixel{step_of(firsts)};
	if (pixel <= 0.0 || std::abs(step_of(seconds) - pixel) > pixel * 0.01)
	{
		Error = STRN("The points of the file don't lie on a grid of square cells");
		return false;
	}
	placement const place{{low_first, low_second}, {high_first - low_first, high_second - low_second}};
	Out.swapped = swap_of(place, Axes, Origin);
	if (Out.swapped)
		for (auto &value : points)
			std::swap(value.first, value.second);
	auto const west{Out.swapped ? low_second : low_first};
	auto const east{Out.swapped ? high_second : high_first};
	auto const south{Out.swapped ? low_first : low_second};
	auto const north{Out.swapped ? high_first : high_second};
	auto const width{static_cast<std::size_t>(std::lround((east - west) / pixel)) + 1};
	auto const height{static_cast<std::size_t>(std::lround((north - south) / pixel)) + 1};
	if (width < 2 || height < 2 || width * height > largest_grid)
	{
		Error = STRN("The grid of the file is too big");
		return false;
	}
	// the points are the middles of the pixels
	Out.pixel = pixel;
	Out.width = static_cast<int>(width);
	Out.height = static_cast<int>(height);
	Out.east = west - pixel * 0.5;
	Out.north = north + pixel * 0.5;
	Out.heights.assign(width * height, no_data);
	for (auto const &value : points)
	{
		auto const column{static_cast<std::size_t>(std::lround((value.first - west) / pixel))};
		auto const row{static_cast<std::size_t>(std::lround((north - value.second) / pixel))};
		Out.heights[row * width + column] = value.height;
	}
	mark_missing(Out);
	Out.format = STRN("grid of points");
	return true;
}

} // namespace

bool grid::sample(double const East, double const North, double &Height) const
{
	if (false == valid() || East < west_edge() || East > east_edge() || North > north_edge() || North < south_edge())
		return false;
	// the values lie in the middles of the pixels
	auto const column{(East - east) / pixel - 0.5};
	auto const row{(north - North) / pixel - 0.5};
	auto const c0{std::clamp(static_cast<int>(std::floor(column)), 0, width - 2)};
	auto const r0{std::clamp(static_cast<int>(std::floor(row)), 0, height - 2)};
	auto const fc{std::clamp(column - c0, 0.0, 1.0)};
	auto const fr{std::clamp(row - r0, 0.0, 1.0)};
	auto const at = [this](int const C, int const R) { return heights[static_cast<std::size_t>(R) * width + C]; };
	std::array const corners{at(c0, r0), at(c0 + 1, r0), at(c0, r0 + 1), at(c0 + 1, r0 + 1)};
	std::array const weights{(1.0 - fc) * (1.0 - fr), fc * (1.0 - fr), (1.0 - fc) * fr, fc * fr};
	// by the values there are: at the edge of the data, and of a hole in it, the heights go on as the nearest ones are
	double sum{0.0};
	double weight{0.0};
	for (std::size_t i = 0; i < corners.size(); ++i)
	{
		if (false == std::isfinite(corners[i]))
			continue;
		sum += corners[i] * weights[i];
		weight += weights[i];
	}
	if (weight > 1e-9)
	{
		Height = sum / weight;
		return true;
	}
	auto const nearest{corners[(fr > 0.5 ? 2 : 0) + (fc > 0.5 ? 1 : 0)]};
	if (false == std::isfinite(nearest))
		return false;
	Height = nearest;
	return true;
}

double grid::inside(double const East, double const North) const
{
	return std::min({East - west_edge(), east_edge() - East, north_edge() - North, North - south_edge()});
}

std::size_t grid::known() const
{
	return static_cast<std::size_t>(std::ranges::count_if(heights, [](float const Value) { return std::isfinite(Value); }));
}

glm::dvec2 grid::range() const
{
	glm::dvec2 result{std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()};
	for (auto const value : heights)
	{
		if (false == std::isfinite(value))
			continue;
		result.x = std::min<double>(result.x, value);
		result.y = std::max<double>(result.y, value);
	}
	return result;
}

bool load_geotiff(std::vector<std::uint8_t> const &Data, grid &Out, std::string &Error)
{
	if (false == editor_geotiff::read(Data, Out, no_data))
	{
		Error = STRN("The GeoTIFF isn't one band of 32-bit floats, not compressed, of at most 8192 pixels a side");
		return false;
	}
	Out.swapped = false;
	mark_missing(Out);
	Out.format = "GeoTIFF";
	return true;
}

bool load_text(std::string_view const Text, std::string_view const Name, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error)
{
	Out = {};
	auto const extension{lowercase(text_of(std::filesystem::path{std::u8string{Name.begin(), Name.end()}}.extension()))};
	auto const loaded{extension == ".asc" ? load_asc(Text, Axes, Origin, Out, Error) : load_xyz(Text, Axes, Origin, Out, Error)};
	if (loaded && Out.known() == 0)
	{
		Error = STRN("The file has no heights, only points without data");
		return false;
	}
	return loaded;
}

bool load(std::filesystem::path const &Path, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error)
{
	std::ifstream file{Path, std::ios::binary};
	if (false == file.is_open())
	{
		Error = STRN("The file can't be opened");
		return false;
	}
	std::vector<std::uint8_t> data{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
	auto const extension{lowercase(text_of(Path.extension()))};
	if (extension == ".tif" || extension == ".tiff")
	{
		Out = {};
		if (false == load_geotiff(data, Out, Error))
			return false;
		if (Out.known() == 0)
		{
			Error = STRN("The file has no heights, only points without data");
			return false;
		}
		return true;
	}
	std::string_view const text{reinterpret_cast<char const *>(data.data()), data.size()}; // NOSONAR: the bytes of the file read as its text
	return load_text(text, text_of(Path.filename()), Axes, Origin, Out, Error);
}

namespace
{
// the cells of the map of the distances: at most this many a side, so a folder of sheets keeps it to a few megabytes
int constexpr largest_side{2048};

// the distances of the cells with data to the nearest one without, both ways over the map (chamfer 1, sqrt 2); beyond the map
// there's no data
void chamfer(std::vector<float> &Distance, int const Columns, int const Rows, float const Cell)
{
	auto const diagonal{Cell * std::numbers::sqrt2_v<float>};
	auto const at = [&Distance, Columns, Rows](int const C, int const R) { return C < 0 || R < 0 || C >= Columns || R >= Rows ? 0.0f : Distance[static_cast<std::size_t>(R) * Columns + C]; };
	auto const relax = [&Distance, &at, Columns, Cell, diagonal](int const C, int const R, int const Step) {
		auto &value{Distance[static_cast<std::size_t>(R) * Columns + C]};
		if (value <= 0.0f)
			return;
		value = std::min({value, at(C - Step, R) + Cell, at(C, R - Step) + Cell, at(C - Step, R - Step) + diagonal, at(C + Step, R - Step) + diagonal});
	};
	for (int r = 0; r < Rows; ++r)
		for (int c = 0; c < Columns; ++c)
			relax(c, r, 1);
	for (int r = Rows - 1; r >= 0; --r)
		for (int c = Columns - 1; c >= 0; --c)
			relax(c, r, -1);
}
} // namespace

void mosaic::build(std::vector<std::shared_ptr<grid const>> Grids)
{
	std::erase_if(Grids, [](std::shared_ptr<grid const> const &Grid) { return Grid == nullptr || false == Grid->valid(); });
	m_grids = std::move(Grids);
	m_distance.clear();
	m_columns = 0;
	m_rows = 0;
	if (m_grids.empty())
		return;
	m_low = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
	m_high = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
	for (auto const &sheet : m_grids)
	{
		m_low = glm::min(m_low, glm::dvec2{sheet->west_edge(), sheet->south_edge()});
		m_high = glm::max(m_high, glm::dvec2{sheet->east_edge(), sheet->north_edge()});
	}
	auto const size{m_high - m_low};
	m_cell = std::max({1.0, pixel(), std::max(size.x, size.y) / largest_side});
	m_columns = std::max(1, static_cast<int>(std::ceil(size.x / m_cell)));
	m_rows = std::max(1, static_cast<int>(std::ceil(size.y / m_cell)));
	// a cell with data: the distance to be found, without: 0
	m_distance.assign(static_cast<std::size_t>(m_columns) * m_rows, 0.0f);
	for (int r = 0; r < m_rows; ++r)
		for (int c = 0; c < m_columns; ++c)
		{
			double height{0.0};
			if (sample(m_low.x + (c + 0.5) * m_cell, m_high.y - (r + 0.5) * m_cell, height))
				m_distance[static_cast<std::size_t>(r) * m_columns + c] = std::numeric_limits<float>::max();
		}
	chamfer(m_distance, m_columns, m_rows, static_cast<float>(m_cell));
}

bool mosaic::sample(double const East, double const North, double &Height) const
{
	return std::ranges::any_of(m_grids | std::views::reverse, [East, North, &Height](std::shared_ptr<grid const> const &Sheet) { return Sheet->sample(East, North, Height); });
}

float mosaic::distance_at(int const Column, int const Row) const
{
	if (Column < 0 || Row < 0 || Column >= m_columns || Row >= m_rows)
		return 0.0f;
	return m_distance[static_cast<std::size_t>(Row) * m_columns + Column];
}

double mosaic::inside(double const East, double const North) const
{
	if (m_distance.empty())
		return 0.0;
	// between the middles of the cells; the distance of a cell is to the middle of the nearest one without data, half a cell
	// beyond the edge
	auto const column{(East - m_low.x) / m_cell - 0.5};
	auto const row{(m_high.y - North) / m_cell - 0.5};
	auto const c0{static_cast<int>(std::floor(column))};
	auto const r0{static_cast<int>(std::floor(row))};
	auto const fc{column - c0};
	auto const fr{row - r0};
	auto const top{std::lerp(static_cast<double>(distance_at(c0, r0)), static_cast<double>(distance_at(c0 + 1, r0)), fc)};
	auto const bottom{std::lerp(static_cast<double>(distance_at(c0, r0 + 1)), static_cast<double>(distance_at(c0 + 1, r0 + 1)), fc)};
	return std::max(0.0, std::lerp(top, bottom, fr) - 0.5 * m_cell);
}

std::vector<glm::dvec2> mosaic::seam() const
{
	std::vector<glm::dvec2> points;
	for (int r = 0; r < m_rows; ++r)
		for (int c = 0; c < m_columns; ++c)
		{
			// a cell with data beside one without
			if (distance_at(c, r) <= 0.0f || (distance_at(c - 1, r) > 0.0f && distance_at(c + 1, r) > 0.0f && distance_at(c, r - 1) > 0.0f && distance_at(c, r + 1) > 0.0f))
				continue;
			points.emplace_back(m_low.x + (c + 0.5) * m_cell, m_high.y - (r + 0.5) * m_cell);
		}
	// a few thousand are enough for the median
	if (points.size() > 4000)
	{
		auto const step{points.size() / 4000 + 1};
		std::vector<glm::dvec2> fewer;
		for (std::size_t i = 0; i < points.size(); i += step)
			fewer.push_back(points[i]);
		points = std::move(fewer);
	}
	return points;
}

double mosaic::pixel() const
{
	double result{std::numeric_limits<double>::max()};
	for (auto const &sheet : m_grids)
		result = std::min(result, sheet->pixel);
	return m_grids.empty() ? 1.0 : result;
}

glm::dvec2 mosaic::range() const
{
	glm::dvec2 result{std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()};
	for (auto const &sheet : m_grids)
	{
		auto const sheetrange{sheet->range()};
		result = {std::min(result.x, sheetrange.x), std::max(result.y, sheetrange.y)};
	}
	return result;
}

std::size_t mosaic::points() const
{
	std::size_t result{0};
	for (auto const &sheet : m_grids)
		result += sheet->known();
	return result;
}

double blend(double const Inside, double const Feather)
{
	if (Inside < 0.0)
		return 0.0;
	if (Feather <= 0.0)
		return 1.0;
	auto const share{std::clamp(Inside / Feather, 0.0, 1.0)};
	return share * share * (3.0 - 2.0 * share);
}

} // namespace editor_dem
