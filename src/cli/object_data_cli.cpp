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

// ---- bootstrap ------------------------------------------------------------

// Opens CASC and loads the base Warcraft object data. This path deliberately
// does not require a map folder, so a snapshot can be exported once and reused
// in environments that do not have Warcraft III installed.
std::optional<std::string> bootstrap_base(const std::string& warcraft, bool hd) {
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

	load_base_object_data([](std::string_view) {});
	return std::nullopt;
}

// Loads base data first, then overlays one unpacked map's modification tables.
std::optional<std::string> bootstrap(const std::string& warcraft, const std::string& map_dir, bool hd) {
	if (const auto err = bootstrap_base(warcraft, hd)) {
		return err;
	}

	hierarchy.map_directory = std::filesystem::absolute(map_dir);
	load_map_object_data();
	return std::nullopt;
}

// Stream one SLK table as deterministic JSON. Keeping ids and columns sorted
// makes snapshots stable in git and easy for agents/tools to diff.
std::size_t write_snapshot_type(std::ostream& out, const char* type_name, const slk::SLK& table) {
	std::vector<std::string> ids;
	ids.reserve(table.row_headers.size());
	for (const auto& [id, index] : table.row_headers) {
		(void)index;
		ids.push_back(id);
	}
	std::sort(ids.begin(), ids.end());

	std::vector<std::string> columns;
	columns.reserve(table.column_headers.size());
	for (const auto& [column, index] : table.column_headers) {
		(void)index;
		columns.push_back(column);
	}
	std::sort(columns.begin(), columns.end());

	out << jstr(type_name) << ":{\"objects\":{";
	for (std::size_t i = 0; i < ids.size(); ++i) {
		if (i) out << ",";
		const std::string& id = ids[i];
		out << jstr(id) << ":{\"name\":" << jstr(display_name(table, id)) << ",\"fields\":{";

		bool first_field = true;
		for (const auto& column : columns) {
			const std::string value = table.data<std::string>(column, id);
			if (value.empty()) {
				continue;
			}
			if (!first_field) out << ",";
			first_field = false;
			out << jstr(column) << ":" << jstr(value);
		}
		out << "}}";
	}
	out << "},\"count\":" << ids.size() << "}";
	return ids.size();
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

	// ---- dump-base-data ----
	// Export textual Warcraft object data once so CI/agents can inspect rawcodes,
	// names, stats, ability lists and model *paths* without copying any game assets.
	if (args.command == "dump-base-data") {
		const auto out_opt = args.get("out");
		if (!out_opt) {
			return error("missing required option: --out <snapshot.json>");
		}

		std::string warcraft;
		if (const auto w = args.get("warcraft")) {
			warcraft = *w;
		} else if (!warcraft_fallback.empty()) {
			warcraft = warcraft_fallback;
		} else {
			return error("could not determine Warcraft III directory; pass --warcraft <dir>");
		}

		try {
			if (const auto err = bootstrap_base(warcraft, args.has_flag("hd"))) {
				return error(*err);
			}
		} catch (const std::exception& e) {
			return error(std::string("failed to load base object data: ") + e.what());
		}

		std::vector<std::string> selected_types;
		if (const auto type_opt = args.get("type")) {
			if (!type_for(*type_opt)) {
				return error("unknown object type: " + *type_opt);
			}
			selected_types.push_back(to_lower(*type_opt));
		} else {
			for (const char* name : kTypeNames) selected_types.emplace_back(name);
		}

		const std::filesystem::path out_path = std::filesystem::absolute(*out_opt);
		std::error_code ec;
		if (out_path.has_parent_path()) {
			std::filesystem::create_directories(out_path.parent_path(), ec);
			if (ec) {
				return error("failed to create snapshot directory: " + ec.message());
			}
		}

		std::ofstream stream(out_path, std::ios::binary | std::ios::trunc);
		if (!stream) {
			return error("failed to open snapshot for writing: " + out_path.string());
		}

		JsonObject counts;
		std::size_t total = 0;
		stream << "{\"schema_version\":1,\"source\":\"warcraft-casc\",\"hd\":"
			   << (args.has_flag("hd") ? "true" : "false") << ",\"types\":{";
		for (std::size_t i = 0; i < selected_types.size(); ++i) {
			if (i) stream << ",";
			const auto info = type_for(selected_types[i]);
			if (!info) continue; // validated above; defensive only
			const std::size_t count = write_snapshot_type(stream, selected_types[i].c_str(), *info->slk);
			counts.number(selected_types[i], count);
			total += count;
		}
		stream << "}}";
		stream.flush();
		if (!stream) {
			return error("failed while writing snapshot: " + out_path.string());
		}

		JsonObject o;
		o.boolean("ok", true);
		o.str("command", args.command);
		o.str("out", out_path.string());
		o.boolean("hd", args.has_flag("hd"));
		o.number("objects", total);
		o.raw("counts", counts.dump());
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

	try {
		if (const auto err = bootstrap(warcraft, *map_opt, args.has_flag("hd"))) {
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
		const std::string query = to_lower(*query_opt);
		std::size_t limit = 50;
		if (const auto l = args.get("limit")) {
			limit = static_cast<std::size_t>(std::max(0, std::atoi(l->c_str())));
		}

		std::vector<std::string> matches;
		for (const auto& [id, index] : slk.row_headers) {
			const std::string name = display_name(slk, id);
			if (to_lower(id).find(query) != std::string::npos || to_lower(name).find(query) != std::string::npos) {
				JsonObject m;
				m.str("id", id);
				m.str("name", name);
				matches.push_back(m.dump());
				if (matches.size() >= limit) {
					break;
				}
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
