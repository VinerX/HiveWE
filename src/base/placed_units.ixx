// Qt-free data-only codec for war3mapUnits.doo (pre-placed units & items), for the
// CLI/agent. The full Units class (src/base/units.ixx) is render-coupled (meshes,
// skeletons, ResourceManager); this module touches none of that — it reads and
// writes only the placement records.
//
// Values are kept exactly as stored in the file: positions are already in world
// coordinates (the engine's stored form), scale is the file's value (multiplier *
// 128). No terrain offset or player-index adjustment is applied, so the data
// round-trips byte-for-byte. Optional fields are keyed off the version / subversion
// read from the file header, and written back with the same versions.

module;

#include <cstdint>

export module PlacedUnits;

import std;
import BinaryReader;
import BinaryWriter;
import Hierarchy;

export namespace placed {

struct ItemSet {
	std::vector<std::pair<std::string, uint32_t>> items; // (item id, chance %)
};

struct Unit {
	std::string id;                 // 4-char type code ("sloc" = start location)
	uint32_t variation = 0;
	float x = 0, y = 0, z = 0;      // world coordinates
	float angle = 0;                // radians
	float scale_x = 128, scale_y = 128, scale_z = 128; // stored = multiplier * 128
	std::string skin_id;            // present iff version >= 8

	uint8_t flags = 2;
	uint32_t player = 0;
	uint8_t unknown1 = 0, unknown2 = 0;
	uint32_t health = 0xFFFFFFFF;   // -1 = default
	uint32_t mana = 0xFFFFFFFF;     // -1 = default

	uint32_t item_table_pointer = 0xFFFFFFFF; // present iff subversion >= 11
	std::vector<ItemSet> item_sets;

	uint32_t gold = 12500;
	float target_acquisition = -1;
	uint32_t level = 1;
	uint32_t strength = 0, agility = 0, intelligence = 0; // present iff subversion >= 11

	std::vector<std::pair<uint32_t, std::string>> inventory; // (slot, item id)
	std::vector<std::tuple<std::string, uint32_t, uint32_t>> abilities; // (id, autocast, level)

	uint32_t random_type = 0;
	std::vector<uint8_t> random;

