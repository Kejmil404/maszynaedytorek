#include "stdafx.h"
#include "doctest.h"
#include "editor/editorDem.hpp"

#include <cmath>
#include <string>

TEST_CASE("dem: an ASCII grid lies on PUWG 1992 by the outer corner of its south-western pixel") {
	std::string const text{
	    "ncols 3\nnrows 2\nxllcorner 500000\nyllcorner 300000\ncellsize 1\nnodata_value -9999\n"
	    "10 11 12\n"
	    "20 -9999 22\n"};
	editor_dem::grid grid;
	std::string error;

	REQUIRE(editor_dem::load_text(text, "sheet.asc", editor_dem::axes::as_written, {0.0, 0.0}, grid, error));
	CHECK(grid.width == 3);
	CHECK(grid.height == 2);
	CHECK(grid.east == doctest::Approx(500000.0));
	CHECK(grid.north == doctest::Approx(300002.0));
	CHECK(grid.known() == 5);
	double height{0.0};
	// the middle of the north-western pixel, the first value of the file
	REQUIRE(grid.sample(500000.5, 300001.5, height));
	CHECK(height == doctest::Approx(10.0));
	// halfway between the two first values of the first row
	REQUIRE(grid.sample(500001.0, 300001.5, height));
	CHECK(height == doctest::Approx(10.5));
	CHECK_FALSE(grid.sample(499990.0, 300001.5, height));
}

TEST_CASE("dem: the coordinates given the other way round are found by the place of the scenery") {
	// the northing first, as the Polish maps name it X
	std::string const text{"ncols 2\nnrows 2\nxllcenter 300000.5\nyllcenter 500000.5\ncellsize 1\n1 2\n3 4\n"};
	editor_dem::grid grid;
	std::string error;

	REQUIRE(editor_dem::load_text(text, "sheet.asc", editor_dem::axes::automatic, {500010.0, 300010.0}, grid, error));
	CHECK(grid.swapped);
	CHECK(grid.east == doctest::Approx(500000.0));
	CHECK(grid.north == doctest::Approx(300002.0));
}

TEST_CASE("dem: the points of a file make a grid of their step") {
	std::string const text{
	    "x y z\n"
	    "500000.5 300000.5 1\n500001.5 300000.5 2\n"
	    "500000.5 300001.5 3\n500001.5 300001.5 4\n"};
	editor_dem::grid grid;
	std::string error;

	REQUIRE(editor_dem::load_text(text, "points.xyz", editor_dem::axes::as_written, {0.0, 0.0}, grid, error));
	CHECK(grid.width == 2);
	CHECK(grid.height == 2);
	CHECK(grid.pixel == doctest::Approx(1.0));
	CHECK(grid.east == doctest::Approx(500000.0));
	CHECK(grid.north == doctest::Approx(300002.0));
	double height{0.0};
	REQUIRE(grid.sample(500000.5, 300001.5, height));
	CHECK(height == doctest::Approx(3.0));
}

TEST_CASE("dem: a broken file is told, not read as heights of 0 m") {
	editor_dem::grid grid;
	std::string error;

	CHECK_FALSE(editor_dem::load_text("ncols 3\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 1\n1 2 3\n4\n", "cut.asc", editor_dem::axes::as_written, {0.0, 0.0}, grid, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("dem: the edge of the file blends in smoothly") {
	CHECK(editor_dem::blend(-1.0, 20.0) == doctest::Approx(0.0));
	CHECK(editor_dem::blend(0.0, 20.0) == doctest::Approx(0.0));
	CHECK(editor_dem::blend(10.0, 20.0) == doctest::Approx(0.5));
	CHECK(editor_dem::blend(25.0, 20.0) == doctest::Approx(1.0));
	CHECK(editor_dem::blend(0.0, 0.0) == doctest::Approx(1.0));
}
