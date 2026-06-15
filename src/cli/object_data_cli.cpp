// HiveWE_cli — object-data commands (units/items/abilities/doodads/destructibles/
// upgrades/buffs). This is a module unit that imports the Qt-free HiveWE_core
// modules so the CLI can read and edit object data with no Qt/OpenGL context.
//
// It deliberately uses no header-based libraries (no nlohmann/json): mixing
// `#include`-based std with `import std` and the imported `hierarchy` inline
// global trips an MSVC internal compiler error (symtable.cpp). So this module is
// pure `import std` + core modules and emits JSON via a tiny local writer; the
// caller (main_cli.cpp) just prints the returned string.

export module ObjectDataCli;

import std;
import ObjectData;
import ModificationTables;
import Globals;
import Hierarchy;
import SLK;
import INI;
import RaceGraph;
import TriggerStrings;
import MapInfo;
import Sounds;
import SafeMove;
import Utilities;
import BinaryReader;
import BinaryWriter;
import PathingAnalysis;
import PlacedUnits;

namespace {
namespace fs = std::filesystem;

std::string resolve_trigger_string(std::string raw, const TriggerStrings& ts) {
	if (raw.starts_with("TRIGSTR")) {
		const std::string_view resolved = ts.string(raw);
		if (!resolved.empty()) {
			return to_utf8(resolved);
		}
	}
	return to_utf8(raw);
}

// ---- tiny JSON writer -----------------------------------------------------

// Quote + escape a string as a JSON string literal. UTF-8 bytes (>= 0x80, e.g.
// Cyrillic unit names) pass through unchanged; only control characters and the
// JSON metacharacters are escaped.
std::string jstr(std::string_view s) {
	std::string o = "\"";
	for (char c : s) {
		switch (c) {
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					o += std::format("\\u{:04x}", static_cast<int>(static_cast<unsigned char>(c)));
				} else {
					o += c;
				}
		}
	}
	o += "\"";
	return o;
}

// Builds a JSON object from "key": value member strings (values already encoded).
struct JsonObject {
	std::vector<std::string> members;
	void raw(std::string_view key, std::string_view encoded_value) {
		members.push_back(jstr(key) + ":" + std::string(encoded_value));
	}
	void str(std::string_view key, std::string_view value) { raw(key, jstr(value)); }
	void boolean(std::string_view key, bool value) { raw(key, value ? "true" : "false"); }
	void number(std::string_view key, std::size_t value) { raw(key, std::to_string(value)); }
	std::string dump() const {
		std::string o = "{";
		for (std::size_t i = 0; i < members.size(); ++i) {
			if (i) o += ",";
			o += members[i];
		}
		o += "}";
		return o;
	}
};

// ---- small arg parser -----------------------------------------------------

struct CliArgs {
	std::string command;
	std::unordered_map<std::string, std::string> options;
	std::vector<std::string> flags;

	bool has_flag(const std::string& name) const {
		return std::find(flags.begin(), flags.end(), name) != flags.end();
	}
	std::optional<std::string> get(const std::string& name) const {
		const auto it = options.find(name);
		if (it == options.end()) {
			return std::nullopt;
		}
		return it->second;
	}
};

CliArgs parse(int argc, char* argv[]) {
	CliArgs args;
	if (argc >= 2) {
		args.command = argv[1];
	}
	for (int i = 2; i < argc; ++i) {
		std::string token = argv[i];
		if (token.rfind("--", 0) != 0) {
			continue;
		}
		const std::string key = token.substr(2);
		if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
			args.options[key] = argv[++i];
		} else {
			args.flags.push_back(key);
		}
	}
	return args;
}

std::string to_lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string to_lower_utf8(std::string_view sv) {
	std::string result;
	result.reserve(sv.size());
	for (std::size_t i = 0; i < sv.size();) {
		unsigned char c = static_cast<unsigned char>(sv[i]);
		if (c < 0x80) {
			result.push_back(static_cast<char>(std::tolower(c)));
			++i;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < sv.size()) {
			unsigned char c2 = static_cast<unsigned char>(sv[i + 1]);
			if ((c2 & 0xC0) == 0x80) {
				unsigned int cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
				if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
				else if (cp == 0x0401) cp = 0x0451;
				result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
				result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				i += 2;
			} else {
				result.push_back(static_cast<char>(c));
				++i;
			}
		} else if ((c & 0xF0) == 0xE0 && i + 2 < sv.size()) {
			result.append(sv.data() + i, 3);
			i += 3;
		} else if ((c & 0xF8) == 0xF0 && i + 3 < sv.size()) {
			result.append(sv.data() + i, 4);
			i += 4;
		} else {
			result.push_back(static_cast<char>(c));
			++i;
		}
	}
	return result;
}

std::string hex_to_string(std::string_view hex) {
	std::string out;
	out.reserve(hex.size() / 2);
	for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
		char c = 0;
		auto d = [](char ch) -> int {
			if (ch >= '0' && ch <= '9') return ch - '0';
			if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
			if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
			return -1;
		};
		int hi = d(hex[i]);
		int lo = d(hex[i + 1]);
		if (hi < 0 || lo < 0) continue;
		c = static_cast<char>((hi << 4) | lo);
		out.push_back(c);
	}
	return out;
}

std::optional<std::string> resolve_suffix(const CliArgs& args) {
	if (const auto h = args.get("suffix-hex")) {
		return hex_to_string(*h);
	}
	if (const auto f = args.get("suffix-file")) {
		try {
			std::ifstream file(*f, std::ios::binary);
			if (!file) return std::nullopt;
			std::stringstream ss;
			ss << file.rdbuf();
			std::string content = ss.str();
			while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) content.pop_back();
			return content;
		} catch (const std::exception&) {
			return std::nullopt;
		}
	}
	return args.get("suffix");
}

std::vector<std::string> split_csv(std::string_view sv) {
	std::vector<std::string> out;
	std::string current;
	for (char c : sv) {
		if (c == ',') {
			if (!current.empty()) {
				out.push_back(current);
			}
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	if (!current.empty()) {
		out.push_back(current);
	}
	for (auto& item : out) {
		while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
		while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
	}
	out.erase(std::remove_if(out.begin(), out.end(), [](const auto& s) { return s.empty(); }), out.end());
	return out;
}

// Parses "x,y" into a pair of doubles. Returns nullopt on malformed input.
std::optional<std::pair<double, double>> parse_xy(std::string_view sv) {
	const auto comma = sv.find(',');
	if (comma == std::string_view::npos) return std::nullopt;
	try {
		const double x = std::stod(std::string(sv.substr(0, comma)));
		const double y = std::stod(std::string(sv.substr(comma + 1)));
		return std::pair{ x, y };
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

// Converts an (x, y) in the chosen coordinate system to a pathing cell.
// system: "world" (default), "tile" (terrain tiles), or "cell" (raw pathing cells).
// Returns nullopt with `err` set when world coords are requested but no offset is
// available (no readable war3map.w3e).
std::optional<std::pair<int, int>> to_cell(const pathing::Grid& grid, std::string_view system,
										   double x, double y, std::string& err) {
	if (system == "cell") {
		return std::pair{ static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)) };
	}
	if (system == "tile") {
		return std::pair{ static_cast<int>(std::lround(x * 4.0)), static_cast<int>(std::lround(y * 4.0)) };
	}
	// world
	if (!grid.has_offset) {
		err = "world coordinates need the terrain offset from war3map.w3e, which could not be read; "
			  "pass --coords cell or --coords tile instead";
		return std::nullopt;
	}
	return std::pair{ grid.world_to_cell_x(static_cast<float>(x)), grid.world_to_cell_y(static_cast<float>(y)) };
}

// ---- regions (war3map.w3r) ------------------------------------------------
// Data-only codec for the regions file: raw world coordinates as stored (no
// terrain-offset conversion), so it round-trips and matches the coordinates used
// by triggers/units. Format version 5.

struct RegionRecord {
	float left = 0, bottom = 0, right = 0, top = 0; // world units
	std::string name;
	int32_t creation_number = 0;
	std::string weather_id;  // 4-char code, empty when none
	std::string ambient_id;  // ambient sound, optional
	uint8_t color[3] = { 255, 255, 255 };
	uint8_t end_byte = 0xff; // trailing marker after the colour, preserved verbatim
};

std::optional<std::vector<RegionRecord>> read_regions(std::string& err) {
	auto result = hierarchy.map_file_read("war3map.w3r");
	if (!result.has_value()) {
		err = "could not read war3map.w3r: " + result.error();
		return std::nullopt;
	}
	try {
		BinaryReader reader = std::move(result.value());
		const uint32_t version = reader.read<uint32_t>();
		if (version != 5) {
			err = std::format("unexpected war3map.w3r version {} (expected 5)", version);
			return std::nullopt;
		}
		std::vector<RegionRecord> out;
		out.resize(reader.read<uint32_t>());
		for (auto& r : out) {
			r.left = reader.read<float>();
			r.bottom = reader.read<float>();
			r.right = reader.read<float>();
			r.top = reader.read<float>();
			r.name = reader.read_c_string();
			r.creation_number = reader.read<int32_t>();
			r.weather_id = reader.read_string(4);
			r.ambient_id = reader.read_c_string();
			r.color[0] = reader.read<uint8_t>();
			r.color[1] = reader.read<uint8_t>();
			r.color[2] = reader.read<uint8_t>();
			r.end_byte = reader.read<uint8_t>();
		}
		return out;
	} catch (const std::exception& e) {
		err = std::string("failed parsing war3map.w3r: ") + e.what();
		return std::nullopt;
	}
}

bool write_regions(const std::vector<RegionRecord>& regions, std::string& err) {
	try {
		BinaryWriter writer;
		writer.write<uint32_t>(5);
		writer.write<uint32_t>(static_cast<uint32_t>(regions.size()));
		for (const auto& r : regions) {
			writer.write<float>(r.left);
			writer.write<float>(r.bottom);
			writer.write<float>(r.right);
			writer.write<float>(r.top);
			writer.write_c_string(r.name);
			writer.write<int32_t>(r.creation_number);
			// weather is exactly 4 bytes; pad/truncate, zero when empty.
			char weather[4] = { 0, 0, 0, 0 };
			for (std::size_t i = 0; i < 4 && i < r.weather_id.size(); ++i) weather[i] = r.weather_id[i];
			writer.buffer.insert(writer.buffer.end(), weather, weather + 4);
			writer.write_c_string(r.ambient_id);
			writer.write<uint8_t>(r.color[0]);
			writer.write<uint8_t>(r.color[1]);
			writer.write<uint8_t>(r.color[2]);
			writer.write<uint8_t>(r.end_byte);
		}
		hierarchy.map_file_write("war3map.w3r", writer.buffer);
		return true;
	} catch (const std::exception& e) {
		err = std::string("failed writing war3map.w3r: ") + e.what();
		return false;
	}
}

// ---- placed units (war3mapUnits.doo) --------------------------------------

std::string placed_unit_to_json(const placed::Unit& u, std::size_t index) {
	JsonObject o;
	o.number("index", index);
	o.str("id", u.id);
	o.str("skin_id", u.skin_id);
	o.raw("player", std::to_string(u.player));
	o.raw("position_world", std::format("[{},{},{}]", u.x, u.y, u.z));
	o.raw("angle_rad", std::to_string(u.angle));
	o.raw("angle_deg", std::to_string(u.angle * 180.0 / std::numbers::pi));
	o.raw("scale", std::format("[{},{},{}]", u.scale_x / 128.f, u.scale_y / 128.f, u.scale_z / 128.f));
	o.raw("health", std::to_string(static_cast<int32_t>(u.health)));
	o.raw("mana", std::to_string(static_cast<int32_t>(u.mana)));
	o.raw("gold", std::to_string(u.gold));
	o.raw("level", std::to_string(u.level));
	o.raw("strength", std::to_string(u.strength));
	o.raw("agility", std::to_string(u.agility));
	o.raw("intelligence", std::to_string(u.intelligence));
	o.raw("creation_number", std::to_string(u.creation_number));
	o.raw("waygate", std::to_string(static_cast<int32_t>(u.waygate)));
	// Inventory items.
	std::string inv = "[";
	for (std::size_t i = 0; i < u.inventory.size(); ++i) {
		if (i) inv += ",";
		inv += jstr(u.inventory[i].second);
	}
	inv += "]";
	o.raw("inventory", inv);
	// Abilities.
	std::string abil = "[";
	for (std::size_t i = 0; i < u.abilities.size(); ++i) {
		if (i) abil += ",";
		abil += jstr(std::get<0>(u.abilities[i]));
	}
	abil += "]";
	o.raw("abilities", abil);
	o.raw("dropped_item_sets", std::to_string(u.item_sets.size()));
	return o.dump();
}

std::string region_to_json(const RegionRecord& r, std::size_t index) {
	JsonObject o;
	o.number("index", index);
	o.str("name", r.name);
	o.raw("creation_number", std::to_string(r.creation_number));
	o.raw("rect_world", std::format("[{},{},{},{}]", r.left, r.bottom, r.right, r.top));
	o.raw("center_world", std::format("[{},{}]", (r.left + r.right) / 2.f, (r.bottom + r.top) / 2.f));
	o.str("weather_id", r.weather_id);
	o.str("ambient_id", r.ambient_id);
	o.raw("color", std::format("[{},{},{}]", r.color[0], r.color[1], r.color[2]));
	return o.dump();
}

std::string changelog_ts() {
	using namespace std::chrono;
	const auto now = system_clock::now();
	const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
	auto secs = static_cast<int64_t>(duration_cast<seconds>(now.time_since_epoch()).count());
	if (secs < 0) secs = 0;

	const int64_t z = secs / 86400 + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	const int m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
	const int y = static_cast<int>(yoe + era * 400 + (m <= 2));

	const int64_t sod = static_cast<int64_t>(duration_cast<seconds>(now.time_since_epoch()).count()) % 86400;
	return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z",
		y, m, d, static_cast<int>(sod / 3600), static_cast<int>((sod % 3600) / 60), static_cast<int>(sod % 60), ms);
}

void append_changelog(const fs::path& map_dir, const std::string& line) {
	const fs::path log_path = map_dir / "_cli_changelog.jsonl";
	std::ofstream log(log_path, std::ios::app | std::ios::binary);
	if (log) log << line << "\n";
}

std::string string_array_json(const std::vector<std::string>& values) {
	std::string out = "[";
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i) out += ",";
		out += jstr(values[i]);
	}
	out += "]";
	return out;
}

