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
import Utilities;

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

	if (args.command == "describe-race") {
		const auto suffix_opt = args.get("suffix");
		if (!suffix_opt) {
			return error("missing required option: --suffix <editor suffix>");
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

		RaceGraphAnalysis analysis = analyze_race_graph(std::filesystem::absolute(*map_opt), *suffix_opt, tokens, ts, units_for_graph);
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
