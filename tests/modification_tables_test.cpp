#include <doctest/doctest.h>

import std;
import types;
import no_init_allocator;
import BinaryReader;
import BinaryWriter;
import SLK;
import ModificationTables;

using namespace std::string_literals;

constexpr u32 mod_table_version = 3;

TEST_CASE("save_modification_table data repeat") {
	slk::SLK meta;
	meta.add_row("Ocr6");
	meta.set_shadow_data("field", "Ocr6", "data");
	meta.set_shadow_data("repeat", "Ocr6", "4"); // non-zero indicates repeating
	meta.set_shadow_data("data", "Ocr6", "1");
	meta.set_shadow_data("type", "Ocr6", "int");
	meta.build_meta_map();

	slk::SLK data;
	data.add_row("Test");
	data.set_shadow_data("dataa1", "Test", "67");

	BinaryWriter writer;
	save_modification_table(writer, data, meta, false, true, false);

	slk::SLK loaded;
	BinaryReader reader(writer.buffer);
	load_modification_table(reader, mod_table_version, loaded, meta, false, true);

	CHECK(loaded.data("dataa1", "Test") == "67");
}

TEST_CASE("custom object with missing base rawcode keeps its map modifications") {
	slk::SLK meta;
	meta.add_row("unam");
	meta.set_shadow_data("field", "unam", "unam");
	meta.set_shadow_data("repeat", "unam", "0");

	BinaryWriter writer;
	writer.write<u32>(1); // object count
	writer.write_string("zzzz"); // unavailable base rawcode
	writer.write_string("hfoo"); // custom object rawcode
	writer.write<u32>(1); // set count (version 3)
	writer.write<u32>(0); // set flags
	writer.write<u32>(1); // modification count
	writer.write_string("unam");
	writer.write<u32>(3); // string
	writer.write_c_string("Custom Hero");
	writer.write<u32>(0); // trailing word after field data

	slk::SLK loaded;
	BinaryReader reader(writer.buffer);
	load_modification_table(reader, mod_table_version, loaded, meta, true, false, "test.w3u");

	CHECK(loaded.base_data.contains("hfoo"));
	CHECK(loaded.data("oldid", "hfoo") == "zzzz");
	CHECK(loaded.data("unam", "hfoo") == "Custom Hero");
}
