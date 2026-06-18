#include <doctest/doctest.h>

import std;
import ObjectData;

using namespace std::string_literals;

TEST_CASE("classify_cell_merge: all null (no overrides)") {
	// Nothing changed anywhere — unchanged
	CHECK(classify_cell_merge(std::nullopt, std::nullopt, std::nullopt) == CellMerge::unchanged);
}

TEST_CASE("classify_cell_merge: mine == theirs") {
	// Both changed to the same value — unchanged
	CHECK(classify_cell_merge(std::nullopt, std::string("a"), std::string("a")) == CellMerge::unchanged);
	CHECK(classify_cell_merge(std::string("base"), std::string("a"), std::string("a")) == CellMerge::unchanged);
}

TEST_CASE("classify_cell_merge: only mine changed") {
	// base was null, mine changed, theirs didn't
	CHECK(classify_cell_merge(std::nullopt, std::string("a"), std::nullopt) == CellMerge::take_mine);
	// base == theirs, mine differs → take_mine
	CHECK(classify_cell_merge(std::string("base"), std::string("a"), std::string("base")) == CellMerge::take_mine);
}

TEST_CASE("classify_cell_merge: only theirs changed") {
	// base was null, theirs changed, mine didn't
	CHECK(classify_cell_merge(std::nullopt, std::nullopt, std::string("b")) == CellMerge::take_theirs);
	// base == mine, theirs differs → take_theirs
	CHECK(classify_cell_merge(std::string("base"), std::string("base"), std::string("b")) == CellMerge::take_theirs);
}

TEST_CASE("classify_cell_merge: both changed differently -> conflict") {
	// base null, both set different values
	CHECK(classify_cell_merge(std::nullopt, std::string("a"), std::string("b")) == CellMerge::conflict);
	// base has value, both diverged differently
	CHECK(classify_cell_merge(std::string("base"), std::string("a"), std::string("b")) == CellMerge::conflict);
}

TEST_CASE("classify_cell_merge: mine deleted override (back to base)") {
	// base had a value, mine removed it (null), theirs unchanged
	CHECK(classify_cell_merge(std::string("base"), std::nullopt, std::string("base")) == CellMerge::take_mine);
}

TEST_CASE("classify_cell_merge: theirs deleted override (back to base)") {
	// base had a value, theirs removed it (null), mine unchanged
	CHECK(classify_cell_merge(std::string("base"), std::string("base"), std::nullopt) == CellMerge::take_theirs);
}

TEST_CASE("classify_cell_merge: both deleted -> unchanged") {
	// Both removed an override — unchanged
	CHECK(classify_cell_merge(std::string("base"), std::nullopt, std::nullopt) == CellMerge::unchanged);
}

TEST_CASE("classify_cell_merge: base null, mine null, theirs set") {
	// No override in base, mine didn't touch, theirs added
	CHECK(classify_cell_merge(std::nullopt, std::nullopt, std::string("b")) == CellMerge::take_theirs);
}

TEST_CASE("classify_cell_merge: base null, mine set, theirs null") {
	// No override in base, mine added, theirs didn't touch
	CHECK(classify_cell_merge(std::nullopt, std::string("a"), std::nullopt) == CellMerge::take_mine);
}

TEST_CASE("classify_cell_merge: all same value") {
	CHECK(classify_cell_merge(std::string("x"), std::string("x"), std::string("x")) == CellMerge::unchanged);
}

TEST_CASE("classify_cell_merge: empty string values") {
	// Empty string is a valid value, not null
	CHECK(classify_cell_merge(std::nullopt, std::string(""), std::string("")) == CellMerge::unchanged);
	CHECK(classify_cell_merge(std::nullopt, std::string("a"), std::string("")) == CellMerge::conflict);
	CHECK(classify_cell_merge(std::nullopt, std::nullopt, std::string("")) == CellMerge::take_theirs);
}