std::string string_array_json(const std::set<std::string>& values) {
	return string_array_json(std::vector<std::string>(values.begin(), values.end()));
}

std::vector<std::string> split_rawcode_list(std::string_view value) {
	std::vector<std::string> result;
	std::string current;
	for (char c : value) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'') {
			current.push_back(c);
		} else {
			if (current.size() == 4) {
				result.push_back(current);
			}
			current.clear();
		}
	}
	if (current.size() == 4) {
		result.push_back(current);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

using RaceNodeIndex = std::unordered_map<std::string, const RaceGraphNode*>;

RaceNodeIndex build_race_node_index(const RaceGraphAnalysis& analysis) {
	RaceNodeIndex index;
	for (const auto& node : analysis.nodes) {
		index.emplace(node.id, &node);
	}
	return index;
}

std::string race_node_ref_json(const std::string& id, const RaceNodeIndex& index) {
	JsonObject o;
	o.str("id", id);
	if (const auto it = index.find(id); it != index.end()) {
		o.str("name", it->second->name);
		o.str("base_id", it->second->base_id);
		o.str("category", it->second->category);
		o.str("editor_suffix", it->second->editor_suffix);
	}
	return o.dump();
}

std::string race_node_ref_array_json(const std::vector<std::string>& ids, const RaceNodeIndex& index) {
	std::string out = "[";
	for (std::size_t i = 0; i < ids.size(); ++i) {
		if (i) out += ",";
		out += race_node_ref_json(ids[i], index);
	}
	out += "]";
	return out;
}

std::string grouped_race_edges_json(
	const std::vector<RaceGraphEdge>& edges,
	const std::vector<std::string>& kinds,
	const RaceNodeIndex& index
) {
	std::set<std::string> kind_filter(kinds.begin(), kinds.end());
	std::map<std::string, std::set<std::string>> grouped;
	for (const auto& edge : edges) {
		if (kind_filter.contains(edge.kind)) {
			grouped[edge.from_id].insert(edge.to_id);
		}
	}

	std::string out = "[";
	bool first_group = true;
	for (const auto& [from, targets] : grouped) {
		if (!first_group) out += ",";
		first_group = false;
		JsonObject o;
		o.raw("from", race_node_ref_json(from, index));
		o.raw("targets", race_node_ref_array_json(std::vector<std::string>(targets.begin(), targets.end()), index));
		out += o.dump();
	}
	out += "]";
	return out;
}

std::string special_rules_json(const std::vector<RaceGraphEdge>& edges, const RaceNodeIndex& index) {
	static const std::set<std::string> interesting = {
		"lua-availability", "lua-auto-research", "lua-adds-ability", "lua-start-tech",
		"lua-disables-tech", "lua-enables-tech", "lua-hides-ability"
	};
	std::string out = "[";
	bool first = true;
	for (const auto& edge : edges) {
		if (!interesting.contains(edge.kind)) {
			continue;
		}
		if (!first) out += ",";
		first = false;
		JsonObject o;
		o.str("kind", edge.kind);
		o.raw("from", race_node_ref_json(edge.from_id, index));
		o.raw("to", race_node_ref_json(edge.to_id, index));
		o.str("detail", edge.detail);
		o.str("symbol", edge.symbol);
		o.str("file", edge.file);
		out += o.dump();
	}
	out += "]";
	return out;
}

std::string lua_sections_json(const std::vector<RaceLuaReference>& refs) {
	std::set<std::pair<std::string, std::string>> unique;
	for (const auto& ref : refs) {
		unique.emplace(ref.symbol, ref.file);
	}
	std::string out = "[";
	bool first = true;
	for (const auto& [symbol, file] : unique) {
		if (!first) out += ",";
		first = false;
		JsonObject o;
		o.str("symbol", symbol);
		o.str("file", file);
		out += o.dump();
	}
	out += "]";
	return out;
}

// ---- object-type registry -------------------------------------------------

struct TypeInfo {
	slk::SLK* slk;
	slk::SLK* meta;
	const char* mod_file;  // map modification table written on save
	bool optional_ints;    // matches Map::save's per-type flag
};

std::optional<TypeInfo> type_for(const std::string& name) {
	const std::string t = to_lower(name);
	if (t == "unit") return TypeInfo{&units_slk, &units_meta_slk, "war3map.w3u", false};
	if (t == "item") return TypeInfo{&items_slk, &items_meta_slk, "war3map.w3t", false};
	if (t == "ability") return TypeInfo{&abilities_slk, &abilities_meta_slk, "war3map.w3a", true};
	if (t == "doodad") return TypeInfo{&doodads_slk, &doodads_meta_slk, "war3map.w3d", true};
	if (t == "destructible") return TypeInfo{&destructibles_slk, &destructibles_meta_slk, "war3map.w3b", false};
	if (t == "upgrade") return TypeInfo{&upgrade_slk, &upgrade_meta_slk, "war3map.w3q", true};
	if (t == "buff") return TypeInfo{&buff_slk, &buff_meta_slk, "war3map.w3h", false};
	return std::nullopt;
}

const char* const kTypeNames[] = {"unit", "item", "ability", "doodad", "destructible", "upgrade", "buff"};

std::string display_name(const slk::SLK& slk, const std::string& id, const TriggerStrings& ts) {
	for (const char* col : {"name", "editorname", "name1", "bufftip"}) {
		if (slk.column_headers.contains(col)) {
			std::string n = slk.data<std::string>(col, id);
			if (!n.empty()) {
				if (n.starts_with("TRIGSTR")) {
					std::string_view resolved = ts.string(n);
					if (!resolved.empty()) {
						return to_utf8(resolved);
					}
				}
				return to_utf8(n);
			}
		}
	}
	return {};
}

std::string editor_suffix(const slk::SLK& slk, const std::string& id, const TriggerStrings& ts) {
	for (const char* col : {"editorsuffix", "version"}) {
		if (slk.column_headers.contains(col)) {
			std::string raw = slk.data<std::string>(col, id);
			if (!raw.empty()) {
				if (raw.starts_with("TRIGSTR")) {
					std::string_view resolved = ts.string(raw);
					if (!resolved.empty()) {
						return to_utf8(resolved);
					}
				}
				return to_utf8(raw);
			}
		}
	}
	return {};
}

// ---- baked name fallback --------------------------------------------------

// Last-resort original WC3 names, shipped in the editor repo so the map-only
// command paths can still show names when live Warcraft III data (CASC) is
// unavailable. Generated via `dump-objects ... --fields name`. Format:
// tab-separated `type<TAB>id<TAB>name`, '#' comment lines skipped. Parsed by hand
// because this module is `import std` only (no nlohmann/json). Loaded once,
// relative to the project root (main_cli sets cwd there before dispatch).
const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& name_fallback_table() {
	static const std::unordered_map<std::string, std::unordered_map<std::string, std::string>> table = [] {
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> t;
		std::ifstream in("data/wc3_name_fallback.tsv", std::ios::binary);
		if (!in) {
			return t;
		}
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty() || line[0] == '#') continue;
			const auto p1 = line.find('\t');
			if (p1 == std::string::npos) continue;
			const auto p2 = line.find('\t', p1 + 1);
			if (p2 == std::string::npos) continue;
			std::string id = line.substr(p1 + 1, p2 - p1 - 1);
			std::string name = line.substr(p2 + 1);
			if (!id.empty() && !name.empty()) {
				t[line.substr(0, p1)][id] = std::move(name);
			}
		}
		return t;
	}();
	return table;
}

// Returns the baked original name for a type+rawcode, or "" if not present.
// Names are already UTF-8 (dumped via to_utf8), so no conversion is needed.
std::string fallback_name(const std::string& type, const std::string& id) {
	const auto& table = name_fallback_table();
	if (const auto it = table.find(type); it != table.end()) {
		if (const auto jt = it->second.find(id); jt != it->second.end()) {
			return jt->second;
		}
	}
	return {};
}

// ---- bootstrap ------------------------------------------------------------

// Opens CASC, loads the WorldEdit strings, points the hierarchy at the map and
// loads base + map object data. Returns an error message on failure.
std::optional<std::string> bootstrap(const std::string& warcraft, const std::string& map_dir, bool hd, TriggerStrings& ts) {
	hierarchy.ptr = false;
	hierarchy.hd = hd;
	hierarchy.teen = false;
	hierarchy.local_files = false;

	if (!hierarchy.open_casc(warcraft)) {
		return "failed to open Warcraft III data (CASC) at: " + warcraft;
	}

	world_edit_strings.load("UI/WorldEditStrings.txt");
	world_edit_game_strings.load("UI/WorldEditGameStrings.txt");
	world_edit_data.load("UI/WorldEditData.txt");
	world_edit_data.substitute(world_edit_game_strings, "WorldEditStrings");
	world_edit_data.substitute(world_edit_strings, "WorldEditStrings");

	hierarchy.map_directory = std::filesystem::absolute(map_dir);

	load_base_object_data([](std::string_view) {});
	load_map_object_data();
	ts.load();
	return std::nullopt;
}

} // namespace