	uint32_t custom_color = 0xFFFFFFFF; // -1 = none
	uint32_t waygate = 0xFFFFFFFF;      // -1 = none
	uint32_t creation_number = 0;
};

struct File {
	uint32_t version = 8;
	uint32_t subversion = 11;
	bool has_skin = true;   // version >= 8
	bool has_sub11 = true;  // subversion >= 11
	std::vector<Unit> units; // units and items together, in file order
};

inline std::optional<File> load(std::string& err) {
	auto result = hierarchy.map_file_read("war3mapUnits.doo");
	if (!result.has_value()) {
		err = "could not read war3mapUnits.doo: " + result.error();
		return std::nullopt;
	}
	try {
		BinaryReader reader = std::move(result.value());
		File file;
		if (reader.read_string(4) != "W3do") {
			err = "war3mapUnits.doo has an invalid magic number (expected W3do)";
			return std::nullopt;
		}
		file.version = reader.read<uint32_t>();
		file.subversion = reader.read<uint32_t>();
		file.has_skin = file.version >= 8;
		file.has_sub11 = file.subversion >= 11;

		const uint32_t count = reader.read<uint32_t>();
		file.units.reserve(count);
		for (uint32_t k = 0; k < count; ++k) {
			Unit u;
			u.id = reader.read_string(4);
			u.variation = reader.read<uint32_t>();
			u.x = reader.read<float>();
			u.y = reader.read<float>();
			u.z = reader.read<float>();
			u.angle = reader.read<float>();
			u.scale_x = reader.read<float>();
			u.scale_y = reader.read<float>();
			u.scale_z = reader.read<float>();
			if (file.has_skin) {
				u.skin_id = reader.read_string(4);
			}
			u.flags = reader.read<uint8_t>();
			u.player = reader.read<uint32_t>();
			u.unknown1 = reader.read<uint8_t>();
			u.unknown2 = reader.read<uint8_t>();
			u.health = reader.read<uint32_t>();
			u.mana = reader.read<uint32_t>();
			if (file.has_sub11) {
				u.item_table_pointer = reader.read<uint32_t>();
			}
			u.item_sets.resize(reader.read<uint32_t>());
			for (auto& set : u.item_sets) {
				set.items.resize(reader.read<uint32_t>());
				for (auto& [id, chance] : set.items) {
					id = reader.read_string(4);
					chance = reader.read<uint32_t>();
				}
			}
			u.gold = reader.read<uint32_t>();
			u.target_acquisition = reader.read<float>();
			u.level = reader.read<uint32_t>();
			if (file.has_sub11) {
				u.strength = reader.read<uint32_t>();
				u.agility = reader.read<uint32_t>();
				u.intelligence = reader.read<uint32_t>();
			}
			u.inventory.resize(reader.read<uint32_t>());
			for (auto& [slot, id] : u.inventory) {
				slot = reader.read<uint32_t>();
				id = reader.read_string(4);
			}
			u.abilities.resize(reader.read<uint32_t>());
			for (auto& [id, autocast, level] : u.abilities) {
				id = reader.read_string(4);
				autocast = reader.read<uint32_t>();
				level = reader.read<uint32_t>();
			}
			u.random_type = reader.read<uint32_t>();
			switch (u.random_type) {
				case 0: u.random = reader.read_vector<uint8_t>(4); break;
				case 1: u.random = reader.read_vector<uint8_t>(8); break;
				case 2: u.random = reader.read_vector<uint8_t>(reader.read<uint32_t>() * 8); break;
				default: break;
			}
			u.custom_color = reader.read<uint32_t>();
			u.waygate = reader.read<uint32_t>();
			u.creation_number = reader.read<uint32_t>();
			file.units.push_back(std::move(u));
		}
		return file;
	} catch (const std::exception& e) {
		err = std::string("failed parsing war3mapUnits.doo: ") + e.what();
		return std::nullopt;
	}
}

inline bool save(const File& file, std::string& err) {
	try {
		BinaryWriter writer;
		writer.write_string("W3do");
		writer.write<uint32_t>(file.version);
		writer.write<uint32_t>(file.subversion);
		writer.write<uint32_t>(static_cast<uint32_t>(file.units.size()));
		for (const auto& u : file.units) {
			// id is always exactly 4 bytes.
			char id4[4] = { ' ', ' ', ' ', ' ' };
			for (std::size_t i = 0; i < 4 && i < u.id.size(); ++i) id4[i] = u.id[i];
			writer.buffer.insert(writer.buffer.end(), id4, id4 + 4);
			writer.write<uint32_t>(u.variation);
			writer.write<float>(u.x);
			writer.write<float>(u.y);
			writer.write<float>(u.z);
			writer.write<float>(u.angle);
			writer.write<float>(u.scale_x);
			writer.write<float>(u.scale_y);
			writer.write<float>(u.scale_z);
			if (file.has_skin) {
				char skin4[4] = { ' ', ' ', ' ', ' ' };
				const std::string& s = u.skin_id.empty() ? u.id : u.skin_id;
				for (std::size_t i = 0; i < 4 && i < s.size(); ++i) skin4[i] = s[i];
				writer.buffer.insert(writer.buffer.end(), skin4, skin4 + 4);
			}
			writer.write<uint8_t>(u.flags);
			writer.write<uint32_t>(u.player);
			writer.write<uint8_t>(u.unknown1);
			writer.write<uint8_t>(u.unknown2);
			writer.write<uint32_t>(u.health);
			writer.write<uint32_t>(u.mana);
			if (file.has_sub11) {
				writer.write<uint32_t>(u.item_table_pointer);
			}
			writer.write<uint32_t>(static_cast<uint32_t>(u.item_sets.size()));
			for (const auto& set : u.item_sets) {
				writer.write<uint32_t>(static_cast<uint32_t>(set.items.size()));
				for (const auto& [id, chance] : set.items) {
					char b[4] = { ' ', ' ', ' ', ' ' };
					for (std::size_t i = 0; i < 4 && i < id.size(); ++i) b[i] = id[i];
					writer.buffer.insert(writer.buffer.end(), b, b + 4);
					writer.write<uint32_t>(chance);
				}
			}
			writer.write<uint32_t>(u.gold);
			writer.write<float>(u.target_acquisition);
			writer.write<uint32_t>(u.level);
			if (file.has_sub11) {
				writer.write<uint32_t>(u.strength);
				writer.write<uint32_t>(u.agility);
				writer.write<uint32_t>(u.intelligence);
			}
			writer.write<uint32_t>(static_cast<uint32_t>(u.inventory.size()));
			for (const auto& [slot, id] : u.inventory) {
				writer.write<uint32_t>(slot);
				char b[4] = { ' ', ' ', ' ', ' ' };
				for (std::size_t i = 0; i < 4 && i < id.size(); ++i) b[i] = id[i];
				writer.buffer.insert(writer.buffer.end(), b, b + 4);
			}
			writer.write<uint32_t>(static_cast<uint32_t>(u.abilities.size()));
			for (const auto& [id, autocast, level] : u.abilities) {
				char b[4] = { ' ', ' ', ' ', ' ' };
				for (std::size_t i = 0; i < 4 && i < id.size(); ++i) b[i] = id[i];
				writer.buffer.insert(writer.buffer.end(), b, b + 4);
				writer.write<uint32_t>(autocast);
				writer.write<uint32_t>(level);
			}
			writer.write<uint32_t>(u.random_type);
			writer.write_vector(u.random);
			writer.write<uint32_t>(u.custom_color);
			writer.write<uint32_t>(u.waygate);
			writer.write<uint32_t>(u.creation_number);
		}
		hierarchy.map_file_write("war3mapUnits.doo", writer.buffer);
		return true;
	} catch (const std::exception& e) {
		err = std::string("failed writing war3mapUnits.doo: ") + e.what();
		return false;
	}
}

} // namespace placed
