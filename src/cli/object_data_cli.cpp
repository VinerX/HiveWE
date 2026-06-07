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

			std::string name = name_map.contains(building_id) ? name_map.at(building_id) : "";
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
				ro.str("name", "");
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
				ro.str("name", "");
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
				uo.str("name", "");
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

				std::string display = std::format("{}  {:8}  {}", id, cat, unit_names.contains(id) ? unit_names.at(id) : "");

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
							workers.push_back(unit_name.contains(uid) ? std::format("{} {}", uid, unit_name.at(uid)) : uid);
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
		slk.set_shadow_data(field, id, *value_opt);
		const std::string new_value = slk.data<std::string>(field, id);

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
			slk.set_shadow_data(field, id, *value_opt);
			const std::string new_value = slk.data<std::string>(field, id);
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