// Returns a JSON string; `ok` is set to its "ok" field for the process exit code.
// `warcraft_fallback` is a directory resolved by the caller (e.g. the HiveWE
// registry setting), used when --warcraft is not given.
export std::string hivewe_object_command(int argc, char* argv[], const std::string& warcraft_fallback, bool& ok) {
	const CliArgs args = parse(argc, argv);
	ok = false;

	auto error = [&](const std::string& msg) {
		JsonObject o;
		o.boolean("ok", false);
		o.str("command", args.command);
		o.str("error", msg);
		return o.dump();
	};

	if (args.command == "list-object-types") {
		std::string arr = "[";
		for (std::size_t i = 0; i < std::size(kTypeNames); ++i) {
			if (i) arr += ",";
			arr += jstr(kTypeNames[i]);
		}
		arr += "]";
		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.raw("types", arr);
		ok = true;
		return o.dump();
	}

	const auto map_opt = args.get("map");
	if (!map_opt) {
		return error("missing required option: --map <map folder>");
	}
	if (!std::filesystem::is_directory(*map_opt)) {
		return error("map folder not found: " + *map_opt);
	}

	// ---- terrain traversability (war3map.wpm), no CASC / object data needed ----
	if (args.command == "pathing-islands" || args.command == "pathing-path") {
		hierarchy.map_directory = std::filesystem::absolute(*map_opt);

		const std::string move_str = args.get("move").value_or("foot");
		const auto move_opt = pathing::parse_move_type(move_str);
		if (!move_opt) {
			return error("unknown --move '" + move_str + "' (expected foot|water|amphibious|fly)");
		}
		const pathing::MoveType move = *move_opt;

		std::string load_err;
		const auto grid_opt = pathing::load_grid(load_err);
		if (!grid_opt) {
			return error(load_err);
		}
		const pathing::Grid& grid = *grid_opt;

		JsonObject grid_obj;
		grid_obj.number("cells_wide", static_cast<std::size_t>(grid.width));
		grid_obj.number("cells_high", static_cast<std::size_t>(grid.height));
		grid_obj.boolean("has_world_offset", grid.has_offset);
		if (grid.has_offset) {
			grid_obj.raw("offset_x", std::to_string(grid.offset_x));
			grid_obj.raw("offset_y", std::to_string(grid.offset_y));
		}

		if (args.command == "pathing-islands") {
			const pathing::Components comps = pathing::connected_components(grid, move);

			std::size_t min_cells = 1;
			if (const auto m = args.get("min-cells")) {
				try { min_cells = static_cast<std::size_t>(std::stoul(*m)); } catch (...) {}
			}
			std::size_t limit = 50;
			if (const auto l = args.get("limit")) {
				try { limit = static_cast<std::size_t>(std::stoul(*l)); } catch (...) {}
			}

			std::string arr = "[";
			std::size_t shown = 0;
			std::size_t total_shown = 0;
			for (const auto& c : comps.components) {
				if (c.cell_count < min_cells) continue;
				total_shown++;
				if (shown >= limit) continue;
				if (shown) arr += ",";
				shown++;

				const double cx = c.sum_x / static_cast<double>(c.cell_count);
				const double cy = c.sum_y / static_cast<double>(c.cell_count);

				JsonObject island;
				island.number("id", static_cast<std::size_t>(c.id));
				island.number("cells", c.cell_count);
				// Each cell is 32x32 world units.
				island.raw("area_world", std::to_string(static_cast<double>(c.cell_count) * 32.0 * 32.0));
				island.raw("bbox_cell", std::format("[{},{},{},{}]", c.min_x, c.min_y, c.max_x, c.max_y));
				island.raw("centroid_cell", std::format("[{},{}]",
					static_cast<int>(std::lround(cx)), static_cast<int>(std::lround(cy))));
				if (grid.has_offset) {
					island.raw("bbox_world", std::format("[{},{},{},{}]",
						grid.cell_to_world_x(c.min_x), grid.cell_to_world_y(c.min_y),
						grid.cell_to_world_x(c.max_x), grid.cell_to_world_y(c.max_y)));
					island.raw("centroid_world", std::format("[{},{}]",
						grid.cell_to_world_x(static_cast<int>(std::lround(cx))),
						grid.cell_to_world_y(static_cast<int>(std::lround(cy)))));
				}
				arr += island.dump();
			}
			arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("move", pathing::move_type_name(move));
			o.raw("grid", grid_obj.dump());
			o.number("island_count", static_cast<std::size_t>(comps.count));
			o.number("islands_returned", shown);
			o.number("islands_matching_filter", total_shown);
			o.raw("islands", arr);
			ok = true;
			return o.dump();
		}

		// pathing-path
		const std::string system = args.get("coords").value_or("world");
		if (system != "world" && system != "tile" && system != "cell") {
			return error("unknown --coords '" + system + "' (expected world|tile|cell)");
		}
		const auto from_opt = args.get("from");
		const auto to_opt = args.get("to");
		if (!from_opt || !to_opt) {
			return error("pathing-path needs --from \"x,y\" and --to \"x,y\"");
		}
		const auto from_xy = parse_xy(*from_opt);
		const auto to_xy = parse_xy(*to_opt);
		if (!from_xy || !to_xy) {
			return error("--from/--to must be \"x,y\" (e.g. --from \"512,-1024\")");
		}

		std::string conv_err;
		const auto start_cell = to_cell(grid, system, from_xy->first, from_xy->second, conv_err);
		if (!start_cell) return error(conv_err);
		const auto goal_cell = to_cell(grid, system, to_xy->first, to_xy->second, conv_err);
		if (!goal_cell) return error(conv_err);

		// Approximate coordinates (e.g. a unit position on an unwalkable footprint)
		// snap to the nearest passable cell within --snap-radius (Chebyshev), unless
		// --no-snap is given.
		const int snap_radius = args.has_flag("no-snap") ? 0 : [&] {
			if (const auto s = args.get("snap-radius")) {
				try { return std::max(0, std::stoi(*s)); } catch (...) {}
			}
			return 16;
		}();
		auto snap_cell = [&](std::pair<int, int> c) -> std::pair<int, int> {
			if (snap_radius <= 0) return c;
			return pathing::nearest_passable(grid, move, c.first, c.second, snap_radius).value_or(c);
		};

		// Optional portals: "ax,ay->bx,by;cx,cy->dx,dy" in the same coordinate system.
		std::vector<pathing::Portal> portals;
		if (const auto portal_arg = args.get("portals")) {
			std::stringstream ss(*portal_arg);
			std::string segment;
			while (std::getline(ss, segment, ';')) {
				const auto arrow = segment.find("->");
				if (arrow == std::string::npos) continue;
				const auto a = parse_xy(segment.substr(0, arrow));
				const auto b = parse_xy(segment.substr(arrow + 2));
				if (!a || !b) continue;
				const auto ac = to_cell(grid, system, a->first, a->second, conv_err);
				const auto bc = to_cell(grid, system, b->first, b->second, conv_err);
				if (ac && bc) {
					const auto sa = snap_cell(*ac);
					const auto sb = snap_cell(*bc);
					portals.push_back({ sa.first, sa.second, sb.first, sb.second });
				}
			}
		}

		const bool oob_start = !grid.in_bounds(start_cell->first, start_cell->second);
		const bool oob_goal = !grid.in_bounds(goal_cell->first, goal_cell->second);
		if (oob_start || oob_goal) {
			return error(std::format("coordinate maps outside the grid ({}x{} cells): {}",
				grid.width, grid.height, oob_start ? "--from" : "--to"));
		}

		const std::pair<int, int> eff_start = snap_cell(*start_cell);
		const std::pair<int, int> eff_goal = snap_cell(*goal_cell);
		const bool start_snapped = eff_start != *start_cell;
		const bool goal_snapped = eff_goal != *goal_cell;

		const pathing::PathResult path = pathing::find_path(
			grid, move, eff_start.first, eff_start.second, eff_goal.first, eff_goal.second, portals);

		// Same-island check (ignores portals): are both endpoints in one component?
		const pathing::Components comps = pathing::connected_components(grid, move);
		const int start_label = comps.labels[grid.index(eff_start.first, eff_start.second)];
		const int goal_label = comps.labels[grid.index(eff_goal.first, eff_goal.second)];

		std::string waypoints = "[";
		for (std::size_t i = 0; i < path.cells.size(); ++i) {
			if (i) waypoints += ",";
			const auto [wx, wy] = path.cells[i];
			JsonObject wp;
			wp.raw("cell", std::format("[{},{}]", wx, wy));
			if (grid.has_offset) {
				wp.raw("world", std::format("[{},{}]", grid.cell_to_world_x(wx), grid.cell_to_world_y(wy)));
			}
			waypoints += wp.dump();
		}
		waypoints += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("move", pathing::move_type_name(move));
		o.str("coords", system);
		o.raw("grid", grid_obj.dump());
		o.raw("start_cell", std::format("[{},{}]", eff_start.first, eff_start.second));
		o.raw("goal_cell", std::format("[{},{}]", eff_goal.first, eff_goal.second));
		o.boolean("start_snapped", start_snapped);
		o.boolean("goal_snapped", goal_snapped);
		if (start_snapped) o.raw("start_requested_cell", std::format("[{},{}]", start_cell->first, start_cell->second));
		if (goal_snapped) o.raw("goal_requested_cell", std::format("[{},{}]", goal_cell->first, goal_cell->second));
		o.boolean("reachable", path.reachable);
		o.boolean("same_island", start_label != -1 && start_label == goal_label);
		o.boolean("used_portal", path.used_portal);
		o.raw("cost_cells", std::to_string(path.cost));
		o.raw("length_world", std::to_string(path.cost * 32.0));
		o.number("waypoint_count", path.cells.size());
		o.raw("waypoints", waypoints);
		ok = true;
		return o.dump();
	}

	// ---- regions (war3map.w3r), no CASC / object data needed ------------------
	if (args.command == "list-regions" || args.command == "add-region" ||
		args.command == "remove-region" || args.command == "set-region") {
		hierarchy.map_directory = std::filesystem::absolute(*map_opt);

		std::string err;
		auto regions_opt = read_regions(err);
		// add-region can create the file if it is missing; others require it.
		std::vector<RegionRecord> regions;
		if (regions_opt) {
			regions = std::move(*regions_opt);
		} else if (args.command != "add-region") {
			return error(err);
		}

		auto find_index = [&](std::size_t& out_index) -> std::optional<std::string> {
			if (const auto idx = args.get("index")) {
				try {
					const std::size_t i = static_cast<std::size_t>(std::stoul(*idx));
					if (i >= regions.size()) return "region index out of range: " + *idx;
					out_index = i;
					return std::nullopt;
				} catch (...) { return "invalid --index: " + *idx; }
			}
			if (const auto name = args.get("name")) {
				for (std::size_t i = 0; i < regions.size(); ++i) {
					if (regions[i].name == *name) { out_index = i; return std::nullopt; }
				}
				return "no region named: " + *name;
			}
			return "specify --index N or --name <region name>";
		};

		auto parse_rect = [&](const std::string& s, RegionRecord& r) -> std::optional<std::string> {
			const auto parts = split_csv(s);
			if (parts.size() != 4) return "--rect must be \"left,bottom,right,top\" (world units)";
			try {
				float v[4];
				for (int i = 0; i < 4; ++i) v[i] = std::stof(parts[i]);
				// Normalise so left<=right and bottom<=top.
				r.left = std::min(v[0], v[2]);
				r.right = std::max(v[0], v[2]);
				r.bottom = std::min(v[1], v[3]);
				r.top = std::max(v[1], v[3]);
				return std::nullopt;
			} catch (...) { return "--rect values must be numbers"; }
		};

		auto apply_color = [&](const std::string& s, RegionRecord& r) -> std::optional<std::string> {
			const auto parts = split_csv(s);
			if (parts.size() != 3) return "--color must be \"r,g,b\" (0-255)";
			try {
				for (int i = 0; i < 3; ++i) r.color[i] = static_cast<uint8_t>(std::clamp(std::stoi(parts[i]), 0, 255));
				return std::nullopt;
			} catch (...) { return "--color values must be integers"; }
		};

		if (args.command == "list-regions") {
			std::string arr = "[";
			for (std::size_t i = 0; i < regions.size(); ++i) {
				if (i) arr += ",";
				arr += region_to_json(regions[i], i);
			}
			arr += "]";
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.number("region_count", regions.size());
			o.raw("regions", arr);
			ok = true;
			return o.dump();
		}

		if (args.command == "add-region") {
			const auto name = args.get("name");
			const auto rect = args.get("rect");
			if (!name) return error("add-region needs --name <name>");
			if (!rect) return error("add-region needs --rect \"left,bottom,right,top\" (world units)");
			RegionRecord r;
			r.name = *name;
			if (const auto e = parse_rect(*rect, r)) return error(*e);
			if (const auto w = args.get("weather")) r.weather_id = *w;
			if (const auto a = args.get("ambient")) r.ambient_id = *a;
			if (const auto c = args.get("color")) { if (const auto e = apply_color(*c, r)) return error(*e); }
			int32_t max_cn = 0;
			for (const auto& existing : regions) max_cn = std::max(max_cn, existing.creation_number);
			r.creation_number = max_cn + 1;
			const std::size_t new_index = regions.size();
			regions.push_back(r);

			if (!args.has_flag("dry-run")) {
				if (!write_regions(regions, err)) return error(err);
			}
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.boolean("dry_run", args.has_flag("dry-run"));
			o.number("region_count", regions.size());
			o.raw("added", region_to_json(r, new_index));
			ok = true;
			return o.dump();
		}

		// remove-region / set-region both target an existing region.
		std::size_t target = 0;
		if (const auto e = find_index(target)) return error(*e);

		if (args.command == "remove-region") {
			const RegionRecord removed = regions[target];
			regions.erase(regions.begin() + target);
			if (!args.has_flag("dry-run")) {
				if (!write_regions(regions, err)) return error(err);
			}
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.boolean("dry_run", args.has_flag("dry-run"));
			o.number("region_count", regions.size());
			o.raw("removed", region_to_json(removed, target));
			ok = true;
			return o.dump();
		}

		// set-region
		RegionRecord& r = regions[target];
		if (const auto rect = args.get("rect")) { if (const auto e = parse_rect(*rect, r)) return error(*e); }
		if (const auto rename = args.get("rename")) r.name = *rename;
		if (const auto w = args.get("weather")) r.weather_id = *w;
		if (const auto a = args.get("ambient")) r.ambient_id = *a;
		if (const auto c = args.get("color")) { if (const auto e = apply_color(*c, r)) return error(*e); }
		if (!args.has_flag("dry-run")) {
			if (!write_regions(regions, err)) return error(err);
		}
		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.boolean("dry_run", args.has_flag("dry-run"));
		o.raw("region", region_to_json(r, target));
		ok = true;
		return o.dump();
	}

	// ---- pre-placed units (war3mapUnits.doo), no CASC / object data needed ----
	if (args.command == "list-units" || args.command == "add-unit" ||
		args.command == "remove-unit" || args.command == "set-unit") {
		hierarchy.map_directory = std::filesystem::absolute(*map_opt);

		std::string err;
		auto file_opt = placed::load(err);
		placed::File file;
		if (file_opt) {
			file = std::move(*file_opt);
		} else if (args.command != "add-unit") {
			return error(err);
		}

		auto find_by_creation = [&](uint32_t cn) -> std::optional<std::size_t> {
			for (std::size_t i = 0; i < file.units.size(); ++i) {
				if (file.units[i].creation_number == cn) return i;
			}
			return std::nullopt;
		};

		if (args.command == "list-units") {
			std::optional<uint32_t> player_filter;
			if (const auto p = args.get("player")) {
				try { player_filter = static_cast<uint32_t>(std::stoul(*p)); } catch (...) {}
			}
			const auto id_filter = args.get("id");
			std::optional<std::array<float, 4>> rect; // l,b,r,t
			if (const auto r = args.get("rect")) {
				const auto parts = split_csv(*r);
				if (parts.size() == 4) {
					try {
						rect = std::array<float, 4>{ std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]) };
					} catch (...) {}
				}
			}
			std::size_t limit = 1000;
			if (const auto l = args.get("limit")) {
				try { limit = static_cast<std::size_t>(std::stoul(*l)); } catch (...) {}
			}

			std::string arr = "[";
			std::size_t shown = 0, matched = 0;
			for (std::size_t i = 0; i < file.units.size(); ++i) {
				const placed::Unit& u = file.units[i];
				if (player_filter && u.player != *player_filter) continue;
				if (id_filter && u.id != *id_filter) continue;
				if (rect) {
					const float lo_x = std::min((*rect)[0], (*rect)[2]), hi_x = std::max((*rect)[0], (*rect)[2]);
					const float lo_y = std::min((*rect)[1], (*rect)[3]), hi_y = std::max((*rect)[1], (*rect)[3]);
					if (u.x < lo_x || u.x > hi_x || u.y < lo_y || u.y > hi_y) continue;
				}
				matched++;
				if (shown >= limit) continue;
				if (shown) arr += ",";
				arr += placed_unit_to_json(u, i);
				shown++;
			}
			arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.number("total_placed", file.units.size());
			o.number("matched", matched);
			o.number("returned", shown);
			o.raw("units", arr);
			ok = true;
			return o.dump();
		}

		// Mutating commands share writing.
		auto deg_to_rad = [](double deg) { return static_cast<float>(deg * std::numbers::pi / 180.0); };
		auto write_back = [&]() -> std::optional<std::string> {
			if (args.has_flag("dry-run")) return std::nullopt;
			std::string werr;
			if (!placed::save(file, werr)) return werr;
			return std::nullopt;
		};

		if (args.command == "add-unit") {
			const auto id = args.get("id");
			const auto x = args.get("x");
			const auto y = args.get("y");
			if (!id) return error("add-unit needs --id <rawcode>");
			if (!x || !y) return error("add-unit needs --x and --y (world coordinates)");

			placed::Unit u;
			try {
				u.id = *id;
				u.skin_id = *id;
				u.x = std::stof(*x);
				u.y = std::stof(*y);
				u.z = args.get("z") ? std::stof(*args.get("z")) : 0.f;
				u.angle = args.get("angle") ? deg_to_rad(std::stod(*args.get("angle"))) : 0.f;
				const float s = args.get("scale") ? std::stof(*args.get("scale")) : 1.f;
				u.scale_x = u.scale_y = u.scale_z = s * 128.f;
				if (const auto p = args.get("player")) u.player = static_cast<uint32_t>(std::stoul(*p));
				if (const auto hp = args.get("health")) u.health = static_cast<uint32_t>(std::stol(*hp));
				if (const auto mp = args.get("mana")) u.mana = static_cast<uint32_t>(std::stol(*mp));
				if (const auto lv = args.get("level")) u.level = static_cast<uint32_t>(std::stoul(*lv));
			} catch (const std::exception&) {
				return error("add-unit: numeric option failed to parse");
			}
			u.random = { 1, 0, 0, 0 }; // matches HiveWE's Units::add_unit default
			uint32_t max_cn = 0;
			for (const auto& existing : file.units) max_cn = std::max(max_cn, existing.creation_number);
			u.creation_number = max_cn + 1;
			const std::size_t new_index = file.units.size();
			file.units.push_back(u);

			if (const auto e = write_back()) return error(*e);
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.boolean("dry_run", args.has_flag("dry-run"));
			o.number("total_placed", file.units.size());
			o.raw("added", placed_unit_to_json(u, new_index));
			ok = true;
			return o.dump();
		}

		// remove-unit / set-unit target by creation number (unique & stable).
		const auto cn_opt = args.get("creation-number");
		if (!cn_opt) return error(std::string(args.command) + " needs --creation-number N (see list-units)");
		uint32_t cn = 0;
		try { cn = static_cast<uint32_t>(std::stoul(*cn_opt)); } catch (...) { return error("invalid --creation-number: " + *cn_opt); }
		const auto target_opt = find_by_creation(cn);
		if (!target_opt) return error("no placed unit with creation_number " + *cn_opt);
		const std::size_t target = *target_opt;

		if (args.command == "remove-unit") {
			const placed::Unit removed = file.units[target];
			file.units.erase(file.units.begin() + target);
			if (const auto e = write_back()) return error(*e);
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.boolean("dry_run", args.has_flag("dry-run"));
			o.number("total_placed", file.units.size());
			o.raw("removed", placed_unit_to_json(removed, target));
			ok = true;
			return o.dump();
		}

		// set-unit
		placed::Unit& u = file.units[target];
		try {
			if (const auto p = args.get("player")) u.player = static_cast<uint32_t>(std::stoul(*p));
			if (const auto x = args.get("x")) u.x = std::stof(*x);
			if (const auto y = args.get("y")) u.y = std::stof(*y);
			if (const auto z = args.get("z")) u.z = std::stof(*z);
			if (const auto a = args.get("angle")) u.angle = deg_to_rad(std::stod(*a));
			if (const auto s = args.get("scale")) { const float v = std::stof(*s) * 128.f; u.scale_x = u.scale_y = u.scale_z = v; }
			if (const auto hp = args.get("health")) u.health = static_cast<uint32_t>(std::stol(*hp));
			if (const auto mp = args.get("mana")) u.mana = static_cast<uint32_t>(std::stol(*mp));
			if (const auto lv = args.get("level")) u.level = static_cast<uint32_t>(std::stoul(*lv));
			if (const auto g = args.get("gold")) u.gold = static_cast<uint32_t>(std::stoul(*g));
		} catch (const std::exception&) {
			return error("set-unit: numeric option failed to parse");
		}
		if (const auto e = write_back()) return error(*e);
		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.boolean("dry_run", args.has_flag("dry-run"));
		o.raw("unit", placed_unit_to_json(u, target));
		ok = true;
		return o.dump();
	}

	// ---- region <-> island helpers (pathing + regions), no CASC needed --------
	if (args.command == "island-at" || args.command == "region-for-island" ||
		args.command == "regions-coverage") {
		hierarchy.map_directory = std::filesystem::absolute(*map_opt);

		const std::string move_str = args.get("move").value_or("foot");
		const auto move_opt = pathing::parse_move_type(move_str);
		if (!move_opt) return error("unknown --move '" + move_str + "' (expected foot|water|amphibious|fly)");

		std::string load_err;
		const auto grid_opt = pathing::load_grid(load_err);
		if (!grid_opt) return error(load_err);
		const pathing::Grid& grid = *grid_opt;
		if (!grid.has_offset) {
			return error("this command needs world coordinates from war3map.w3e, which could not be read");
		}
		const pathing::Components comps = pathing::connected_components(grid, *move_opt);

		// Island world bbox covering all its cells (cell edges, not centres).
		auto island_bbox_world = [&](const pathing::Component& c) {
			const float l = grid.offset_x + c.min_x * 32.f;
			const float r = grid.offset_x + (c.max_x + 1) * 32.f;
			const float b = grid.offset_y + c.min_y * 32.f;
			const float t = grid.offset_y + (c.max_y + 1) * 32.f;
			return std::array<float, 4>{ l, b, r, t };
		};
		auto find_component = [&](int id) -> const pathing::Component* {
			for (const auto& c : comps.components) if (c.id == id) return &c;
			return nullptr;
		};

		if (args.command == "island-at") {
			const auto x = args.get("x");
			const auto y = args.get("y");
			if (!x || !y) return error("island-at needs --x and --y (world coordinates)");
			int cx = 0, cy = 0;
			try { cx = grid.world_to_cell_x(std::stof(*x)); cy = grid.world_to_cell_y(std::stof(*y)); }
			catch (...) { return error("--x/--y must be numbers"); }
			const int snap_radius = 16;
			const auto snapped = pathing::nearest_passable(grid, *move_opt, cx, cy, snap_radius);
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("move", pathing::move_type_name(*move_opt));
			if (!snapped) {
				o.boolean("on_island", false);
				o.str("note", "no passable cell within snap radius — point is in impassable terrain for this move type");
				ok = true;
				return o.dump();
			}
			const int label = comps.labels[grid.index(snapped->first, snapped->second)];
			const pathing::Component* c = find_component(label);
			o.boolean("on_island", c != nullptr);
			if (c) {
				const auto bb = island_bbox_world(*c);
				o.number("island_id", static_cast<std::size_t>(c->id));
				o.number("island_cells", c->cell_count);
				o.raw("island_bbox_world", std::format("[{},{},{},{}]", bb[0], bb[1], bb[2], bb[3]));
			}
			ok = true;
			return o.dump();
		}

		if (args.command == "region-for-island") {
			const auto id_opt = args.get("island-id");
			if (!id_opt) return error("region-for-island needs --island-id K (see pathing-islands)");
			int island_id = 0;
			try { island_id = std::stoi(*id_opt); } catch (...) { return error("invalid --island-id: " + *id_opt); }
			const pathing::Component* c = find_component(island_id);
			if (!c) return error("no island with id " + *id_opt + " for move type " + move_str);

			float pad = 0.f;
			if (const auto p = args.get("padding")) { try { pad = std::stof(*p); } catch (...) {} }
			const auto bb = island_bbox_world(*c);

			RegionRecord r;
			r.name = args.get("name").value_or(std::format("Island_{}", island_id));
			r.left = bb[0] - pad;
			r.bottom = bb[1] - pad;
			r.right = bb[2] + pad;
			r.top = bb[3] + pad;

			std::string rerr;
			auto regions_opt = read_regions(rerr);
			std::vector<RegionRecord> regions = regions_opt ? std::move(*regions_opt) : std::vector<RegionRecord>{};
			int32_t max_cn = 0;
			for (const auto& e : regions) max_cn = std::max(max_cn, e.creation_number);
			r.creation_number = max_cn + 1;
			const std::size_t new_index = regions.size();
			regions.push_back(r);
			if (!args.has_flag("dry-run")) {
				if (!write_regions(regions, rerr)) return error(rerr);
			}

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.boolean("dry_run", args.has_flag("dry-run"));
			o.number("island_id", static_cast<std::size_t>(island_id));
			o.number("island_cells", c->cell_count);
			o.raw("added", region_to_json(r, new_index));
			ok = true;
			return o.dump();
		}

		// regions-coverage: which islands are / are not covered by an existing region.
		std::size_t min_cells = 50;
		if (const auto m = args.get("min-cells")) { try { min_cells = static_cast<std::size_t>(std::stoul(*m)); } catch (...) {} }

		std::string rerr;
		const auto regions_opt = read_regions(rerr);
		const std::vector<RegionRecord> regions = regions_opt ? *regions_opt : std::vector<RegionRecord>{};

		auto overlaps = [](const std::array<float, 4>& isl, const RegionRecord& reg) {
			// isl = [l,b,r,t]; region rect normalised on read.
			return !(reg.right < isl[0] || reg.left > isl[2] || reg.top < isl[1] || reg.bottom > isl[3]);
		};

		std::string arr = "[";
		std::size_t shown = 0, uncovered = 0;
		std::string uncovered_ids = "[";
		for (const auto& c : comps.components) {
			if (c.cell_count < min_cells) continue;
			const auto bb = island_bbox_world(c);
			std::string covering = "[";
			bool any = false;
			for (const auto& reg : regions) {
				if (overlaps(bb, reg)) {
					if (any) covering += ",";
					covering += jstr(reg.name);
					any = true;
				}
			}
			covering += "]";
			if (!any) {
				if (uncovered) uncovered_ids += ",";
				uncovered_ids += std::to_string(c.id);
				uncovered++;
			}
			if (shown) arr += ",";
			JsonObject isl;
			isl.number("island_id", static_cast<std::size_t>(c.id));
			isl.number("cells", c.cell_count);
			isl.raw("bbox_world", std::format("[{},{},{},{}]", bb[0], bb[1], bb[2], bb[3]));
			isl.boolean("covered", any);
			isl.raw("covering_regions", covering);
			arr += isl.dump();
			shown++;
		}
		arr += "]";
		uncovered_ids += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("move", pathing::move_type_name(*move_opt));
		o.number("islands_considered", shown);
		o.number("uncovered_count", uncovered);
		o.raw("uncovered_island_ids", uncovered_ids);
		o.raw("islands", arr);
		ok = true;
		return o.dump();
	}

	if (args.command == "describe-race") {
		const auto suffix_opt = resolve_suffix(args);
		if (!suffix_opt) {
			return error("missing required option: --suffix <editor suffix> (or --suffix-hex <hex> / --suffix-file <path>)");
		}

		std::vector<std::string> tokens;
		if (const auto token_opt = args.get("tokens")) {
			tokens = split_csv(*token_opt);
		}
		if (tokens.empty()) {
			tokens.push_back(*suffix_opt);
		}

		hierarchy.map_directory = std::filesystem::absolute(*map_opt);
		TriggerStrings ts;
		try {
			ts.load();
		} catch (const std::exception&) {
			// Map-only race analysis can still work without resolved trigger strings.
		}

		const slk::SLK* units_for_graph = nullptr;
		std::vector<std::string> bootstrap_warnings;

		std::string warcraft;
		if (const auto w = args.get("warcraft")) {
			warcraft = *w;
		} else if (!warcraft_fallback.empty()) {
			warcraft = warcraft_fallback;
		}

		if (!warcraft.empty()) {
			try {
				if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
					bootstrap_warnings.push_back("full object data unavailable: " + *err);
				} else {
					units_for_graph = &units_slk;
				}
			} catch (const std::exception& e) {
				bootstrap_warnings.push_back(std::string("full object data unavailable: ") + e.what());
			}
		} else {
			bootstrap_warnings.push_back("Warcraft III directory was not provided; using map-only race analysis");
		}

		const slk::SLK* abilities_for_graph = units_for_graph ? &abilities_slk : nullptr;
		const slk::SLK* upgrades_for_graph = units_for_graph ? &upgrade_slk : nullptr;
		RaceGraphAnalysis analysis = analyze_race_graph(std::filesystem::absolute(*map_opt), *suffix_opt, tokens, ts, units_for_graph, abilities_for_graph, upgrades_for_graph);
		analysis.warnings.insert(analysis.warnings.begin(), bootstrap_warnings.begin(), bootstrap_warnings.end());

		std::string nodes = "[";
		for (std::size_t i = 0; i < analysis.nodes.size(); ++i) {
			if (i) nodes += ",";
			const auto& node = analysis.nodes[i];
			JsonObject no;
			no.str("id", node.id);
			no.str("category", node.category);
			no.str("name", node.name);
			no.str("editor_suffix", node.editor_suffix);
			no.str("base_id", node.base_id);
			no.str("source", node.source);
			JsonObject fields;
			for (const auto& [key, value] : node.fields) {
				fields.str(key, value);
			}
			no.raw("fields", fields.dump());
			nodes += no.dump();
		}
		nodes += "]";

		std::string edges = "[";
		for (std::size_t i = 0; i < analysis.edges.size(); ++i) {
			if (i) edges += ",";
			const auto& edge = analysis.edges[i];
			JsonObject eo;
			eo.str("kind", edge.kind);
			eo.str("from", edge.from_id);
			eo.str("to", edge.to_id);
			eo.str("source", edge.source);
			eo.str("detail", edge.detail);
			eo.str("file", edge.file);
			eo.str("symbol", edge.symbol);
			edges += eo.dump();
		}
		edges += "]";

		std::string refs = "[";
		for (std::size_t i = 0; i < analysis.lua_references.size(); ++i) {
			if (i) refs += ",";
			const auto& ref = analysis.lua_references[i];
			JsonObject ro;
			ro.str("file", ref.file);
			ro.str("symbol", ref.symbol);
			ro.raw("rawcodes", string_array_json(ref.rawcodes));
			ro.raw("matched_ids", string_array_json(ref.matched_ids));
			ro.str("excerpt", ref.excerpt);
			refs += ro.dump();
		}
		refs += "]";

		JsonObject summary;
		summary.number("node_count", analysis.nodes.size());
		summary.number("edge_count", analysis.edges.size());
		summary.number("lua_reference_count", analysis.lua_references.size());
		summary.number("seed_count", analysis.seed_unit_ids.size());

		const RaceNodeIndex node_index = build_race_node_index(analysis);
		std::vector<std::string> start_units;
		std::vector<std::string> start_tech;
		std::set<std::string> builder_ids;
		for (const auto& edge : analysis.edges) {
			if (edge.kind == "lua-starts-with") {
				start_units.push_back(edge.to_id);
			} else if (edge.kind == "lua-start-tech") {
				start_tech.push_back(edge.to_id);
			} else if (edge.kind == "builds") {
				builder_ids.insert(edge.from_id);
			}
		}
		std::sort(start_units.begin(), start_units.end());
		start_units.erase(std::unique(start_units.begin(), start_units.end()), start_units.end());
		std::sort(start_tech.begin(), start_tech.end());
		start_tech.erase(std::unique(start_tech.begin(), start_tech.end()), start_tech.end());

		JsonObject agent_view;
		agent_view.raw("seed_units", race_node_ref_array_json(analysis.seed_unit_ids, node_index));
		agent_view.raw("start_units", race_node_ref_array_json(start_units, node_index));
		agent_view.raw("start_tech", race_node_ref_array_json(start_tech, node_index));
		agent_view.raw("builders", race_node_ref_array_json(std::vector<std::string>(builder_ids.begin(), builder_ids.end()), node_index));
		agent_view.raw("build_tree", grouped_race_edges_json(analysis.edges, {"builds", "lua-build-choice"}, node_index));
		agent_view.raw("train_tree", grouped_race_edges_json(analysis.edges, {"trains", "lua-trains"}, node_index));
		agent_view.raw("upgrade_tree", grouped_race_edges_json(analysis.edges, {"morphs-to", "lua-upgrades-to"}, node_index));
		agent_view.raw("research_tree", grouped_race_edges_json(analysis.edges, {"researches", "lua-researches"}, node_index));
		agent_view.raw("special_rules", special_rules_json(analysis.edges, node_index));
		agent_view.raw("lua_sections", lua_sections_json(analysis.lua_references));

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("suffix", *suffix_opt);
		o.raw("tokens", string_array_json(tokens));
		o.boolean("used_full_object_data", analysis.used_full_object_data);
		o.raw("warnings", string_array_json(analysis.warnings));
		o.raw("seed_unit_ids", string_array_json(analysis.seed_unit_ids));
		o.raw("summary", summary.dump());
		o.raw("agent_view", agent_view.dump());
		o.raw("nodes", nodes);
		o.raw("edges", edges);
		o.raw("lua_references", refs);
		ok = true;
		return o.dump();
	}

	if (args.command == "show-building") {
		const auto id_opt = args.get("id");
		if (!id_opt) {
			return error("missing required option: --id <building rawcode>");
		}
		const std::string building_id = *id_opt;

		hierarchy.map_directory = std::filesystem::absolute(*map_opt);
		TriggerStrings ts;
		try { ts.load(); } catch (const std::exception&) {}

		std::string warcraft;
		if (const auto w = args.get("warcraft")) {
			warcraft = *w;
		} else if (!warcraft_fallback.empty()) {
			warcraft = warcraft_fallback;
		}
		if (!warcraft.empty()) {
			try {
				if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
					// continue with map-only fallback
				} else {
					goto show_building_full;
				}
			} catch (const std::exception&) {}
		}
		// Fall through to map-only path
		goto show_building_map_only;

	show_building_full:
		{
			if (!units_slk.row_headers.contains(building_id)) {
				return error("building not found in unit data: " + building_id);
			}
			const bool is_bldg = units_slk.data<std::string>("isbldg", building_id) == "1";
			const bool is_hero = !units_slk.data<std::string>("primaryattribute", building_id).empty();
			std::string category = is_bldg ? "building" : (is_hero ? "hero" : "unit");

			std::string base_id = units_slk.data<std::string>("oldid", building_id);
			JsonObject fields;
			for (const auto& [col, col_idx] : units_slk.column_headers) {
				std::string v = units_slk.data<std::string>(col, building_id);
				if (!v.empty()) {
					fields.str(col, v);
				}
			}

			std::vector<std::string> trains = split_rawcode_list(units_slk.data<std::string>("trains", building_id));
			std::vector<std::string> ability_ids = split_rawcode_list(units_slk.data<std::string>("abillist", building_id));
			std::vector<std::string> hero_ability_ids = split_rawcode_list(units_slk.data<std::string>("heroabillist", building_id));
			ability_ids.insert(ability_ids.end(), hero_ability_ids.begin(), hero_ability_ids.end());
			std::sort(ability_ids.begin(), ability_ids.end());
			ability_ids.erase(std::unique(ability_ids.begin(), ability_ids.end()), ability_ids.end());

			std::vector<std::string> research_ids = split_rawcode_list(units_slk.data<std::string>("researches", building_id));
			for (auto& rid : split_rawcode_list(units_slk.data<std::string>("heroresearches", building_id))) {
				research_ids.push_back(rid);
			}
			std::sort(research_ids.begin(), research_ids.end());
			research_ids.erase(std::unique(research_ids.begin(), research_ids.end()), research_ids.end());

			std::vector<std::string> upgrades_to;
			for (const char* key : {"upgrade", "upgrades", "revive"}) {
				for (auto& id : split_rawcode_list(units_slk.data<std::string>(key, building_id))) {
					upgrades_to.push_back(id);
				}
			}
			std::sort(upgrades_to.begin(), upgrades_to.end());
			upgrades_to.erase(std::unique(upgrades_to.begin(), upgrades_to.end()), upgrades_to.end());

			auto train_names = string_array_json(trains);
			std::string ability_arr = "[";
			for (std::size_t i = 0; i < ability_ids.size(); ++i) {
				if (i) ability_arr += ",";
				const auto n = display_name(abilities_slk, ability_ids[i], ts);
				JsonObject ro;
				ro.str("id", ability_ids[i]);
				ro.str("name", n);
				ability_arr += ro.dump();
			}
			ability_arr += "]";

			std::string research_arr = "[";
			for (std::size_t i = 0; i < research_ids.size(); ++i) {
				if (i) research_arr += ",";
				const auto n = display_name(upgrade_slk, research_ids[i], ts);
				JsonObject ro;
				ro.str("id", research_ids[i]);
				ro.str("name", n);
				research_arr += ro.dump();
			}
			research_arr += "]";

			std::string upgrade_arr = "[";
			for (std::size_t i = 0; i < upgrades_to.size(); ++i) {
				if (i) upgrade_arr += ",";
				const auto n = display_name(units_slk, upgrades_to[i], ts);
				JsonObject uo;
				uo.str("id", upgrades_to[i]);
				uo.str("name", n);
				upgrade_arr += uo.dump();
			}
			upgrade_arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("id", building_id);
			o.str("name", display_name(units_slk, building_id, ts));
			o.str("category", category);
			o.str("base_id", base_id);
			o.raw("trains", train_names);
			o.raw("researches", research_arr);
			o.raw("abilities", ability_arr);
			o.raw("upgrades_to", upgrade_arr);
			o.boolean("used_full_data", true);
			ok = true;
			return o.dump();
		}

	show_building_map_only:
		{
			const fs::path meta_path = "data/overrides/units/UnitMetaData.slk";
			if (!fs::is_regular_file(meta_path)) {
				return error("local UnitMetaData.slk not found; cannot read .w3u without Warcraft III data");
			}
			slk::SLK meta_slk(meta_path, true);
			meta_slk.build_meta_map();

			if (!hierarchy.map_file_exists("war3map.w3u")) {
				return error("map does not contain war3map.w3u");
			}

			BinaryReader reader = hierarchy.map_file_read_or_throw("war3map.w3u", "show-building");
			const uint32_t version = reader.read<uint32_t>();

			std::unordered_map<std::string, std::string> name_map;
			std::unordered_map<std::string, std::string> oldid_map;
			std::unordered_map<std::string, std::string> isbldg_map;
			std::unordered_map<std::string, std::string> trains_map;
			std::unordered_map<std::string, std::string> abillist_map;
			std::unordered_map<std::string, std::string> researches_map;
			std::unordered_map<std::string, std::string> upgrade_map;

			auto parse_table = [&](bool custom) {
				const uint32_t objects = reader.read<uint32_t>();
				for (uint32_t i = 0; i < objects; ++i) {
					const std::string orig = reader.read_string(4);
					const std::string mod = reader.read_string(4);
					const std::string obj = custom ? mod : orig;
					if (version >= 3) { (void)reader.read<uint32_t>(); (void)reader.read<uint32_t>(); }
					const uint32_t mods = reader.read<uint32_t>();
					for (uint32_t j = 0; j < mods; ++j) {
						const std::string mod_id = reader.read_string(4);
						const uint32_t type = reader.read<uint32_t>();
						std::string col = to_lower(meta_slk.data<std::string>("field", mod_id));
						std::string v;
						switch (type) {
							case 0: v = std::to_string(reader.read<int>()); break;
							case 1: case 2: v = std::to_string(reader.read<float>()); break;
							case 3: v = reader.read_c_string(); break;
							default: v.clear(); break;
						}
						reader.advance(4);
						if (col == "name") name_map[obj] = resolve_trigger_string(v, ts);
						else if (col == "oldid") oldid_map[obj] = v;
						else if (col == "isbldg") isbldg_map[obj] = v;
						else if (col == "trains") trains_map[obj] = v;
						else if (col == "abillist" || col == "heroabillist") {
							if (auto it = abillist_map.find(obj); it != abillist_map.end())
								it->second += "," + v;
							else
								abillist_map[obj] = v;
						}
						else if (col == "researches" || col == "heroresearches") {
							if (auto it = researches_map.find(obj); it != researches_map.end())
								it->second += "," + v;
							else
								researches_map[obj] = v;
						}
						else if (col == "upgrade" || col == "upgrades" || col == "revive") upgrade_map[obj] = v;
					}
				}
			};

			parse_table(false);
			parse_table(true);

			if (!name_map.contains(building_id) && !oldid_map.contains(building_id)) {
				return error("building not found in war3map.w3u: " + building_id);
			}

			std::string name = name_map.contains(building_id) && !name_map.at(building_id).empty()
				? name_map.at(building_id) : fallback_name("unit", building_id);
			std::string base_id = oldid_map.contains(building_id) ? oldid_map.at(building_id) : "";
			std::string category = isbldg_map.contains(building_id) && isbldg_map.at(building_id) == "1" ? "building" : "unit";

			std::vector<std::string> trains = split_rawcode_list(trains_map.contains(building_id) ? trains_map.at(building_id) : "");
			std::vector<std::string> ability_ids = split_rawcode_list(abillist_map.contains(building_id) ? abillist_map.at(building_id) : "");
			std::vector<std::string> research_ids = split_rawcode_list(researches_map.contains(building_id) ? researches_map.at(building_id) : "");
			std::vector<std::string> upgrades_to = split_rawcode_list(upgrade_map.contains(building_id) ? upgrade_map.at(building_id) : "");

			std::set<std::string> unique_abilities(ability_ids.begin(), ability_ids.end());
			std::set<std::string> unique_researches(research_ids.begin(), research_ids.end());
			std::sort(trains.begin(), trains.end());
			trains.erase(std::unique(trains.begin(), trains.end()), trains.end());

			std::string ability_arr = "[";
			bool first_a = true;
			for (const auto& aid : unique_abilities) {
				if (!first_a) ability_arr += ",";
				first_a = false;
				JsonObject ro;
				ro.str("id", aid);
				ro.str("name", fallback_name("ability", aid));
				ability_arr += ro.dump();
			}
			ability_arr += "]";

			std::string research_arr = "[";
			bool first_r = true;
			for (const auto& rid : unique_researches) {
				if (!first_r) research_arr += ",";
				first_r = false;
				JsonObject ro;
				ro.str("id", rid);
				ro.str("name", fallback_name("upgrade", rid));
				research_arr += ro.dump();
			}
			research_arr += "]";

			std::string upgrade_arr = "[";
			bool first_u = true;
			for (const auto& uid : upgrades_to) {
				if (!first_u) upgrade_arr += ",";
				first_u = false;
				JsonObject uo;
				uo.str("id", uid);
				uo.str("name", fallback_name("unit", uid));
				upgrade_arr += uo.dump();
			}
			upgrade_arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("id", building_id);
			o.str("name", name);
			o.str("category", category);
			o.str("base_id", base_id);
			o.raw("trains", string_array_json(trains));
			o.raw("researches", research_arr);
			o.raw("abilities", ability_arr);
			o.raw("upgrades_to", upgrade_arr);
			o.boolean("used_full_data", false);
			ok = true;
			return o.dump();
		}
	}

	if (args.command == "list-race-objects") {
		const auto suffix_opt = resolve_suffix(args);
		if (!suffix_opt) {
			return error("missing required option: --suffix <editor suffix> (or --suffix-hex <hex> / --suffix-file <path>)");
		}
		const auto type_opt = args.get("type");
		std::string type_filter = type_opt.value_or("all");

		hierarchy.map_directory = std::filesystem::absolute(*map_opt);
		TriggerStrings ts;
		try { ts.load(); } catch (const std::exception&) {}

		std::string warcraft;
		std::vector<std::string> warnings;
		if (const auto w = args.get("warcraft")) {
			warcraft = *w;
		} else if (!warcraft_fallback.empty()) {
			warcraft = warcraft_fallback;
		}

		std::unordered_map<std::string, std::string> unit_names;
		std::unordered_map<std::string, std::string> unit_suffixes;
		std::unordered_map<std::string, std::string> unit_oldids;
		std::unordered_map<std::string, std::string> unit_isbldg;
		std::unordered_map<std::string, std::string> unit_trains;
		std::unordered_map<std::string, std::string> unit_abillist;
		std::unordered_map<std::string, std::string> unit_upgrade;
		std::unordered_map<std::string, std::string> unit_primary;

		if (!warcraft.empty()) {
			try {
				if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
					warnings.push_back("full object data unavailable: " + *err);
					goto list_race_map_only;
				}
				for (const auto& [id, idx] : units_slk.row_headers) {
					unit_names[id] = resolve_trigger_string(units_slk.data<std::string>("name", id), ts);
					unit_suffixes[id] = resolve_trigger_string(units_slk.data<std::string>("editorsuffix", id), ts);
					unit_oldids[id] = units_slk.data<std::string>("oldid", id);
					unit_isbldg[id] = units_slk.data<std::string>("isbldg", id);
					unit_trains[id] = units_slk.data<std::string>("trains", id);
					unit_abillist[id] = units_slk.data<std::string>("abillist", id);
					unit_upgrade[id] = units_slk.data<std::string>("upgrade", id);
					unit_primary[id] = units_slk.data<std::string>("primaryattribute", id);
				}
				goto list_race_output;
			} catch (const std::exception& e) {
				warnings.push_back(std::string("full object data unavailable: ") + e.what());
			}
		}

	list_race_map_only:
		{
			const fs::path meta_path = "data/overrides/units/UnitMetaData.slk";
			if (!fs::is_regular_file(meta_path)) {
				warnings.push_back("local UnitMetaData.slk not found; map-only fallback unavailable");
				goto list_race_output;
			}
			slk::SLK meta_slk(meta_path, true);
			meta_slk.build_meta_map();
			if (!hierarchy.map_file_exists("war3map.w3u")) {
				warnings.push_back("map does not contain war3map.w3u");
				goto list_race_output;
			}
			BinaryReader reader = hierarchy.map_file_read_or_throw("war3map.w3u", "list-race-objects");
			const uint32_t version = reader.read<uint32_t>();

			auto parse_table = [&](bool custom) {
				const uint32_t objects = reader.read<uint32_t>();
				for (uint32_t i = 0; i < objects; ++i) {
					const std::string orig = reader.read_string(4);
					const std::string mod = reader.read_string(4);
					const std::string obj = custom ? mod : orig;
					if (version >= 3) { (void)reader.read<uint32_t>(); (void)reader.read<uint32_t>(); }
					const uint32_t mods = reader.read<uint32_t>();
					for (uint32_t j = 0; j < mods; ++j) {
						const std::string mod_id = reader.read_string(4);
						const uint32_t type = reader.read<uint32_t>();
						std::string col = to_lower(meta_slk.data<std::string>("field", mod_id));
						std::string v;
						switch (type) {
							case 0: v = std::to_string(reader.read<int>()); break;
							case 1: case 2: v = std::to_string(reader.read<float>()); break;
							case 3: v = reader.read_c_string(); break;
							default: v.clear(); break;
						}
						reader.advance(4);
						if (col == "name") unit_names[obj] = resolve_trigger_string(v, ts);
						else if (col == "editorsuffix") unit_suffixes[obj] = resolve_trigger_string(v, ts);
						else if (col == "oldid") unit_oldids[obj] = v;
						else if (col == "isbldg") unit_isbldg[obj] = v;
						else if (col == "trains") unit_trains[obj] = v;
						else if (col == "abillist" || col == "heroabillist") {
							if (auto it = unit_abillist.find(obj); it != unit_abillist.end())
								it->second += "," + v;
							else
								unit_abillist[obj] = v;
						}
						else if (col == "upgrade" || col == "upgrades" || col == "revive") unit_upgrade[obj] = v;
						else if (col == "primaryattribute") unit_primary[obj] = v;
					}
				}
			};
			parse_table(false);
			parse_table(true);
		}

	list_race_output:
		{
			std::vector<std::string> rows;
			for (const auto& [id, suffix] : unit_suffixes) {
				if (to_lower_utf8(suffix).find(to_lower_utf8(*suffix_opt)) == std::string::npos) {
					continue;
				}
				const bool is_building = unit_isbldg.contains(id) && unit_isbldg.at(id) == "1";
				const bool is_hero = !is_building && unit_primary.contains(id) && !unit_primary.at(id).empty() && unit_primary.at(id) != "0";
				std::string cat = is_building ? "building" : (is_hero ? "hero" : "unit");
				if (type_filter != "all" && cat != type_filter) continue;

				std::string resolved_name = unit_names.contains(id) && !unit_names.at(id).empty()
					? unit_names.at(id) : fallback_name("unit", id);
				std::string display = std::format("{}  {:8}  {}", id, cat, resolved_name);

				if (is_building) {
					if (unit_trains.contains(id) && !unit_trains.at(id).empty()) {
						display += "  trains: " + unit_trains.at(id);
					}
					std::string abils = unit_abillist.contains(id) ? unit_abillist.at(id) : "";
					if (!abils.empty()) {
						display += "  researches: " + abils;
					}
					if (unit_upgrade.contains(id) && !unit_upgrade.at(id).empty()) {
						display += "  upgrades_to: " + unit_upgrade.at(id);
					}
				}

				JsonObject ro;
				ro.str("line", display);
				rows.push_back(ro.dump());
			}

			std::sort(rows.begin(), rows.end());

			std::string arr = "[";
			for (std::size_t i = 0; i < rows.size(); ++i) {
				if (i) arr += ",";
				arr += rows[i];
			}
			arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("suffix", *suffix_opt);
			o.number("count", rows.size());
			o.raw("objects", arr);
			if (!warnings.empty()) {
				o.raw("warnings", string_array_json(warnings));
			}
			ok = true;
			return o.dump();
		}
	}

	if (args.command == "list-all-races") {
		hierarchy.map_directory = std::filesystem::absolute(*map_opt);
		TriggerStrings ts;
		try { ts.load(); } catch (const std::exception&) {}

		std::string warcraft;
		std::vector<std::string> warnings;
		if (const auto w = args.get("warcraft")) {
			warcraft = *w;
		} else if (!warcraft_fallback.empty()) {
			warcraft = warcraft_fallback;
		}

		std::unordered_map<std::string, std::string> unit_suffixes;
		std::unordered_map<std::string, std::string> unit_isbldg;
		std::unordered_map<std::string, std::string> unit_name;
		std::unordered_map<std::string, std::string> unit_trains;
		std::unordered_map<std::string, std::string> unit_primary;
		std::unordered_map<std::string, std::string> unit_builds;

		if (!warcraft.empty()) {
			try {
				if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
					warnings.push_back("full object data unavailable: " + *err);
					goto list_races_map_only;
				}
				for (const auto& [id, idx] : units_slk.row_headers) {
					unit_suffixes[id] = resolve_trigger_string(units_slk.data<std::string>("editorsuffix", id), ts);
					unit_isbldg[id] = units_slk.data<std::string>("isbldg", id);
					unit_name[id] = resolve_trigger_string(units_slk.data<std::string>("name", id), ts);
					unit_trains[id] = units_slk.data<std::string>("trains", id);
					unit_primary[id] = units_slk.data<std::string>("primaryattribute", id);
					unit_builds[id] = units_slk.data<std::string>("builds", id);
				}
				goto list_races_output;
			} catch (const std::exception& e) {
				warnings.push_back(std::string("full object data unavailable: ") + e.what());
			}
		}

	list_races_map_only:
		{
			const fs::path meta_path = "data/overrides/units/UnitMetaData.slk";
			if (!fs::is_regular_file(meta_path)) {
				warnings.push_back("local UnitMetaData.slk not found");
				goto list_races_output;
			}
			slk::SLK meta_slk(meta_path, true);
			meta_slk.build_meta_map();
			if (!hierarchy.map_file_exists("war3map.w3u")) {
				warnings.push_back("map does not contain war3map.w3u");
				goto list_races_output;
			}
			BinaryReader reader = hierarchy.map_file_read_or_throw("war3map.w3u", "list-all-races");
			const uint32_t version = reader.read<uint32_t>();

			auto parse_table = [&](bool custom) {
				const uint32_t objects = reader.read<uint32_t>();
				for (uint32_t i = 0; i < objects; ++i) {
					const std::string orig = reader.read_string(4);
					const std::string mod = reader.read_string(4);
					const std::string obj = custom ? mod : orig;
					if (version >= 3) { (void)reader.read<uint32_t>(); (void)reader.read<uint32_t>(); }
					const uint32_t mods = reader.read<uint32_t>();
					for (uint32_t j = 0; j < mods; ++j) {
						const std::string mod_id = reader.read_string(4);
						const uint32_t type = reader.read<uint32_t>();
						std::string col = to_lower(meta_slk.data<std::string>("field", mod_id));
						std::string v;
						switch (type) {
							case 0: v = std::to_string(reader.read<int>()); break;
							case 1: case 2: v = std::to_string(reader.read<float>()); break;
							case 3: v = reader.read_c_string(); break;
							default: v.clear(); break;
						}
						reader.advance(4);
						if (col == "editorsuffix") unit_suffixes[obj] = resolve_trigger_string(v, ts);
						else if (col == "isbldg") unit_isbldg[obj] = v;
						else if (col == "name") unit_name[obj] = resolve_trigger_string(v, ts);
						else if (col == "trains") unit_trains[obj] = v;
						else if (col == "primaryattribute") unit_primary[obj] = v;
						else if (col == "builds") unit_builds[obj] = v;
					}
				}
			};
			parse_table(false);
			parse_table(true);
		}

	list_races_output:
		{
			std::map<std::string, std::vector<std::string>> race_units;
			for (const auto& [id, suffix] : unit_suffixes) {
				std::string s = suffix;
				while (s.starts_with("(")) s.erase(0, 1);
				while (s.ends_with(")")) s.pop_back();
				if (s.empty() || s.size() > 60) continue;
				if (s.size() < 3) continue;
				if (s.find("Level") != std::string::npos) continue;
				if (s.find(")(\'") != std::string::npos) continue;
				if (s.find(")(") != std::string::npos) continue;
				race_units[s].push_back(id);
			}

			std::string arr = "[";
			bool first_race = true;
			for (const auto& [race_suffix, ids] : race_units) {
				JsonObject ro;
				ro.str("suffix", race_suffix);
				std::string short_code;
				for (char c : race_suffix) {
					if (std::isalpha(static_cast<unsigned char>(c))) short_code.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
					if (short_code.size() >= 2) break;
				}
				if (short_code.empty()) short_code = "??";

				std::vector<std::string> workers;
				int building_count = 0;
				int unit_count = 0;
				int hero_count = 0;
				std::string tech;

				for (const auto& uid : ids) {
					bool is_bldg = unit_isbldg.contains(uid) && unit_isbldg.at(uid) == "1";
					bool has_builds = unit_builds.contains(uid) && !unit_builds.at(uid).empty();
					bool has_primary = unit_primary.contains(uid) && !unit_primary.at(uid).empty() && unit_primary.at(uid) != "0";
					bool is_hero = !is_bldg && has_primary;

					if (is_bldg) {
						building_count++;
					} else if (is_hero) {
						hero_count++;
					} else {
						unit_count++;
						if (has_builds) {
							std::string wn = unit_name.contains(uid) && !unit_name.at(uid).empty()
								? unit_name.at(uid) : fallback_name("unit", uid);
							workers.push_back(wn.empty() ? uid : std::format("{} {}", uid, wn));
						}
					}
				}

				if (building_count + unit_count + hero_count < 3) continue;
				if (building_count == 0) continue;
				if (unit_count == 0) continue;

				if (!first_race) arr += ",";
				first_race = false;

				ro.str("short", short_code);
				ro.number("buildings", building_count);
				ro.number("units", unit_count);
				ro.number("heroes", hero_count);
				ro.raw("workers", string_array_json(workers));
				arr += ro.dump();
			}
			arr += "]";

			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.number("race_count", race_units.size());
			o.raw("races", arr);
			if (!warnings.empty()) {
				o.raw("warnings", string_array_json(warnings));
			}
			ok = true;
			return o.dump();
		}
	}

	// ---- trace-unit ----
	if (args.command == "trace-unit") {
		const auto id_opt = args.get("id");
		if (!id_opt) return error("missing required option: --id <rawcode>");
		const std::string root_id = *id_opt;
		if (root_id.size() != 4) return error("--id must be a 4-character rawcode, got: " + root_id);

		const int max_depth = args.get("depth") ? std::stoi(*args.get("depth")) : -1;
		const std::string format = args.get("format").value_or("tree");
		if (format != "tree" && format != "flat" && format != "json")
			return error("--format must be tree, flat, or json");

		std::string warcraft;
		if (const auto w = args.get("warcraft")) warcraft = *w;
		else if (!warcraft_fallback.empty()) warcraft = warcraft_fallback;
		else return error("could not determine Warcraft III directory");

		TriggerStrings ts;
		if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts))
			return error(*err);

		if (!units_slk.row_headers.contains(root_id))
			return error("unit not found: " + root_id);

		// Precompute reverse lookups: who trains/builds/upgrades-to a given rawcode
		std::unordered_map<std::string, std::vector<std::string>> trained_by;
		std::unordered_map<std::string, std::vector<std::string>> built_by;
		std::unordered_map<std::string, std::vector<std::string>> upgraded_from;

		for (const auto& [row_id, idx] : units_slk.row_headers) {
			for (const auto& t : split_rawcode_list(units_slk.data<std::string>("trains", row_id)))
				trained_by[t].push_back(row_id);
			for (const auto& b : split_rawcode_list(units_slk.data<std::string>("builds", row_id)))
				built_by[b].push_back(row_id);
			for (const char* f : {"upgrade", "upgrades", "revive"}) {
				for (const auto& u : split_rawcode_list(units_slk.data<std::string>(f, row_id)))
					upgraded_from[u].push_back(row_id);
			}
		}

		struct TNode {
			std::string id, name, editorsuffix, requires_field;
			bool is_building = false, is_worker = false;
			std::vector<std::string> trains, builds, upgrades_to, researches, abillist;
			std::vector<TNode> children;
		};

		std::unordered_set<std::string> visited;

		std::function<TNode(const std::string&, int)> build_node;
		build_node = [&](const std::string& id, int depth) -> TNode {
			TNode n;
			n.id = id;
			n.name = display_name(units_slk, id, ts);
			n.editorsuffix = editor_suffix(units_slk, id, ts);
			n.is_building = units_slk.data<std::string>("isbldg", id) == "1";
			n.trains = split_rawcode_list(units_slk.data<std::string>("trains", id));
			n.builds = split_rawcode_list(units_slk.data<std::string>("builds", id));
			n.is_worker = !n.builds.empty();
			n.researches = split_rawcode_list(units_slk.data<std::string>("researches", id));
			n.requires_field = units_slk.data<std::string>("requires", id);

			for (const char* f : {"abillist", "heroabillist"}) {
				auto list = split_rawcode_list(units_slk.data<std::string>(f, id));
				n.abillist.insert(n.abillist.end(), list.begin(), list.end());
			}
			std::sort(n.abillist.begin(), n.abillist.end());
			n.abillist.erase(std::unique(n.abillist.begin(), n.abillist.end()), n.abillist.end());

			for (const char* f : {"upgrade", "upgrades", "revive"}) {
				auto list = split_rawcode_list(units_slk.data<std::string>(f, id));
				n.upgrades_to.insert(n.upgrades_to.end(), list.begin(), list.end());
			}
			std::sort(n.upgrades_to.begin(), n.upgrades_to.end());
			n.upgrades_to.erase(std::unique(n.upgrades_to.begin(), n.upgrades_to.end()), n.upgrades_to.end());

			if (max_depth >= 0 && depth >= max_depth) return n;
			if (!visited.insert(id).second) return n;

			std::vector<std::string> child_ids;
			auto push_children = [&](const std::vector<std::string>& ids) {
				for (const auto& c : ids)
					if (units_slk.row_headers.contains(c)) child_ids.push_back(c);
			};
			push_children(n.builds);
			push_children(n.trains);
			push_children(n.upgrades_to);
			if (auto it = trained_by.find(id); it != trained_by.end()) push_children(it->second);
			if (auto it = built_by.find(id); it != built_by.end()) push_children(it->second);
			if (auto it = upgraded_from.find(id); it != upgraded_from.end()) push_children(it->second);

			std::sort(child_ids.begin(), child_ids.end());
			child_ids.erase(std::unique(child_ids.begin(), child_ids.end()), child_ids.end());

			for (const auto& child : child_ids)
				n.children.push_back(build_node(child, depth + 1));

			return n;
		};

		TNode root = build_node(root_id, 0);

		// ---- JSON output ----
		if (format == "json") {
			std::function<JsonObject(const TNode&)> to_json;
			to_json = [&](const TNode& n) -> JsonObject {
				JsonObject obj;
				obj.str("id", n.id);
				obj.str("name", n.name);
				if (!n.editorsuffix.empty()) obj.str("editor_suffix", n.editorsuffix);
				obj.str("type", n.is_building ? "building" : (n.is_worker ? "worker" : "unit"));
				if (!n.trains.empty()) obj.raw("trains", string_array_json(n.trains));
				if (!n.builds.empty()) obj.raw("builds", string_array_json(n.builds));
				if (!n.upgrades_to.empty()) obj.raw("upgrades_to", string_array_json(n.upgrades_to));
				if (!n.researches.empty()) obj.raw("researches", string_array_json(n.researches));
				if (!n.abillist.empty()) obj.raw("abilities", string_array_json(n.abillist));
				if (!n.requires_field.empty()) obj.str("requires", n.requires_field);
				if (!n.children.empty()) {
					std::string arr = "[";
					for (size_t i = 0; i < n.children.size(); ++i) {
						if (i) arr += ",";
						arr += to_json(n.children[i]).dump();
					}
					arr += "]";
					obj.raw("children", arr);
				}
				return obj;
			};
			JsonObject o;
			o.boolean("ok", true);
			o.str("command", args.command);
			o.str("map", *map_opt);
			o.str("root_id", root_id);
			o.number("max_depth", max_depth);
			o.number("nodes", static_cast<int>(visited.size()) + 1);
			o.raw("tree", to_json(root).dump());
			ok = true;
			return o.dump();
		}

		// ---- text output (tree / flat) ----
		std::string output;

		auto type_label = [](const TNode& n) -> std::string {
			std::string t;
			if (n.is_building) t = "building";
			else if (n.is_worker) t = "worker";
			else t = "unit";
			if (!n.editorsuffix.empty()) t += ", " + n.editorsuffix;
			return t;
		};

		auto field_list = [](const std::vector<std::string>& v) -> std::string {
			if (v.empty()) return "(none)";
			std::string s;
			for (size_t i = 0; i < v.size(); ++i) {
				if (i) s += ", ";
				s += v[i];
			}
			return s;
		};

		auto node_header = [&](const TNode& n) -> std::string {
			std::string h = n.id;
			if (!n.name.empty()) h += " (" + n.name + ")";
			h += " [" + type_label(n) + "]";
			return h;
		};

		auto append_fields = [&](const TNode& n, const std::string& indent) {
			if (!n.trains.empty()) output += indent + "trains: " + field_list(n.trains) + "\n";
			if (!n.builds.empty()) output += indent + "builds: " + field_list(n.builds) + "\n";
			if (!n.upgrades_to.empty()) output += indent + "upgrades_to: " + field_list(n.upgrades_to) + "\n";
			if (!n.researches.empty()) output += indent + "researches: " + field_list(n.researches) + "\n";
			if (!n.abillist.empty()) output += indent + "abilities: " + field_list(n.abillist) + "\n";
			if (!n.requires_field.empty()) output += indent + "requires: " + n.requires_field + "\n";
		};

		if (format == "flat") {
			std::function<void(const TNode&, int)> flat_out;
			flat_out = [&](const TNode& n, int depth) {
				std::string indent(depth * 2, ' ');
				output += indent + node_header(n) + "\n";
				append_fields(n, indent + "  ");
				for (const auto& child : n.children)
					flat_out(child, depth + 1);
			};
			flat_out(root, 0);
		} else {
			// tree: ASCII tree with +-- / \-- connectors and continuation bars
			std::function<void(const TNode&, const std::string&)> tree_out;
			tree_out = [&](const TNode& n, const std::string& line_prefix) {
				output += line_prefix + node_header(n) + "\n";

				// Field indent: replace the last 4-char connector with a continuation bar
				std::string field_indent = line_prefix;
				if (field_indent.size() >= 4) {
					std::string_view tail(field_indent.c_str() + field_indent.size() - 4, 4);
					if (tail == std::string_view("+-- ")) field_indent.replace(field_indent.size() - 4, 4, "|   ");
					else if (tail == std::string_view("\\-- ")) field_indent.replace(field_indent.size() - 4, 4, "    ");
				} else {
					field_indent = "    ";
				}

				append_fields(n, field_indent);

				for (size_t i = 0; i < n.children.size(); ++i) {
					bool last = (i == n.children.size() - 1);
					std::string child_prefix = field_indent + (last ? "\\-- " : "+-- ");
					tree_out(n.children[i], child_prefix);
				}
			};
			tree_out(root, "");
		}

		std::cout << output << std::flush;

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("root_id", root_id);
		o.str("format", format);
		o.number("max_depth", max_depth);
		o.number("nodes", static_cast<int>(visited.size()) + 1);
		ok = true;
		return o.dump();
	}

	std::string warcraft;
	if (const auto w = args.get("warcraft")) {
		warcraft = *w;
	} else if (!warcraft_fallback.empty()) {
		warcraft = warcraft_fallback;
	} else {
		return error("could not determine Warcraft III directory; pass --warcraft <dir>");
	}

	TriggerStrings ts;
	try {
		if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
			return error(*err);
		}
	} catch (const std::exception& e) {
		return error(std::string("failed to load object data: ") + e.what());
	}

	const auto type_opt = args.get("type");
	if (!type_opt) {
		return error("missing required option: --type <unit|item|ability|doodad|destructible|upgrade|buff>");
	}
	const auto info = type_for(*type_opt);
	if (!info) {
		return error("unknown object type: " + *type_opt);
	}
	slk::SLK& slk = *info->slk;

	// ---- search-objects ----
	if (args.command == "search-objects") {
		const auto query_opt = args.get("query");
		const auto field_opt = args.get("field");
		const auto field_value_opt = args.get("field-value");

		if (!query_opt && !(field_opt && field_value_opt)) {
			return error("missing required option: --query <substring> or (--field <col> --field-value <substr>)");
		}
		if (field_opt && !field_value_opt) {
			return error("--field requires --field-value");
		}

		std::size_t limit = 50;
		if (const auto l = args.get("limit")) {
			limit = static_cast<std::size_t>(std::max(0, std::atoi(l->c_str())));
		}

		std::string query_lower;
		if (query_opt) {
			query_lower = to_lower_utf8(to_utf8(*query_opt));
		}
		std::string fval_lower;
		if (field_value_opt) {
			fval_lower = to_lower_utf8(to_utf8(*field_value_opt));
		}

		auto text_check = [&](const std::string& s) -> bool {
			return !s.empty() && to_lower_utf8(s).find(query_lower) != std::string::npos;
		};

		std::vector<std::string> matches;
		for (const auto& [id, index] : slk.row_headers) {
			if (query_opt) {
				if (!text_check(id) && !text_check(display_name(slk, id, ts))
					&& !text_check(editor_suffix(slk, id, ts))
					&& !text_check(slk.data<std::string>("comment(s)", id))) {
					continue;
				}
			}

			if (field_opt && field_value_opt) {
				std::string fval = to_lower_utf8(slk.data<std::string>(*field_opt, id));
				if (fval.find(fval_lower) == std::string::npos) {
					continue;
				}
			}

			const std::string name = display_name(slk, id, ts);
			JsonObject m;
			m.str("id", id);
			m.str("name", name);
			matches.push_back(m.dump());
			if (matches.size() >= limit) {
				break;
			}
		}
		std::string arr = "[";
		for (std::size_t i = 0; i < matches.size(); ++i) {
			if (i) arr += ",";
			arr += matches[i];
		}
		arr += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.str("query", *query_opt);
		o.number("count", matches.size());
		o.raw("matches", arr);
		ok = true;
		return o.dump();
	}

	// ---- get-object ----
	if (args.command == "get-object") {
		const auto id_opt = args.get("id");
		if (!id_opt) {
			return error("missing required option: --id <object id>");
		}
		if (!slk.row_headers.contains(*id_opt)) {
			return error("object not found: " + *id_opt + " (type " + *type_opt + ")");
		}
		const std::string& id = *id_opt;

		std::optional<std::vector<std::string>> filter;
		if (const auto f = args.get("fields")) {
			std::vector<std::string> cols;
			std::string cur;
			for (char c : *f) {
				if (c == ',') { if (!cur.empty()) cols.push_back(to_lower(cur)); cur.clear(); }
				else cur.push_back(c);
			}
			if (!cur.empty()) cols.push_back(to_lower(cur));
			filter = cols;
		}

		JsonObject fields;
		if (filter) {
			for (const auto& col : *filter) {
				fields.str(col, slk.data<std::string>(col, id));
			}
		} else {
			for (const auto& [col, index] : slk.column_headers) {
				std::string v = slk.data<std::string>(col, id);
				if (!v.empty()) {
					fields.str(col, v);
				}
			}
		}

		JsonObject overrides;
		if (auto it = slk.shadow_data.find(id); it != slk.shadow_data.end()) {
			for (const auto& [col, v] : it->second) {
				overrides.str(col, v);
			}
		}

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.str("id", id);
		o.str("name", display_name(slk, id, ts));
		o.raw("fields", fields.dump());
		o.raw("overrides", overrides.dump());
		ok = true;
		return o.dump();
	}

	// ---- set-field ----
	if (args.command == "set-field") {
		const auto id_opt = args.get("id");
		const auto field_opt = args.get("field");
		const auto value_opt = args.get("value");
		if (!id_opt) return error("missing required option: --id <object id>");
		if (!field_opt) return error("missing required option: --field <column>");
		if (!value_opt) return error("missing required option: --value <value>");
		if (!slk.row_headers.contains(*id_opt)) {
			return error("object not found: " + *id_opt + " (type " + *type_opt + ")");
		}
		const std::string field = to_lower(*field_opt);
		const std::string& id = *id_opt;

		const std::string old_value = slk.data<std::string>(field, id);
		if (old_value.starts_with("TRIGSTR")) {
			std::string key = old_value;
			ts.set_string(key, *value_opt);
			ts.save();
		} else {
			slk.set_shadow_data(field, id, *value_opt);
		}
		const std::string new_value = old_value.starts_with("TRIGSTR") ? *value_opt : slk.data<std::string>(field, id);

		// Persist the modification table back into the map folder.
		save_modification_file(info->mod_file, *info->slk, *info->meta, info->optional_ints, false);
		{
			JsonObject log;
			log.str("ts", changelog_ts());
			log.str("command", "set-field");
			log.str("type", *type_opt);
			log.str("id", id);
			log.str("field", field);
			log.str("old", old_value);
			log.str("new", new_value);
			append_changelog(*map_opt, log.dump());
		}

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.str("id", id);
		o.str("field", field);
		o.str("old_value", old_value);
		o.str("new_value", new_value);
		o.str("written", info->mod_file);
		ok = true;
		return o.dump();
	}

	// ---- copy-object ----
	if (args.command == "copy-object") {
		const auto source_opt = args.get("source");
		const auto new_id_opt = args.get("new-id");
		if (!source_opt) return error("missing required option: --source <rawcode>");
		if (!new_id_opt) return error("missing required option: --new-id <rawcode>");
		if (new_id_opt->size() != 4) return error("--new-id must be exactly 4 characters, got: " + *new_id_opt);
		if (!slk.base_data.contains(*source_opt) && !slk.shadow_data.contains(*source_opt)) {
			if (slk.row_headers.contains(*source_opt)) {
				return error("copy-object requires source to be a base-game object, not a custom object. Use get-object to check base_id.");
			}
			return error("source object not found in type " + *type_opt + ": " + *source_opt);
		}
		if (slk.row_headers.contains(*new_id_opt)) {
			return error("new-id already exists in type " + *type_opt + ": " + *new_id_opt);
		}

		const bool dry_run = args.has_flag("dry-run");
		if (!dry_run) {
			slk.copy_row(*source_opt, *new_id_opt, false);

			if (const auto name_opt = args.get("name")) {
				slk.set_shadow_data("name", *new_id_opt, *name_opt);
			}

			save_modification_file(info->mod_file, *info->slk, *info->meta, info->optional_ints, false);
			{
				JsonObject log;
				log.str("ts", changelog_ts());
				log.str("command", "copy-object");
				log.str("type", *type_opt);
				log.str("source", *source_opt);
				log.str("new_id", *new_id_opt);
				if (const auto name_opt2 = args.get("name")) log.str("name", *name_opt2);
				append_changelog(*map_opt, log.dump());
			}
		}

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.str("source", *source_opt);
		o.str("new_id", *new_id_opt);
		if (const auto name_opt3 = args.get("name")) o.str("name", *name_opt3);
		o.boolean("dry_run", dry_run);
		o.str("written", dry_run ? "" : info->mod_file);
		ok = true;
		return o.dump();
	}

	// ---- batch-edit ----
	if (args.command == "batch-edit") {
		const auto field_opt = args.get("field");
		const auto value_opt = args.get("value");
		if (!field_opt) return error("missing required option: --field <column>");
		if (!value_opt) return error("missing required option: --value <value>");

		std::vector<std::string> ids;
		if (const auto ids_opt = args.get("ids")) {
			ids = split_csv(*ids_opt);
		} else if (const auto file_opt = args.get("id-file")) {
			std::ifstream file(*file_opt, std::ios::binary);
			if (!file) return error("cannot open id-file: " + *file_opt);
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
				while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
				while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
				if (!line.empty()) ids.push_back(line);
			}
		}
		if (ids.empty()) {
			return error("missing required option: --ids <a,b,c> or --id-file <path>");
		}

		const std::string field = to_lower(*field_opt);
		const bool dry_run = args.has_flag("dry-run");

		std::vector<std::string> warnings;
		std::string changed_arr = "[";
		bool first = true;
		std::size_t count_changed = 0;

		for (const auto& id : ids) {
			if (!slk.row_headers.contains(id)) {
				warnings.push_back("id not found: " + id);
				continue;
			}
			const std::string old_value = slk.data<std::string>(field, id);
			if (old_value.starts_with("TRIGSTR")) {
				std::string key = old_value;
				ts.set_string(key, *value_opt);
			} else {
				slk.set_shadow_data(field, id, *value_opt);
			}
			const std::string new_value = old_value.starts_with("TRIGSTR") ? *value_opt : slk.data<std::string>(field, id);
			const bool written = (old_value != new_value);
			if (written) ++count_changed;

			if (!first) changed_arr += ",";
			first = false;
			JsonObject entry;
			entry.str("id", id);
			entry.str("old", old_value);
			entry.str("new", new_value);
			entry.boolean("written", written);
			changed_arr += entry.dump();
		}
		changed_arr += "]";

		if (!dry_run) {
			save_modification_file(info->mod_file, *info->slk, *info->meta, info->optional_ints, false);
			ts.save();
			{
				JsonObject log;
				log.str("ts", changelog_ts());
				log.str("command", "batch-edit");
				log.str("type", *type_opt);
				log.str("field", field);
				log.number("count_changed", count_changed);
				log.raw("changes", changed_arr);
				append_changelog(*map_opt, log.dump());
			}
		}

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.str("field", field);
		o.str("value", *value_opt);
		o.number("count_processed", ids.size());
		o.number("count_changed", count_changed);
		o.boolean("dry_run", dry_run);
		o.raw("warnings", string_array_json(warnings));
		o.raw("changed", changed_arr);
		ok = true;
		return o.dump();
	}

	// ---- get-objects-bulk ----
	if (args.command == "get-objects-bulk") {
		std::vector<std::string> ids;
		if (const auto ids_opt = args.get("ids")) {
			ids = split_csv(*ids_opt);
		} else if (const auto file_opt = args.get("id-file")) {
			std::ifstream file(*file_opt, std::ios::binary);
			if (!file) return error("cannot open id-file: " + *file_opt);
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
				while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
				while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
				if (line.empty() || line.starts_with('#')) continue;
				ids.push_back(line);
			}
		}
		if (ids.empty()) {
			return error("missing required option: --ids <a,b,c> or --id-file <path>");
		}

		std::optional<std::vector<std::string>> filter;
		if (const auto f = args.get("fields")) {
			filter = split_csv(*f);
			for (auto& col : *filter) col = to_lower(col);
		}

		std::vector<std::string> missing;
		std::string objects_arr = "[";
		bool first = true;
		for (const auto& id : ids) {
			if (!slk.row_headers.contains(id)) {
				missing.push_back(id);
				continue;
			}
			JsonObject fields;
			if (filter) {
				for (const auto& col : *filter) {
					fields.str(col, slk.data<std::string>(col, id));
				}
			} else {
				for (const auto& [col, col_idx] : slk.column_headers) {
					std::string v = slk.data<std::string>(col, id);
					if (!v.empty()) fields.str(col, v);
				}
			}
			if (!first) objects_arr += ",";
			first = false;
			JsonObject obj;
			obj.str("id", id);
			obj.str("name", display_name(slk, id, ts));
			obj.raw("fields", fields.dump());
			objects_arr += obj.dump();
		}
		objects_arr += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.number("count", ids.size() - missing.size());
		o.raw("missing", string_array_json(missing));
		o.raw("objects", objects_arr);
		ok = true;
		return o.dump();
	}

	// ---- dump-objects ----
	if (args.command == "dump-objects") {
		const std::string type_name = to_lower(*type_opt);
		std::vector<std::string> use_fields;
		if (const auto f = args.get("fields")) {
			use_fields = split_csv(*f);
			for (auto& col : use_fields) col = to_lower(col);
		} else {
			if (type_name == "unit") {
				use_fields = {"name", "editorsuffix", "goldcost", "lumbercost", "foodcost", "hitpoints",
					"damagebase", "damagenumberofdice", "damagesidesperdie", "defense", "buildtime",
					"attacks1enabled", "isbldg"};
			} else if (type_name == "item") {
				use_fields = {"name", "editorsuffix", "goldcost", "lumbercost", "level"};
			} else if (type_name == "ability") {
				use_fields = {"name", "editorsuffix", "mana", "cool", "levels", "race"};
			} else if (type_name == "upgrade") {
				use_fields = {"name", "editorsuffix", "goldcost", "lumbercost", "levels"};
			} else if (type_name == "doodad") {
				use_fields = {"name", "editorsuffix"};
			} else if (type_name == "destructible") {
				use_fields = {"name", "editorsuffix", "hitpoints"};
			} else if (type_name == "buff") {
				use_fields = {"name", "editorsuffix", "race"};
			}
		}

		const auto suffix_opt = resolve_suffix(args);

		std::size_t max_count = 1000;
		if (const auto m = args.get("max")) {
			max_count = static_cast<std::size_t>(std::max<std::size_t>(1, std::stoull(*m)));
		}

		std::string suffix_lower;
		if (suffix_opt) {
			suffix_lower = to_lower_utf8(*suffix_opt);
		}

		std::string objects_arr = "[";
		bool first = true;
		std::size_t count = 0;

		for (const auto& [id, idx] : slk.row_headers) {
			if (suffix_opt) {
				std::string es = editor_suffix(slk, id, ts);
				if (to_lower_utf8(es).find(suffix_lower) == std::string::npos) {
					continue;
				}
			}
			if (count >= max_count) break;

			JsonObject fields;
			for (const auto& col : use_fields) {
				if (col == "name" || col == "editorsuffix") continue;
				fields.str(col, slk.data<std::string>(col, id));
			}

			if (!first) objects_arr += ",";
			first = false;
			JsonObject obj;
			obj.str("id", id);
			obj.str("name", display_name(slk, id, ts));
			obj.str("editor_suffix", editor_suffix(slk, id, ts));
			obj.raw("fields", fields.dump());
			objects_arr += obj.dump();
			++count;
		}
		objects_arr += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.raw("fields_used", string_array_json(use_fields));
		if (suffix_opt) o.str("filtered_by_suffix", *suffix_opt);
		o.number("count", count);
		o.raw("objects", objects_arr);
		ok = true;
		return o.dump();
	}

	// ---- list-fields ----
	if (args.command == "list-fields") {
		std::unordered_map<std::string, std::string> col_display;
		for (const auto& [code, row_idx] : info->meta->row_headers) {
			std::string col = info->meta->data<std::string>("field", code);
			std::string display = info->meta->data<std::string>("displayname", code);
			if (!col.empty() && !display.empty()) {
				col_display[col] = display;
			}
		}

		const auto search_opt = args.get("search");
		std::string search_lower;
		if (search_opt) {
			search_lower = to_lower_utf8(to_utf8(*search_opt));
		}

		std::string arr = "[";
		bool first = true;
		std::size_t count = 0;

		for (const auto& [col, col_idx] : slk.column_headers) {
			if (search_opt) {
				std::string col_lower = to_lower_utf8(col);
				std::string disp_lower;
				if (auto it = col_display.find(col); it != col_display.end()) {
					disp_lower = to_lower_utf8(it->second);
				}
				if (col_lower.find(search_lower) == std::string::npos
					&& disp_lower.find(search_lower) == std::string::npos) {
					continue;
				}
			}

			if (!first) arr += ",";
			first = false;
			const std::string display = col_display.contains(col) ? col_display.at(col) : "";
			JsonObject f;
			f.str("column", col);
			f.str("display", display);
			arr += f.dump();
			++count;
		}
		arr += "]";

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("map", *map_opt);
		o.str("type", *type_opt);
		o.number("count", count);
		if (search_opt) o.str("search", *search_opt);
		o.raw("fields", arr);
		ok = true;
		return o.dump();
	}

	return error("unknown object-data command: " + args.command);
}

