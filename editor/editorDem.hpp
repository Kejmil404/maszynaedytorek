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
