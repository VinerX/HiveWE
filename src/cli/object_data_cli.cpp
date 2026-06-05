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
import TriggerStrings;

namespace {

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

std::string display_name(const slk::SLK& slk, const std::string& id) {
	for (const char* col : {"name", "editorname", "bufftip"}) {
		if (slk.column_headers.contains(col)) {
			std::string n = slk.data<std::string>(col, id);
			if (!n.empty()) {
				return n;
			}
		}
	}
	return {};
}

bool is_valid_utf8(std::string_view sv) noexcept {
	for (std::size_t i = 0; i < sv.size();) {
		unsigned char c = static_cast<unsigned char>(sv[i]);
		std::size_t len;
		if (c < 0x80) {
			len = 1;
		} else if ((c & 0xE0) == 0xC0) {
			len = 2;
		} else if ((c & 0xF0) == 0xE0) {
			len = 3;
		} else if ((c & 0xF8) == 0xF0) {
			len = 4;
		} else {
			return false;
		}
		if (i + len > sv.size()) {
			return false;
		}
		for (std::size_t j = 1; j < len; ++j) {
			if ((static_cast<unsigned char>(sv[i + j]) & 0xC0) != 0x80) {
				return false;
			}
		}
		// reject overlong sequences
		if (len == 2) {
			unsigned int cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(sv[i + 1]) & 0x3F);
			if (cp < 0x80) return false;
		} else if (len == 3) {
			unsigned int cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(sv[i + 1]) & 0x3F) << 6)
				| (static_cast<unsigned char>(sv[i + 2]) & 0x3F);
			if (cp < 0x800) return false;
		} else if (len == 4) {
			unsigned int cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(sv[i + 1]) & 0x3F) << 12)
				| ((static_cast<unsigned char>(sv[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(sv[i + 3]) & 0x3F);
			if (cp < 0x10000) return false;
		}
		i += len;
	}
	return true;
}

std::string to_utf8(std::string_view sv) {
	if (sv.empty() || is_valid_utf8(sv)) {
		return std::string(sv);
	}
	// Not valid UTF-8 — assume Windows-1251 (Russian/localized maps).
	// CP1251 high bytes (0x80–0xFF) each map to a single Unicode codepoint
	// that always encodes as a 2-byte UTF-8 sequence.
	std::string out;
	for (unsigned char b : sv) {
		if (b < 0x80) {
			out.push_back(static_cast<char>(b));
		} else {
			// Convert CP1251 byte to Unicode codepoint, then to UTF-8.
			// Use a lookup table for CP1251 codepoints (RFC 1345 / Unicode mapping).
			static constexpr unsigned short cp1251_uni[128] = {
				0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021, // 80-87
				0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F, // 88-8F
				0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014, // 90-97
				0x0098,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F, // 98-9F
				0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7, // A0-A7
				0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407, // A8-AF
				0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7, // B0-B7
				0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457, // B8-BF
				0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417, // C0-C7
				0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F, // C8-CF
				0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427, // D0-D7
				0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F, // D8-DF
				0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437, // E0-E7
				0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F, // E8-EF
				0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447, // F0-F7
				0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F, // F8-FF
			};
			unsigned short u = cp1251_uni[b - 0x80];
			if (u < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (u >> 6)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xE0 | (u >> 12)));
				out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			}
		}
	}
	return out;
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
		if (!query_opt) {
			return error("missing required option: --query <substring>");
		}
		const std::string query = to_lower_utf8(to_utf8(*query_opt));
		std::size_t limit = 50;
		if (const auto l = args.get("limit")) {
			limit = static_cast<std::size_t>(std::max(0, std::atoi(l->c_str())));
		}

		std::vector<std::string> matches;
		auto check = [&](const std::string& s) -> bool {
			return !s.empty() && to_lower_utf8(s).find(query) != std::string::npos;
		};
		for (const auto& [id, index] : slk.row_headers) {
			if (!check(id) && !check(display_name(slk, id))
				&& !check(editor_suffix(slk, id, ts))
				&& !check(slk.data<std::string>("comment(s)", id))) {
				continue;
			}
			const std::string name = display_name(slk, id);
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
		o.str("name", display_name(slk, id));
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
		slk.set_shadow_data(field, id, *value_opt);
		const std::string new_value = slk.data<std::string>(field, id);

		// Persist the modification table back into the map folder.
		save_modification_file(info->mod_file, *info->slk, *info->meta, info->optional_ints, false);

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

	return error("unknown object-data command: " + args.command);
}