// safe-move: move an imported file inside the map and rewrite every detectable
// reference to it (object data, sounds, map info, map script), then regenerate
// war3map.imp. Pass --dry-run to preview without changing anything.
export std::string hivewe_safe_move_command(int argc, char* argv[], const std::string& warcraft_fallback, bool& ok) {
	const CliArgs args = parse(argc, argv);
	ok = false;

	auto error = [&](const std::string& msg) {
		JsonObject o;
		o.boolean("ok", false);
		o.str("command", args.command);
		o.str("error", msg);
		return o.dump();
	};

	const auto map_opt = args.get("map");
	const auto from_opt = args.get("from");
	const auto to_opt = args.get("to");
	if (!map_opt) {
		return error("missing required option: --map <map folder>");
	}
	if (!from_opt) {
		return error("missing required option: --from <relative path>");
	}
	if (!to_opt) {
		return error("missing required option: --to <relative path>");
	}
	if (!std::filesystem::is_directory(*map_opt)) {
		return error("map folder not found: " + *map_opt);
	}

	std::string warcraft;
	if (const auto w = args.get("warcraft")) {
		warcraft = *w;
	} else if (!warcraft_fallback.empty()) {
		warcraft = warcraft_fallback;
	} else {
		return error("could not determine Warcraft III directory; pass --warcraft <dir>");
	}

	TriggerStrings ts;
	try {
		if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"), ts)) {
			return error(*err);
		}
	} catch (const std::exception& e) {
		return error(std::string("failed to load object data: ") + e.what());
	}

	MapInfo info;
	try {
		info.load();
	} catch (const std::exception& e) {
		return error(std::string("failed to load war3map.w3i: ") + e.what());
	}

	Sounds sounds;
	if (hierarchy.map_file_exists("war3map.w3s")) {
		try {
			sounds.load();
		} catch (const std::exception&) {
			// A malformed w3s shouldn't block the move; the raw rewriter handles it.
		}
	}

	const bool dry_run = args.has_flag("dry-run");

	MoveReport rep;
	try {
		rep = safe_move_file(*from_opt, *to_opt, dry_run, info, sounds);
	} catch (const std::exception& e) {
		return error(std::string("safe-move failed: ") + e.what());
	}

	std::string refs = "[";
	for (std::size_t i = 0; i < rep.references.size(); ++i) {
		if (i) refs += ",";
		JsonObject r;
		r.str("location", rep.references[i].location);
		r.str("old_value", rep.references[i].old_value);
		r.str("new_value", rep.references[i].new_value);
		refs += r.dump();
	}
	refs += "]";

	std::string warns = "[";
	for (std::size_t i = 0; i < rep.warnings.size(); ++i) {
		if (i) warns += ",";
		warns += jstr(rep.warnings[i]);
	}
	warns += "]";

	JsonObject o;
	o.boolean("ok", rep.ok);
	o.str("command", args.command);
	o.str("map", *map_opt);
	o.str("from", rep.from);
	o.str("to", rep.to);
	o.boolean("dry_run", rep.dry_run);
	o.boolean("renamed", rep.renamed);
	o.number("reference_count", rep.reference_count());
	o.raw("references", refs);
	o.raw("warnings", warns);
	if (!rep.ok) {
		o.str("error", rep.error);
	}
	ok = rep.ok;
	return o.dump();
}
