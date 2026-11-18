/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#pragma once

#include <cstdint>
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// a terrain model read from a file of the user: an ESRI ASCII grid (.asc), a grid of points (.xyz, .txt, .csv) or a GeoTIFF of
// 32-bit floats (.tif), as geoportal.gov.pl gives them for download. the grid lies in PUWG 1992 (EPSG:2180), the heights are
// those of the file. it's only read by the editor: the terrain made of it is the heightmap of the scenery, as from the service
namespace editor_dem
{

// how the two coordinates of the file are to be read. the Polish maps name the northing X and the easting Y, so some files
// give them the other way round than the formats say
enum class axes
{
	automatic, // the one which puts the file nearer the place of the scenery
	as_written, // the first one is the easting
	swapped // the first one is the northing
};

struct grid
{
	double east{0.0}; // of the outer edge of the first column, the westernmost
	double north{0.0}; // of the outer edge of the first row, the northernmost
	double pixel{1.0};
	int width{0};
	int height{0};
	std::vector<float> heights; // rows from the north, columns from the west; not finite where the file has no data
	bool swapped{false}; // the coordinates of the file were read the other way round
	std::string format; // what the file was read as, for the messages

	bool valid() const { return width >= 2 && height >= 2 && pixel > 0.0 && heights.size() == static_cast<std::size_t>(width) * height; }
	double west_edge() const { return east; }
	double east_edge() const { return east + width * pixel; }
	double north_edge() const { return north; }
	double south_edge() const { return north - height * pixel; }
	// the height at the point of PUWG 1992, between the values in the middles of the pixels; at the edge of the data the nearest
	// value. false outside of the grid and where the file has no data
	bool sample(double East, double North, double &Height) const;
	// how far the point lies inside the rectangle of the grid, m; 0 or less: outside of it
	double inside(double East, double North) const;
	// the values with data, and the lowest and highest of them
	std::size_t known() const;
	glm::dvec2 range() const;
};

// sheets of the model put in together, as a folder of them: one terrain of all of them. where they overlap, the later one is
// taken. its edge is the outer edge of all of them, where the terrain there is meets it; between the sheets there's no edge
class mosaic
{
  public:
	void build(std::vector<std::shared_ptr<grid const>> Grids);
	bool empty() const { return m_grids.empty(); }
	std::vector<std::shared_ptr<grid const>> const &grids() const { return m_grids; }
	// the height at the point of PUWG 1992 of the last sheet which has it
	bool sample(double East, double North, double &Height) const;
	// how far the point lies inside the outer edge of the sheets, m, the holes of the data counted as outside; 0 outside of them
	double inside(double East, double North) const;
	// the points just inside the outer edge, in PUWG 1992 (easting, northing)
	std::vector<glm::dvec2> seam() const;
	// the rectangle of all the sheets: west and south, east and north
	glm::dvec2 const &low() const { return m_low; }
	glm::dvec2 const &high() const { return m_high; }
	// the finest point spacing of the sheets
	double pixel() const;
	glm::dvec2 range() const;
	std::size_t points() const;

  private:
	float distance_at(int Column, int Row) const;

	std::vector<std::shared_ptr<grid const>> m_grids;
	glm::dvec2 m_low{0.0};
	glm::dvec2 m_high{0.0};
	double m_cell{1.0}; // m, of the map of the distances to the edge
	int m_columns{0};
	int m_rows{0};
	std::vector<float> m_distance; // m, from the middle of a cell to the nearest one without data; rows from the north
};

// a file of those read for a mosaic, as the editor lists it
struct listed_file
{
	std::string name;
	std::string note; // what it was read as, or why it wasn't
	bool read{false};
};
// the files read for a mosaic: the mosaic of those which could be read, and what became of each
struct read_files
{
	std::shared_ptr<editor_dem::mosaic const> mosaic; // qualified: the member takes the name of the type
	std::vector<listed_file> sheets;
	std::string error;
};

// the grid of the file, its coordinates read as Axes says; Origin (easting, northing) is the place of the scenery the automatic
// choice goes by, (0, 0): not placed. false and Error for a file which can't be read
bool load(std::filesystem::path const &Path, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error);
// the same, from the text of a file already read; Name tells the format by its extension
bool load_text(std::string_view const Text, std::string_view const Name, axes const Axes, glm::dvec2 const &Origin, grid &Out, std::string &Error);
// the same from the bytes of a GeoTIFF; its coordinates are always those of the format
bool load_geotiff(std::vector<std::uint8_t> const &Data, grid &Out, std::string &Error);

// the share (0..1) of the heights of the file at a point Inside metres inside its edge, over a band of Feather metres: smooth at
// both ends of the band, so the terrain doesn't break at its edges. Feather 0: the file as it is right up to its edge
double blend(double const Inside, double const Feather);

} // namespace editor_dem
