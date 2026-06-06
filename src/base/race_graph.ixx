export module RaceGraph;

import std;
import BinaryReader;
import Hierarchy;
import SLK;
import TriggerStrings;
import Utilities;

namespace fs = std::filesystem;

export struct RaceGraphNode {
	std::string id;
	std::string category;
	std::string name;
	std::string editor_suffix;
	std::string base_id;
	std::string source;
	std::vector<std::pair<std::string, std::string>> fields;
};

export struct RaceGraphEdge {
	std::string kind;
	std::string from_id;
	std::string to_id;
	std::string source;
	std::string detail;
	std::string file;
	std::string symbol;
};

export struct RaceLuaReference {
	std::string file;
	std::string symbol;
	std::vector<std::string> rawcodes;
	std::vector<std::string> matched_ids;
	std::string excerpt;
};

export struct RaceGraphAnalysis {
	std::string suffix;
	std::vector<std::string> race_tokens;
	bool used_full_object_data = false;
	std::vector<std::string> warnings;
	std::vector<std::string> seed_unit_ids;
	std::vector<RaceGraphNode> nodes;
	std::vector<RaceGraphEdge> edges;
	std::vector<RaceLuaReference> lua_references;
};

namespace {

struct UnitRecord {
	std::string id;
	std::string base_id;
	std::string name;
	std::string editor_suffix;
	std::string source;
	bool custom = false;
	std::unordered_map<std::string, std::string> fields;
};

struct LuaSection {
	std::string file;
	std::string symbol;
	std::vector<std::string> lines;
	std::set<std::string> rawcodes;
};

std::string to_lower_ascii(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string to_lower_utf8(std::string_view sv) {
	std::string result;
	result.reserve(sv.size());
	for (std::size_t i = 0; i < sv.size();) {
		const unsigned char c = static_cast<unsigned char>(sv[i]);
		if (c < 0x80) {
			result.push_back(static_cast<char>(std::tolower(c)));
			++i;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < sv.size()) {
			const unsigned char c2 = static_cast<unsigned char>(sv[i + 1]);
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

std::vector<std::string> token_variants(const std::string& token) {
	std::vector<std::string> variants;
	const std::string lowered = to_lower_utf8(token);
	if (!lowered.empty()) {
		variants.push_back(lowered);
	}

	std::string spaced;
	spaced.reserve(token.size() + 4);
	for (std::size_t i = 0; i < token.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(token[i]);
		if (i > 0 && std::isupper(c) && std::islower(static_cast<unsigned char>(token[i - 1]))) {
			spaced.push_back(' ');
		}
		spaced.push_back(static_cast<char>(c));
	}

	const std::string spaced_lower = to_lower_utf8(spaced);
	if (!spaced_lower.empty() && spaced_lower != lowered) {
		variants.push_back(spaced_lower);
	}

	return variants;
}

bool contains_any_token(std::string_view text, const std::vector<std::string>& tokens) {
	const std::string lowered = to_lower_utf8(text);
	for (const auto& token : tokens) {
		for (const auto& variant : token_variants(token)) {
			if (!variant.empty() && lowered.find(variant) != std::string::npos) {
				return true;
			}
		}
	}
	return false;
}

std::string trim_copy(std::string s) {
	auto not_space = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
	s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
	return s;
}

// Resolves a raw object-data string field to a display-ready UTF-8 string:
// expands TRIGSTR references (RU maps store localized names as TRIGSTR refs)
// and transcodes any leftover CP1251 bytes to UTF-8 so the JSON the CLI emits
// is valid and names survive the round-trip to the agent.
std::string resolve_trigger_string(std::string raw, const TriggerStrings& ts) {
	if (raw.starts_with("TRIGSTR")) {
		const std::string_view resolved = ts.string(raw);
		if (!resolved.empty()) {
			return to_utf8(resolved);
		}
	}
	return to_utf8(raw);
}

bool is_interesting_unit_field(std::string_view key) {
	static const std::set<std::string, std::less<>> exact = {
		"name", "editorsuffix", "comment(s)", "oldid", "builds", "trains", "upgrades", "researches",
		"abillist", "heroabillist", "isbldg", "requires", "requiresamount", "dependencyor",
		"dependencyand", "upgrade", "revive", "sellunits", "sellitems", "makeitems"
	};
	if (exact.contains(key)) {
		return true;
	}
	return key.find("abil") != std::string_view::npos || key.find("depend") != std::string_view::npos;
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

std::vector<std::string> extract_fourccs(std::string_view line) {
	std::vector<std::string> result;
	for (std::size_t pos = 0; pos < line.size();) {
		const std::size_t start = line.find("FourCC(", pos);
		if (start == std::string_view::npos) {
			break;
		}
		const std::size_t quote = line.find_first_of("'\"", start + 7);
		if (quote == std::string_view::npos || quote + 5 > line.size()) {
			break;
		}
		result.emplace_back(line.substr(quote + 1, 4));
		pos = quote + 5;
	}
	return result;
}

std::string first_fourcc_or_empty(std::string_view line) {
	const auto all = extract_fourccs(line);
	return all.empty() ? std::string() : all.front();
}

std::string field_value(const UnitRecord& record, std::string_view key) {
	if (const auto it = record.fields.find(std::string(key)); it != record.fields.end()) {
		return it->second;
	}
	return {};
}

std::unordered_map<std::string, UnitRecord> load_full_unit_records(const slk::SLK& units, const TriggerStrings& ts) {
	std::unordered_map<std::string, UnitRecord> out;
	for (const auto& [id, index] : units.row_headers) {
		UnitRecord record;
		record.id = id;
		record.base_id = units.data<std::string>("oldid", id);
		record.custom = !record.base_id.empty();
		// Same fallback order as the GUI display name (display_name in
		// object_data_cli / the table model): name -> editorname -> bufftip.
		for (const char* col : {"name", "editorname", "bufftip"}) {
			record.name = resolve_trigger_string(units.data<std::string>(col, id), ts);
			if (!record.name.empty()) {
				break;
			}
		}
		record.editor_suffix = resolve_trigger_string(units.data<std::string>("editorsuffix", id), ts);
		record.source = "full-object-data";
		for (const auto& [col, col_index] : units.column_headers) {
			if (!is_interesting_unit_field(col)) {
				continue;
			}
			std::string value = resolve_trigger_string(units.data<std::string>(col, id), ts);
			if (!value.empty()) {
				record.fields[col] = value;
			}
		}
		out.emplace(id, std::move(record));
	}
	return out;
}

void load_map_unit_table(BinaryReader& reader, const uint32_t version, const slk::SLK& meta_slk, bool custom_table, std::unordered_map<std::string, UnitRecord>& out) {
	const uint32_t objects = reader.read<uint32_t>();
	for (uint32_t i = 0; i < objects; ++i) {
		const std::string original_id = reader.read_string(4);
		const std::string modified_id = reader.read_string(4);
		const std::string object_id = custom_table ? modified_id : original_id;
		UnitRecord& record = out[object_id];
		record.id = object_id;
		record.base_id = original_id;
		record.custom = custom_table;
		record.source = "map-w3u";

		if (version >= 3) {
			const uint32_t set_count = reader.read<uint32_t>();
			const uint32_t set_flags = reader.read<uint32_t>();
			(void)set_count;
			(void)set_flags;
		}

		const uint32_t modifications = reader.read<uint32_t>();
		for (uint32_t j = 0; j < modifications; ++j) {
			const std::string modification_id = reader.read_string(4);
			const uint32_t type = reader.read<uint32_t>();

			const std::string column_header = to_lower_ascii(meta_slk.data<std::string>("field", modification_id));

			std::string value;
			switch (type) {
				case 0: value = std::to_string(reader.read<int>()); break;
				case 1:
				case 2: value = std::to_string(reader.read<float>()); break;
				case 3: value = reader.read_c_string(); break;
				default: value.clear(); break;
			}
			reader.advance(4);

			if (!column_header.empty()) {
				record.fields[column_header] = value;
			}
		}
	}
}

std::unordered_map<std::string, UnitRecord> load_map_only_unit_records(const fs::path& map_dir, const TriggerStrings& ts, std::vector<std::string>& warnings) {
	std::unordered_map<std::string, UnitRecord> out;
	const fs::path meta_path = "data/overrides/units/UnitMetaData.slk";
	if (!fs::is_regular_file(meta_path)) {
		warnings.push_back("local UnitMetaData.slk not found; map-only unit fallback is unavailable");
		return out;
	}

	slk::SLK meta_slk(meta_path, true);
	meta_slk.build_meta_map();

	if (!hierarchy.map_file_exists("war3map.w3u")) {
		warnings.push_back("map does not contain war3map.w3u");
		return out;
	}

	BinaryReader reader = hierarchy.map_file_read_or_throw("war3map.w3u", "RaceGraph::load_map_only_unit_records");
	const uint32_t version = reader.read<uint32_t>();
	load_map_unit_table(reader, version, meta_slk, false, out);
	load_map_unit_table(reader, version, meta_slk, true, out);

	for (auto& [id, record] : out) {
		if (const auto it = record.fields.find("name"); it != record.fields.end()) {
			record.name = resolve_trigger_string(it->second, ts);
		}
		if (const auto it = record.fields.find("editorsuffix"); it != record.fields.end()) {
			record.editor_suffix = resolve_trigger_string(it->second, ts);
		}
	}

	return out;
}

std::vector<fs::path> collect_lua_files(const fs::path& map_dir) {
	std::vector<fs::path> out;
	const std::vector<fs::path> roots = {
		map_dir.parent_path() / "lua_rewrite" / "output",
		map_dir / "_lua" / "monolith_split",
		map_dir
	};
	for (const auto& root : roots) {
		if (!fs::exists(root)) {
			continue;
		}
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			if (entry.path().extension() == ".lua") {
				out.push_back(entry.path());
			}
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::vector<LuaSection> collect_lua_sections(const fs::path& map_dir) {
	std::vector<LuaSection> sections;
	for (const auto& file : collect_lua_files(map_dir)) {
		auto read = read_file(file);
		if (!read) {
			continue;
		}
		const std::string text(reinterpret_cast<const char*>(read->buffer.data()), read->buffer.size());
		std::istringstream stream(text);
		std::string line;

		LuaSection function_section;
		bool in_function = false;

		auto flush = [&](LuaSection& section, bool& active) {
			if (!active) {
				return;
			}
			if (!section.rawcodes.empty() || !section.lines.empty()) {
				sections.push_back(section);
			}
			section = LuaSection();
			active = false;
		};

		while (std::getline(stream, line)) {
			const std::string trimmed = trim_copy(line);
			if (trimmed.starts_with("function ")) {
				flush(function_section, in_function);
				std::string name = trimmed.substr(9);
				name = name.substr(0, name.find('('));
				in_function = true;
				function_section.file = file.lexically_normal().string();
				function_section.symbol = name;
			}

			for (const auto& rawcode : extract_fourccs(trimmed)) {
				if (in_function) {
					function_section.rawcodes.insert(rawcode);
				}
			}

			if (in_function) {
				function_section.lines.push_back(trimmed);
			}
		}

		flush(function_section, in_function);
	}
	return sections;
}

std::string section_base_key(std::string_view symbol) {
	std::string lowered = to_lower_utf8(symbol);
	if (lowered.starts_with("inittrig_")) {
		lowered.erase(0, std::string_view("inittrig_").size());
	} else if (lowered.starts_with("trig_")) {
		lowered.erase(0, std::string_view("trig_").size());
	}
	for (const std::string_view suffix : {"_actions", "_conditions"}) {
		if (lowered.ends_with(suffix)) {
			lowered.resize(lowered.size() - suffix.size());
			break;
		}
	}
	return lowered;
}

std::string parse_trigger_handle(std::string_view line) {
	const std::size_t pos = line.find("gg_trg_");
	if (pos == std::string_view::npos) {
		return {};
	}

	std::size_t end = pos + 7;
	while (end < line.size()) {
		const unsigned char c = static_cast<unsigned char>(line[end]);
		if (!std::isalnum(c) && c != '_') {
			break;
		}
		++end;
	}

	return to_lower_utf8(line.substr(pos + 7, end - (pos + 7)));
}

std::pair<std::string, int> parse_set_player_tech_max_allowed(std::string_view line) {
	const std::string id = first_fourcc_or_empty(line);
	if (id.empty()) {
		return {};
	}

	const std::size_t fourcc_pos = line.find("FourCC(");
	if (fourcc_pos == std::string_view::npos) {
		return {};
	}
	const std::size_t fourcc_end = line.find(')', fourcc_pos);
	if (fourcc_end == std::string_view::npos) {
		return {};
	}
	const std::size_t first_comma = line.find(',', fourcc_end);
	if (first_comma == std::string_view::npos) {
		return {};
	}
	const std::size_t second_comma = line.find(',', first_comma + 1);
	if (second_comma == std::string_view::npos) {
		return {};
	}

	std::string value = trim_copy(std::string(line.substr(first_comma + 1, second_comma - first_comma - 1)));
	value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
	if (value.empty()) {
		return {};
	}

	return {id, std::atoi(value.c_str())};
}

std::vector<LuaSection> select_relevant_lua_sections(const std::vector<LuaSection>& sections, const std::vector<std::string>& tokens) {
	std::unordered_multimap<std::string, std::size_t> sections_by_base;
	for (std::size_t i = 0; i < sections.size(); ++i) {
		sections_by_base.emplace(section_base_key(sections[i].symbol), i);
	}

	std::set<std::size_t> relevant_indices;
	std::deque<std::size_t> queue;
	auto mark_relevant = [&](std::size_t index) {
		if (relevant_indices.insert(index).second) {
			queue.push_back(index);
		}
	};

	for (std::size_t i = 0; i < sections.size(); ++i) {
		if (contains_any_token(sections[i].symbol, tokens)) {
			mark_relevant(i);
		}
	}

	while (!queue.empty()) {
		const LuaSection& section = sections[queue.front()];
		queue.pop_front();

		for (const auto& line : section.lines) {
			if (trim_copy(line).starts_with("--")) {
				continue;
			}
			if (line.find("EnableTrigger(") == std::string::npos &&
				line.find("ConditionalTriggerExecute(") == std::string::npos &&
				line.find("TriggerExecute(") == std::string::npos) {
				continue;
			}

			const std::string handle = parse_trigger_handle(line);
			if (handle.empty()) {
				continue;
			}

			const auto [begin, end] = sections_by_base.equal_range(handle);
			for (auto it = begin; it != end; ++it) {
				mark_relevant(it->second);
			}
		}
	}

	std::vector<LuaSection> relevant;
	for (const std::size_t index : relevant_indices) {
		relevant.push_back(sections[index]);
	}
	return relevant;
}

bool looks_like_upgrade_id(const std::string& id) {
	return id.size() == 4 && !id.empty() && id.front() == 'R';
}

bool looks_like_ability_id(const std::string& id) {
	return id.size() == 4 && !id.empty() && (id.front() == 'A' || id.front() == 'a');
}

bool is_tree_seed_symbol(const std::string& symbol_lower) {
	return symbol_lower.find("start") != std::string::npos ||
		symbol_lower.find("join_") != std::string::npos ||
		symbol_lower.find("choosebuildings") != std::string::npos ||
		symbol_lower.find("pereborbuildings2") != std::string::npos ||
		symbol_lower.find("strateg_") != std::string::npos;
}

bool is_race_init_symbol(const std::string& symbol_lower) {
	return symbol_lower.find("trig_race_") != std::string::npos && symbol_lower.find("_actions") != std::string::npos;
}

std::set<std::string> extract_section_seed_ids(const LuaSection& section) {
	std::set<std::string> ids;
	const std::string symbol_lower = to_lower_utf8(section.symbol);

	if (symbol_lower.find("start") != std::string::npos) {
		for (const auto& line : section.lines) {
			if (line.find("CreateNUnitsAtLoc(") == std::string::npos) {
				continue;
			}
			for (const auto& id : extract_fourccs(line)) {
				ids.insert(id);
			}
		}
	}

	if (symbol_lower.find("choosebuildings") != std::string::npos) {
		for (const auto& line : section.lines) {
			if (line.find("CheckAndAddBuilding(") == std::string::npos) {
				continue;
			}
			const std::string id = first_fourcc_or_empty(line);
			if (!id.empty()) {
				ids.insert(id);
			}
		}
	}

	if (symbol_lower.find("pereborbuildings2") != std::string::npos) {
		std::string current_building;
		for (const auto& line : section.lines) {
			if ((line.find("if id == FourCC(") != std::string::npos || line.find("elseif id == FourCC(") != std::string::npos)) {
				current_building = first_fourcc_or_empty(line);
				if (!current_building.empty()) {
					ids.insert(current_building);
				}
				continue;
			}
			if (line.find("IssueImmediateOrderById(") != std::string::npos) {
				const std::string id = first_fourcc_or_empty(line);
				if (!id.empty()) {
					ids.insert(id);
				}
			}
		}
	}

	if (symbol_lower.find("strateg_") != std::string::npos) {
		for (const auto& line : section.lines) {
			if (line.find("MakeGradeCheckCap(") != std::string::npos || line.find("BuildT(") != std::string::npos) {
				for (const auto& id : extract_fourccs(line)) {
					ids.insert(id);
				}
			}
		}
	}

	if (is_race_init_symbol(symbol_lower)) {
		for (const auto& line : section.lines) {
			if (line.find("CreateNUnitsAtLoc(") != std::string::npos) {
				for (const auto& id : extract_fourccs(line)) {
					ids.insert(id);
				}
			}
		}
	}

	return ids;
}

void upsert_node(
	std::unordered_map<std::string, RaceGraphNode>& nodes,
	const UnitRecord* record,
	const std::string& id,
	const std::string& category,
	const std::string& source,
	const std::unordered_map<std::string, std::string>* ability_names = nullptr,
	const std::unordered_map<std::string, std::string>* upgrade_names = nullptr
) {
	RaceGraphNode& node = nodes[id];
	node.id = id;
	if (!category.empty()) {
		node.category = category;
	}
	if (!source.empty() && node.source.empty()) {
		node.source = source;
	}
	if (record) {
		node.name = record->name;
		node.editor_suffix = record->editor_suffix;
		node.base_id = record->base_id;
		node.source = record->source;
		node.category = field_value(*record, "isbldg") == "1" ? "building" : "unit";
		node.fields.clear();
		for (const auto& [key, value] : record->fields) {
			if (is_interesting_unit_field(key) && !value.empty()) {
				node.fields.emplace_back(key, value);
			}
		}
		std::sort(node.fields.begin(), node.fields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	}
	if (node.name.empty() && !record) {
		if (node.category == "ability" && ability_names) {
			if (auto it = ability_names->find(id); it != ability_names->end()) {
				node.name = it->second;
			}
		} else if (node.category == "upgrade" && upgrade_names) {
			if (auto it = upgrade_names->find(id); it != upgrade_names->end()) {
				node.name = it->second;
			}
		}
	}
}

void add_edge(
	std::vector<RaceGraphEdge>& edges,
	std::set<std::tuple<std::string, std::string, std::string, std::string>>& seen,
	std::string kind,
	std::string from_id,
	std::string to_id,
	std::string source,
	std::string detail = {},
	std::string file = {},
	std::string symbol = {}
) {
	const auto key = std::make_tuple(kind, from_id, to_id, detail);
	if (!seen.insert(key).second) {
		return;
	}
	edges.push_back(RaceGraphEdge{
		.kind = std::move(kind),
		.from_id = std::move(from_id),
		.to_id = std::move(to_id),
		.source = std::move(source),
		.detail = std::move(detail),
		.file = std::move(file),
		.symbol = std::move(symbol),
	});
}

void collect_object_edges(
	const std::unordered_map<std::string, UnitRecord>& records,
	std::unordered_map<std::string, RaceGraphNode>& nodes,
	std::vector<RaceGraphEdge>& edges,
	const std::vector<std::string>& start_ids,
	const std::unordered_map<std::string, std::string>* ability_names = nullptr,
	const std::unordered_map<std::string, std::string>* upgrade_names = nullptr
) {
	std::set<std::tuple<std::string, std::string, std::string, std::string>> seen_edges;
	std::set<std::string> visited;
	std::deque<std::string> queue(start_ids.begin(), start_ids.end());

	auto enqueue = [&](const std::string& id) {
		if (!id.empty() && !visited.contains(id)) {
			queue.push_back(id);
		}
	};

	while (!queue.empty()) {
		const std::string id = queue.front();
		queue.pop_front();
		if (!visited.insert(id).second) {
			continue;
		}

		const auto it = records.find(id);
		if (it == records.end()) {
			continue;
		}
		const UnitRecord& record = it->second;
		upsert_node(nodes, &record, id, {}, {}, ability_names, upgrade_names);

		const auto build_targets = split_rawcode_list(field_value(record, "builds"));
		for (const auto& target : build_targets) {
			upsert_node(nodes, records.contains(target) ? &records.at(target) : nullptr, target, records.contains(target) ? "" : "building", "object-data", ability_names, upgrade_names);
			add_edge(edges, seen_edges, "builds", id, target, "object-data", "builds");
			enqueue(target);
		}

		const auto train_targets = split_rawcode_list(field_value(record, "trains"));
		for (const auto& target : train_targets) {
			upsert_node(nodes, records.contains(target) ? &records.at(target) : nullptr, target, records.contains(target) ? "" : "unit", "object-data", ability_names, upgrade_names);
			add_edge(edges, seen_edges, "trains", id, target, "object-data", "trains");
			enqueue(target);
		}

		for (const std::string key : {"upgrades", "researches"}) {
			for (const auto& target : split_rawcode_list(field_value(record, key))) {
				upsert_node(nodes, nullptr, target, looks_like_upgrade_id(target) ? "upgrade" : "unknown", "object-data", ability_names, upgrade_names);
				add_edge(edges, seen_edges, "researches", id, target, "object-data", key);
			}
		}

		for (const std::string key : {"abillist", "heroabillist"}) {
			for (const auto& target : split_rawcode_list(field_value(record, key))) {
				upsert_node(nodes, nullptr, target, looks_like_ability_id(target) ? "ability" : "unknown", "object-data", ability_names, upgrade_names);
				add_edge(edges, seen_edges, "has-ability", id, target, "object-data", key);
			}
		}

		for (const std::string key : {"upgrade", "revive"}) {
			for (const auto& target : split_rawcode_list(field_value(record, key))) {
				upsert_node(nodes, records.contains(target) ? &records.at(target) : nullptr, target, records.contains(target) ? "" : "unit", "object-data", ability_names, upgrade_names);
				add_edge(edges, seen_edges, "morphs-to", id, target, "object-data", key);
				enqueue(target);
			}
		}
	}
}

void collect_lua_edges(
	const std::vector<LuaSection>& sections,
	const std::unordered_map<std::string, UnitRecord>& records,
	std::unordered_map<std::string, RaceGraphNode>& nodes,
	std::vector<RaceGraphEdge>& edges,
	std::vector<RaceLuaReference>& refs,
	const std::vector<std::string>& tokens,
	const std::string& race_id,
	const std::unordered_map<std::string, std::string>* ability_names = nullptr,
	const std::unordered_map<std::string, std::string>* upgrade_names = nullptr
) {
	std::set<std::tuple<std::string, std::string, std::string, std::string>> seen_edges;
	std::unordered_map<std::string, std::string> trigger_ability_by_base;
	for (const auto& section : sections) {
		const std::string symbol_lower = to_lower_utf8(section.symbol);
		if (!symbol_lower.ends_with("_conditions")) {
			continue;
		}
		for (const auto& line : section.lines) {
			if (line.find("GetSpellAbilityId()") == std::string::npos) {
				continue;
			}
			const std::string ability = first_fourcc_or_empty(line);
			if (!ability.empty()) {
				trigger_ability_by_base[section_base_key(section.symbol)] = ability;
			}
			break;
		}
	}

	upsert_node(nodes, nullptr, race_id, "race", "derived");

	for (const auto& section : sections) {
		const std::size_t edge_count_before = edges.size();
		RaceLuaReference ref;
		ref.file = section.file;
		ref.symbol = section.symbol;
		ref.rawcodes.assign(section.rawcodes.begin(), section.rawcodes.end());
		for (const auto& id : ref.rawcodes) {
			if (records.contains(id)) {
				ref.matched_ids.push_back(id);
				upsert_node(nodes, &records.at(id), id, {}, {}, ability_names, upgrade_names);
			} else {
				upsert_node(nodes, nullptr, id, looks_like_upgrade_id(id) ? "upgrade" : (looks_like_ability_id(id) ? "ability" : "unknown"), "lua", ability_names, upgrade_names);
			}
		}

		std::size_t excerpt_lines = 0;
		for (const auto& line : section.lines) {
			if (line.empty()) {
				continue;
			}
			if (!ref.excerpt.empty()) {
				ref.excerpt += "\n";
			}
			ref.excerpt += line;
			if (++excerpt_lines >= 24) {
				break;
			}
		}
		const std::string symbol_lower = to_lower_utf8(section.symbol);
		const std::string symbol_base = section_base_key(section.symbol);
		if (symbol_lower.find("start") != std::string::npos && contains_any_token(section.symbol, tokens)) {
			for (const auto& line : section.lines) {
				if (line.find("CreateNUnitsAtLoc(") == std::string::npos) {
					continue;
				}
				for (const auto& id : extract_fourccs(line)) {
					add_edge(edges, seen_edges, "lua-starts-with", race_id, id, "lua", "CreateNUnitsAtLoc", section.file, section.symbol);
				}
			}
		}

		if (is_race_init_symbol(symbol_lower) && contains_any_token(section.symbol, tokens)) {
			for (const auto& line : section.lines) {
				if (line.find("CreateNUnitsAtLoc(") != std::string::npos) {
					for (const auto& id : extract_fourccs(line)) {
						add_edge(edges, seen_edges, "lua-starts-with", race_id, id, "lua", "CreateNUnitsAtLoc", section.file, section.symbol);
					}
				} else if (line.find("SetPlayerTechResearchedSwap(") != std::string::npos || line.find("SetPlayerTechResearched(") != std::string::npos) {
					const std::string id = first_fourcc_or_empty(line);
					if (!id.empty()) {
						add_edge(edges, seen_edges, "lua-start-tech", race_id, id, "lua", "SetPlayerTechResearched", section.file, section.symbol);
					}
				}
			}
		}

		if (const auto it = trigger_ability_by_base.find(symbol_base); it != trigger_ability_by_base.end() && symbol_lower.ends_with("_actions")) {
			const std::string& trigger_ability = it->second;
			for (const auto& line : section.lines) {
				if (line.find("SetPlayerTechMaxAllowedSwap(") != std::string::npos || line.find("SetPlayerTechMaxAllowed(") != std::string::npos) {
					const auto [target, max_allowed] = parse_set_player_tech_max_allowed(line);
					if (!target.empty()) {
						add_edge(
							edges,
							seen_edges,
							max_allowed <= 0 ? "lua-disables-tech" : "lua-enables-tech",
							trigger_ability,
							target,
							"lua",
							"SetPlayerTechMaxAllowed",
							section.file,
							section.symbol
						);
					}
				} else if (line.find("SetPlayerAbilityAvailableBJ(") != std::string::npos && line.find("false") != std::string::npos) {
					const std::string target = first_fourcc_or_empty(line);
					if (!target.empty()) {
						add_edge(edges, seen_edges, "lua-hides-ability", trigger_ability, target, "lua", "SetPlayerAbilityAvailableBJ", section.file, section.symbol);
					}
				} else if (line.find("SetPlayerTechResearchedSwap(") != std::string::npos || line.find("SetPlayerTechResearched(") != std::string::npos) {
					const std::string target = first_fourcc_or_empty(line);
					if (!target.empty()) {
						add_edge(edges, seen_edges, "lua-auto-research", trigger_ability, target, "lua", "SetPlayerTechResearched", section.file, section.symbol);
					}
				}
			}
		}

		if (symbol_lower.find("choosebuildings") != std::string::npos) {
			for (const auto& line : section.lines) {
				if (line.find("CheckAndAddBuilding(") == std::string::npos) {
					continue;
				}
				const std::string id = first_fourcc_or_empty(line);
				if (!id.empty()) {
					add_edge(edges, seen_edges, "lua-build-choice", race_id, id, "lua", "CheckAndAddBuilding", section.file, section.symbol);
				}
			}
		}

		if (symbol_lower.find("pereborbuildings2") != std::string::npos) {
			std::string current_building;
			for (const auto& line : section.lines) {
				if ((line.find("if id == FourCC(") != std::string::npos || line.find("elseif id == FourCC(") != std::string::npos)) {
					current_building = first_fourcc_or_empty(line);
					continue;
				}
				if (line.find("IssueImmediateOrderById(") != std::string::npos && !current_building.empty()) {
					const std::string target = first_fourcc_or_empty(line);
					if (!target.empty()) {
						add_edge(edges, seen_edges, "lua-trains", current_building, target, "lua", "IssueImmediateOrderById", section.file, section.symbol);
					}
				}
			}
		}

		if (symbol_lower.find("strateg_") != std::string::npos) {
			for (const auto& line : section.lines) {
				if (line.find("MakeGradeCheckCap(") != std::string::npos) {
					const auto ids = extract_fourccs(line);
					if (ids.size() >= 2) {
						add_edge(edges, seen_edges, "lua-researches", ids[0], ids[1], "lua", "MakeGradeCheckCap", section.file, section.symbol);
					}
				} else if (line.find("BuildT(") != std::string::npos) {
					const auto ids = extract_fourccs(line);
					if (ids.size() >= 2) {
						add_edge(edges, seen_edges, "lua-upgrades-to", ids[0], ids[1], "lua", "BuildT", section.file, section.symbol);
					}
				}
			}
		}

		if (contains_any_token(section.symbol, tokens) && symbol_lower.find("blood elves") != std::string::npos) {
			std::string current_rule;
			for (const auto& line : section.lines) {
				if (line.find("MakeBloodElfPathTrig(") != std::string::npos || line.find("MakeBuildTrig(") != std::string::npos) {
					const auto ids = extract_fourccs(line);
					current_rule = ids.empty() ? std::string() : ids.front();
					continue;
				}
				if (line.find("GetUnitTypeId(GetConstructedStructure())") != std::string::npos) {
					current_rule = first_fourcc_or_empty(line);
					continue;
				}
				if (current_rule.empty()) {
					continue;
				}
				if (line.find("SetPlayerTechMaxAllowed(") != std::string::npos) {
					const std::string target = first_fourcc_or_empty(line);
					if (!target.empty()) {
						add_edge(edges, seen_edges, "lua-availability", current_rule, target, "lua", "SetPlayerTechMaxAllowed", section.file, section.symbol);
					}
				} else if (line.find("SetPlayerTechResearched(") != std::string::npos) {
					const std::string target = first_fourcc_or_empty(line);
					if (!target.empty()) {
						add_edge(edges, seen_edges, "lua-auto-research", current_rule, target, "lua", "SetPlayerTechResearched", section.file, section.symbol);
					}
				} else if (line.find("UnitAddAbility(") != std::string::npos) {
					const auto ids = extract_fourccs(line);
					if (ids.size() >= 2) {
						add_edge(edges, seen_edges, "lua-adds-ability", ids[0], ids[1], "lua", "UnitAddAbility", section.file, section.symbol);
					} else if (ids.size() == 1) {
						add_edge(edges, seen_edges, "lua-adds-ability", current_rule, ids[0], "lua", "UnitAddAbility", section.file, section.symbol);
					}
				}
			}
		}

		const bool emitted_edges = edges.size() > edge_count_before;
		if (emitted_edges || is_tree_seed_symbol(symbol_lower) || is_race_init_symbol(symbol_lower) || symbol_lower.find("blood elves") != std::string::npos) {
			refs.push_back(std::move(ref));
		}
	}
}

} // namespace

export RaceGraphAnalysis analyze_race_graph(
	const fs::path& map_dir,
	const std::string& suffix,
	const std::vector<std::string>& race_tokens,
	const TriggerStrings& ts,
	const slk::SLK* full_units,
	const slk::SLK* full_abilities = nullptr,
	const slk::SLK* full_upgrades = nullptr
) {
	RaceGraphAnalysis result;
	result.suffix = suffix;
	result.race_tokens = race_tokens;

	std::unordered_map<std::string, std::string> ability_names_map;
	std::unordered_map<std::string, std::string> upgrade_names_map;
	const std::unordered_map<std::string, std::string>* ability_names = nullptr;
	const std::unordered_map<std::string, std::string>* upgrade_names = nullptr;

	if (full_abilities) {
		for (const auto& [id, index] : full_abilities->row_headers) {
			for (const char* col : {"name", "editorname", "bufftip"}) {
				const std::string n = full_abilities->data<std::string>(col, id);
				if (!n.empty()) {
					ability_names_map[id] = resolve_trigger_string(n, ts);
					break;
				}
			}
		}
		ability_names = &ability_names_map;
	}
	if (full_upgrades) {
		for (const auto& [id, index] : full_upgrades->row_headers) {
			for (const char* col : {"name1", "name", "bufftip"}) {
				std::string n = full_upgrades->data<std::string>(col, id);
				if (!n.empty()) {
					upgrade_names_map[id] = resolve_trigger_string(n, ts);
					break;
				}
			}
		}
		upgrade_names = &upgrade_names_map;
	}

	std::unordered_map<std::string, UnitRecord> records;
	if (full_units != nullptr) {
		records = load_full_unit_records(*full_units, ts);
		result.used_full_object_data = true;
	} else {
		records = load_map_only_unit_records(map_dir, ts, result.warnings);
	}

	for (const auto& [id, record] : records) {
		if (!suffix.empty() && contains_any_token(record.editor_suffix, {suffix})) {
			result.seed_unit_ids.push_back(id);
		}
	}

	const auto sections = collect_lua_sections(map_dir);
	const auto relevant_sections = select_relevant_lua_sections(sections, race_tokens);
	for (const auto& section : relevant_sections) {
		const std::string symbol_lower = to_lower_utf8(section.symbol);
		if (!is_tree_seed_symbol(symbol_lower) && !is_race_init_symbol(symbol_lower)) {
			continue;
		}
		for (const auto& id : extract_section_seed_ids(section)) {
			if (records.contains(id) && std::find(result.seed_unit_ids.begin(), result.seed_unit_ids.end(), id) == result.seed_unit_ids.end()) {
				result.seed_unit_ids.push_back(id);
			}
		}
	}

	std::sort(result.seed_unit_ids.begin(), result.seed_unit_ids.end());
	result.seed_unit_ids.erase(std::unique(result.seed_unit_ids.begin(), result.seed_unit_ids.end()), result.seed_unit_ids.end());

	if (result.seed_unit_ids.empty()) {
		result.warnings.push_back("no race seed units matched the requested suffix or Lua sections");
	}

	std::unordered_map<std::string, RaceGraphNode> node_map;
	collect_object_edges(records, node_map, result.edges, result.seed_unit_ids, ability_names, upgrade_names);
	collect_lua_edges(relevant_sections, records, node_map, result.edges, result.lua_references, race_tokens, "race:" + suffix, ability_names, upgrade_names);

	for (auto& [id, node] : node_map) {
		result.nodes.push_back(std::move(node));
	}
	std::sort(result.nodes.begin(), result.nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
	std::sort(result.edges.begin(), result.edges.end(), [](const auto& a, const auto& b) {
		return std::tie(a.kind, a.from_id, a.to_id, a.detail) < std::tie(b.kind, b.from_id, b.to_id, b.detail);
	});

	return result;
}
