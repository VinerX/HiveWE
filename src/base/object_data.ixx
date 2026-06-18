export module ObjectData;

// Headless object-data loading: builds the base SLK tables (units, items,
// abilities, doodads, destructibles, upgrades, buffs) from the WC3 game data and
// then overlays a map's modification tables (.w3u/.w3t/.w3a/.w3b/.w3d/.w3h/.w3q).
//
// This was lifted out of Map (a QObject pulling in Terrain/RenderManager/OpenGL)
// so it can be compiled into a headless core and driven by the CLI without a GL
// context. It operates on the global SLKs declared in Globals; Map now forwards
// to these functions.

import std;
import SLK;
import INI;
import Hierarchy;
import ModificationTables;
import Globals;
import UnorderedMap;

namespace fs = std::filesystem;

// Loads an optional INI game-data file, reporting (via log_phase) when it is
// absent instead of failing the whole load.
export template<typename Logger>
ini::INI load_optional_ini_with_log(const fs::path& ini_path, Logger&& log_phase) {
	const auto opened = hierarchy.open_file(ini_path);
	if (!opened) {
		log_phase(std::format("OPTIONAL game data missing, skipping {} ({})", ini_path.string(), opened.error()));
		return ini::INI();
	}
	return ini::INI(ini_path);
}

export template<typename Logger>
void load_base_object_data(Logger&& log_phase) {
	auto units_future = std::async(std::launch::async, [&] {
		units_slk = slk::SLK("Units/UnitData.slk");
		// By making some changes to unitmetadata.slk and unitdata.slk we can avoid the 1->2->2 mapping for SLK->OE->W3U files. We have to add some columns for this though
		units_slk.add_column("missilearc2");
		units_slk.add_column("missileart2");
		units_slk.add_column("missilespeed2");
		units_slk.add_column("buttonpos2");

		units_meta_slk = slk::SLK("Units/UnitMetaData.slk");
		units_meta_slk.substitute(world_edit_strings, "WorldEditStrings");
		units_meta_slk.build_meta_map();

		unit_editor_data = load_optional_ini_with_log("UI/UnitEditorData.txt", log_phase);
		unit_editor_data.substitute(world_edit_strings, "WorldEditStrings");
		// Have to substitute twice since some keys refer to other keys in the same file
		unit_editor_data.substitute(world_edit_strings, "WorldEditStrings");

		units_slk.merge(load_optional_ini_with_log("Units/UnitSkin.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/UnitWeaponsFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/UnitWeaponsSkin.txt", log_phase), units_meta_slk);

		units_slk.merge(slk::SLK("Units/UnitBalance.slk"));
		units_slk.merge(slk::SLK("Units/unitUI.slk"));
		units_slk.merge(slk::SLK("Units/UnitWeapons.slk"));
		units_slk.merge(slk::SLK("Units/UnitAbilities.slk"));

		units_slk.merge(load_optional_ini_with_log("Units/HumanUnitFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/OrcUnitFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/UndeadUnitFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/NightElfUnitFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/NeutralUnitFunc.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/CampaignUnitFunc.txt", log_phase), units_meta_slk);

		units_slk.merge(load_optional_ini_with_log("Units/HumanUnitStrings.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/OrcUnitStrings.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/UndeadUnitStrings.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/NightElfUnitStrings.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/NeutralUnitStrings.txt", log_phase), units_meta_slk);
		units_slk.merge(load_optional_ini_with_log("Units/CampaignUnitStrings.txt", log_phase), units_meta_slk);
	});

	auto items_future = std::async(std::launch::async, [&] {
		items_slk = slk::SLK("Units/ItemData.slk");
		items_meta_slk = slk::SLK("Units/ItemMetaData.slk");
		items_meta_slk.substitute(world_edit_strings, "WorldEditStrings");
		items_meta_slk.build_meta_map();

		items_slk.merge(load_optional_ini_with_log("Units/ItemSkin.txt", log_phase), items_meta_slk);
		items_slk.merge(load_optional_ini_with_log("Units/ItemFunc.txt", log_phase), items_meta_slk);
		items_slk.merge(load_optional_ini_with_log("Units/ItemStrings.txt", log_phase), items_meta_slk);
	});

	auto doodads_future = std::async(std::launch::async, [&] {
		doodads_slk = slk::SLK("Doodads/Doodads.slk");
		doodads_meta_slk = slk::SLK("Doodads/DoodadMetaData.slk");
		doodads_meta_slk.substitute(world_edit_strings, "WorldEditStrings");
		doodads_meta_slk.build_meta_map();

		doodads_slk.merge(load_optional_ini_with_log("Doodads/DoodadSkins.txt", log_phase), doodads_meta_slk);
		doodads_slk.substitute(world_edit_strings, "WorldEditStrings");
		doodads_slk.substitute(world_edit_game_strings, "WorldEditStrings");

		// Sometimes fields are empty or "-" which denotes empty aka the value 0.0
		for (auto& [key, fields] : doodads_slk.base_data) {
			if (auto found = fields.find("maxpitch"); found != fields.end()) {
				if (found->second.empty() || found->second == "-") {
					found->second = "0";
				}
			} else {
				fields["maxpitch"] = "0";
			}
			if (auto found = fields.find("maxroll"); found != fields.end()) {
				if (found->second.empty() || found->second == "-") {
					found->second = "0";
				}
			} else {
				fields["maxroll"] = "0";
			}
		}
	});

	auto destructibles_future = std::async(std::launch::async, [&] {
		destructibles_slk = slk::SLK("Units/DestructableData.slk");
		destructibles_slk.substitute(world_edit_strings, "WorldEditStrings");

		destructibles_meta_slk = slk::SLK("Units/DestructableMetaData.slk");
		destructibles_meta_slk.substitute(world_edit_strings, "WorldEditStrings");
		destructibles_meta_slk.build_meta_map();

		destructibles_slk.merge(load_optional_ini_with_log("Units/DestructableSkin.txt", log_phase), destructibles_meta_slk);
		destructibles_slk.substitute(world_edit_strings, "WorldEditStrings");
		destructibles_slk.substitute(world_edit_game_strings, "WorldEditStrings");

		// Sometimes fields are empty or "-" which denotes empty aka the value 0.0
		for (auto& [key, fields] : destructibles_slk.base_data) {
			if (auto found = fields.find("maxpitch"); found != fields.end()) {
				if (found->second.empty() || found->second == "-") {
					found->second = "0";
				}
			} else {
				fields["maxpitch"] = "0";
			}
			if (auto found = fields.find("maxroll"); found != fields.end()) {
				if (found->second.empty() || found->second == "-") {
					found->second = "0";
				}
			} else {
				fields["maxroll"] = "0";
			}
		}
	});

	// Load shared files
	const ini::INI ability_skin_ini = load_optional_ini_with_log("Units/AbilitySkin.txt", log_phase);
	const ini::INI ability_skin_strings_ini = load_optional_ini_with_log("Units/AbilitySkinStrings.txt", log_phase);
	const ini::INI human_ability_func_ini = load_optional_ini_with_log("Units/HumanAbilityFunc.txt", log_phase);
	const ini::INI orc_ability_func_ini = load_optional_ini_with_log("Units/OrcAbilityFunc.txt", log_phase);
	const ini::INI undead_ability_func_ini = load_optional_ini_with_log("Units/UndeadAbilityFunc.txt", log_phase);
	const ini::INI night_elf_ability_func_ini = load_optional_ini_with_log("Units/NightElfAbilityFunc.txt", log_phase);
	const ini::INI neutral_ability_func_ini = load_optional_ini_with_log("Units/NeutralAbilityFunc.txt", log_phase);
	const ini::INI item_ability_func_ini = load_optional_ini_with_log("Units/ItemAbilityFunc.txt", log_phase);
	const ini::INI common_ability_func_ini = load_optional_ini_with_log("Units/CommonAbilityFunc.txt", log_phase);
	const ini::INI campaign_ability_func_ini = load_optional_ini_with_log("Units/CampaignAbilityFunc.txt", log_phase);
	const ini::INI human_ability_strings_ini = load_optional_ini_with_log("Units/HumanAbilityStrings.txt", log_phase);
	const ini::INI orc_ability_strings_ini = load_optional_ini_with_log("Units/OrcAbilityStrings.txt", log_phase);
	const ini::INI undead_ability_strings_ini = load_optional_ini_with_log("Units/UndeadAbilityStrings.txt", log_phase);
	const ini::INI night_elf_ability_strings_ini = load_optional_ini_with_log("Units/NightElfAbilityStrings.txt", log_phase);
	const ini::INI neutral_ability_strings_ini = load_optional_ini_with_log("Units/NeutralAbilityStrings.txt", log_phase);
	const ini::INI item_ability_strings_ini = load_optional_ini_with_log("Units/ItemAbilityStrings.txt", log_phase);
	const ini::INI common_ability_strings_ini = load_optional_ini_with_log("Units/CommonAbilityStrings.txt", log_phase);
	const ini::INI campaign_ability_strings_ini = load_optional_ini_with_log("Units/CampaignAbilityStrings.txt", log_phase);

	auto abilities_future = std::async(std::launch::async, [&] {
		abilities_slk = slk::SLK("Units/AbilityData.slk");
		abilities_meta_slk = slk::SLK("Units/AbilityMetaData.slk");
		abilities_meta_slk.substitute(world_edit_strings, "WorldEditStrings");

		// Patch the SLKs
		abilities_slk.add_column("buttonpos2");
		abilities_slk.add_column("unbuttonpos2");
		abilities_slk.add_column("researchbuttonpos2");
		abilities_meta_slk.set_shadow_data("field", "abpy", "buttonpos2");
		abilities_meta_slk.set_shadow_data("field", "auby", "unbuttonpos2");
		abilities_meta_slk.set_shadow_data("field", "arpy", "researchbuttonpos2");
		abilities_meta_slk.build_meta_map();

		abilities_slk.merge(ability_skin_ini, abilities_meta_slk);
		abilities_slk.merge(ability_skin_strings_ini, abilities_meta_slk);
		abilities_slk.merge(human_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(orc_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(undead_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(night_elf_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(neutral_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(item_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(common_ability_func_ini, abilities_meta_slk);
		abilities_slk.merge(campaign_ability_func_ini, abilities_meta_slk);

		abilities_slk.merge(human_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(orc_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(undead_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(night_elf_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(neutral_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(item_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(common_ability_strings_ini, abilities_meta_slk);
		abilities_slk.merge(campaign_ability_strings_ini, abilities_meta_slk);
	});

	// Upgrades
	auto upgrade_future = std::async(std::launch::async, [&] {
		upgrade_slk = slk::SLK("Units/UpgradeData.slk");
		upgrade_meta_slk = slk::SLK("Units/UpgradeMetaData.slk");
		upgrade_meta_slk.substitute(world_edit_strings, "WorldEditStrings");

		// Patch the SLKs
		upgrade_slk.add_column("buttonpos2");
		upgrade_meta_slk.set_shadow_data("field", "gbpy", "buttonpos2");
		upgrade_meta_slk.build_meta_map();

		upgrade_slk.merge(ability_skin_ini, upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/UpgradeSkin.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/HumanUpgradeFunc.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/OrcUpgradeFunc.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/UndeadUpgradeFunc.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/NightElfUpgradeFunc.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/NeutralUpgradeFunc.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/CampaignUpgradeFunc.txt", log_phase), upgrade_meta_slk);

		upgrade_slk.merge(load_optional_ini_with_log("Units/CampaignUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/HumanUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/NeutralUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/NightElfUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/OrcUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/UndeadUpgradeStrings.txt", log_phase), upgrade_meta_slk);
		upgrade_slk.merge(load_optional_ini_with_log("Units/UpgradeSkinStrings.txt", log_phase), upgrade_meta_slk);
	});

	auto buff_future = std::async(std::launch::async, [&] {
		buff_slk = slk::SLK("Units/AbilityBuffData.slk");
		buff_meta_slk = slk::SLK("Units/AbilityBuffMetaData.slk");
		buff_meta_slk.substitute(world_edit_strings, "WorldEditStrings");
		buff_meta_slk.build_meta_map();

		buff_slk.merge(ability_skin_ini, buff_meta_slk);
		buff_slk.merge(ability_skin_strings_ini, buff_meta_slk);
		buff_slk.merge(human_ability_func_ini, buff_meta_slk);
		buff_slk.merge(orc_ability_func_ini, buff_meta_slk);
		buff_slk.merge(undead_ability_func_ini, buff_meta_slk);
		buff_slk.merge(night_elf_ability_func_ini, buff_meta_slk);
		buff_slk.merge(neutral_ability_func_ini, buff_meta_slk);
		buff_slk.merge(item_ability_func_ini, buff_meta_slk);
		buff_slk.merge(common_ability_func_ini, buff_meta_slk);
		buff_slk.merge(campaign_ability_func_ini, buff_meta_slk);

		buff_slk.merge(human_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(orc_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(undead_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(night_elf_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(neutral_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(item_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(common_ability_strings_ini, buff_meta_slk);
		buff_slk.merge(campaign_ability_strings_ini, buff_meta_slk);
	});

	units_future.get();
	abilities_future.get();
	items_future.get();
	doodads_future.get();
	destructibles_future.get();
	upgrade_future.get();
	buff_future.get();
}

// Outcome of a 3-way merge of one object-data cell (a single id+field override).
// Each side is the map's shadow override (std::nullopt = no override = base game
// value). base = the override at map-load time; mine = current in-editor state;
// theirs = the state now on disk (e.g. edited by the CLI/agent).
export enum class CellMerge {
	unchanged,   // mine == theirs (nothing to do)
	take_mine,   // only the editor changed this cell
	take_theirs, // only the disk changed this cell
	conflict     // both changed it differently — needs resolution
};

export inline CellMerge classify_cell_merge(const std::optional<std::string>& base,
											 const std::optional<std::string>& mine,
											 const std::optional<std::string>& theirs) {
	if (mine == theirs) {
		return CellMerge::unchanged;
	}
	if (mine == base) {
		return CellMerge::take_theirs;
	}
	if (theirs == base) {
		return CellMerge::take_mine;
	}
	return CellMerge::conflict;
}

// A single object-data cell whose editor (mine) and on-disk (theirs) states both
// diverged from the load-time baseline — the user must pick which to keep.
export struct ObjectMergeConflict {
	std::string table;  // "unit","item","ability","doodad","destructible","upgrade","buff"
	std::string id;     // object rawcode
	std::string field;  // SLK column
	std::optional<std::string> base, mine, theirs; // std::nullopt = no override (base game value)
	bool take_theirs = true; // resolution; defaults to the on-disk value
};

// An auto-resolved cell change to apply to the live SLK (std::nullopt = remove the
// override, reverting to the base game value).
export struct ObjectMergeChange {
	int table_index = 0;
	std::string id;
	std::string field;
	std::optional<std::string> value;
};

// A custom object present on disk but not in the editor; its row must be created
// (copied from oldid) before its fields are applied.
export struct ObjectMergeRowAdd {
	int table_index = 0;
	std::string id;
	std::string oldid;
};

export struct ObjectMergePlan {
	std::vector<ObjectMergeChange> changes;
	std::vector<ObjectMergeConflict> conflicts;
	std::vector<ObjectMergeRowAdd> row_adds;

	[[nodiscard]] bool empty() const { return changes.empty() && conflicts.empty() && row_adds.empty(); }
};

// ---- 3-way object-data merge (free functions, callable from both GUI and CLI) ----

export using ShadowData = hive::unordered_map<std::string, hive::unordered_map<std::string, std::string>>;

export struct ObjectMergeTableDesc {
	const char* name;
	slk::SLK* slk;
	slk::SLK* meta;
	bool optional_ints;
	std::array<const char*, 2> files; // map modification files (main + skin)
};

export std::array<ObjectMergeTableDesc, 7> get_object_merge_tables() {
	return { {
		{ "unit", &units_slk, &units_meta_slk, false, { "war3map.w3u", "war3mapSkin.w3u" } },
		{ "item", &items_slk, &items_meta_slk, false, { "war3map.w3t", "war3mapSkin.w3t" } },
		{ "ability", &abilities_slk, &abilities_meta_slk, true, { "war3map.w3a", "war3mapSkin.w3a" } },
		{ "doodad", &doodads_slk, &doodads_meta_slk, true, { "war3map.w3d", "war3mapSkin.w3d" } },
		{ "destructible", &destructibles_slk, &destructibles_meta_slk, false, { "war3map.w3b", "war3mapSkin.w3b" } },
		{ "upgrade", &upgrade_slk, &upgrade_meta_slk, true, { "war3map.w3q", "war3mapSkin.w3q" } },
		{ "buff", &buff_slk, &buff_meta_slk, false, { "war3map.w3h", "war3mapSkin.w3h" } },
	} };
}

inline auto shadow_lookup(const ShadowData& s, const std::string& id, const std::string& field) -> std::optional<std::string> {
	const auto row = s.find(id);
	if (row == s.end()) {
		return std::nullopt;
	}
	const auto cell = row->second.find(field);
	if (cell == row->second.end()) {
		return std::nullopt;
	}
	return cell->second;
}

// Loads the "theirs" modification files into a set of shadow maps.
// Optionally uses a different map directory (swaps hierarchy.map_directory temporarily).
export std::array<ShadowData, 7> load_theirs_shadows(
	const std::array<ObjectMergeTableDesc, 7>& tables,
	const std::optional<std::string>& map_directory = std::nullopt)
{
	std::array<ShadowData, 7> theirs;
	auto saved_dir = hierarchy.map_directory;
	if (map_directory) {
		hierarchy.map_directory = *map_directory;
	}

	for (int i = 0; i < 7; i++) {
		slk::SLK temp = *tables[i].slk;
		temp.shadow_data.clear();
		for (const char* file : tables[i].files) {
			if (hierarchy.map_file_exists(file)) {
				load_modification_file(file, temp, *tables[i].meta, tables[i].optional_ints);
			}
		}
		theirs[i] = std::move(temp.shadow_data);
	}

	hierarchy.map_directory = saved_dir;
	return theirs;
}

// Computes a 3-way merge between base (load-time baseline), mine (current state),
// and theirs (disk/other-branch state). Does NOT mutate any live data.
export ObjectMergePlan compute_object_merge(
	const std::array<ShadowData, 7>& base,
	const std::array<ShadowData, 7>& mine,
	const std::array<ShadowData, 7>& theirs,
	const std::array<ObjectMergeTableDesc, 7>& tables)
{
	ObjectMergePlan plan;

	for (int i = 0; i < 7; i++) {
		const auto& T = tables[i];

		std::unordered_set<std::string> ids;
		for (const auto& [id, _] : base[i]) ids.insert(id);
		for (const auto& [id, _] : mine[i]) ids.insert(id);
		for (const auto& [id, _] : theirs[i]) ids.insert(id);

		for (const std::string& id : ids) {
			const bool in_editor = T.slk->base_data.contains(id);
			bool row_add_planned = false;
			auto ensure_row = [&] {
				if (in_editor || row_add_planned) {
					return;
				}
				std::string oldid;
				if (const auto o = shadow_lookup(theirs[i], id, "oldid")) {
					oldid = *o;
				}
				plan.row_adds.push_back({ i, id, oldid });
				row_add_planned = true;
			};

			std::unordered_set<std::string> fields;
			auto collect = [&](const ShadowData& s) {
				const auto row = s.find(id);
				if (row != s.end()) {
					for (const auto& [f, _] : row->second) fields.insert(f);
				}
			};
			collect(base[i]);
			collect(mine[i]);
			collect(theirs[i]);

			for (const std::string& field : fields) {
				const auto vb = shadow_lookup(base[i], id, field);
				const auto vm = shadow_lookup(mine[i], id, field);
				const auto vt = shadow_lookup(theirs[i], id, field);
				switch (classify_cell_merge(vb, vm, vt)) {
					case CellMerge::unchanged:
					case CellMerge::take_mine:
						break;
					case CellMerge::take_theirs:
						ensure_row();
						plan.changes.push_back({ i, id, field, vt });
						break;
					case CellMerge::conflict:
						ensure_row();
						plan.conflicts.push_back({ T.name, id, field, vb, vm, vt, /*take_theirs=*/true });
						break;
				}
			}
		}
	}
	return plan;
}

// Applies a (resolved) merge plan to the live SLKs.
export void apply_object_merge(const ObjectMergePlan& plan, const std::array<ObjectMergeTableDesc, 7>& tables) {
	auto apply_cell = [](slk::SLK* slk, const std::string& id, const std::string& field, const std::optional<std::string>& value) {
		if (value) {
			slk->set_shadow_data(field, id, *value);
		} else if (slk->shadow_data.contains(id)) {
			slk->shadow_data.at(id).erase(field);
			if (slk->shadow_data.at(id).empty()) {
				slk->shadow_data.erase(id);
			}
		}
	};
	auto table_index = [&](const std::string& name) {
		for (int i = 0; i < 7; i++) {
			if (name == tables[i].name) return i;
		}
		return 0;
	};

	for (const ObjectMergeRowAdd& add : plan.row_adds) {
		slk::SLK* slk = tables[add.table_index].slk;
		if (slk->base_data.contains(add.id)) {
			continue;
		}
		if (!add.oldid.empty() && slk->base_data.contains(add.oldid)) {
			slk->copy_row(add.oldid, add.id, false);
		} else {
			slk->add_row(add.id);
		}
	}
	for (const ObjectMergeChange& c : plan.changes) {
		apply_cell(tables[c.table_index].slk, c.id, c.field, c.value);
	}
	for (const ObjectMergeConflict& cf : plan.conflicts) {
		slk::SLK* slk = tables[table_index(cf.table)].slk;
		apply_cell(slk, cf.id, cf.field, cf.take_theirs ? cf.theirs : cf.mine);
	}
}

export void load_map_object_data() {
	if (hierarchy.map_file_exists("war3map.w3d")) {
		load_modification_file("war3map.w3d", doodads_slk, doodads_meta_slk, true);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3d")) {
		load_modification_file("war3mapSkin.w3d", doodads_slk, doodads_meta_slk, true);
	}

	if (hierarchy.map_file_exists("war3map.w3b")) {
		load_modification_file("war3map.w3b", destructibles_slk, destructibles_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3b")) {
		load_modification_file("war3mapSkin.w3b", destructibles_slk, destructibles_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3map.w3u")) {
		load_modification_file("war3map.w3u", units_slk, units_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3u")) {
		load_modification_file("war3mapSkin.w3u", units_slk, units_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3map.w3t")) {
		load_modification_file("war3map.w3t", items_slk, items_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3t")) {
		load_modification_file("war3mapSkin.w3t", items_slk, items_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3map.w3a")) {
		load_modification_file("war3map.w3a", abilities_slk, abilities_meta_slk, true);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3a")) {
		load_modification_file("war3mapSkin.w3a", abilities_slk, abilities_meta_slk, true);
	}

	if (hierarchy.map_file_exists("war3map.w3h")) {
		load_modification_file("war3map.w3h", buff_slk, buff_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3h")) {
		load_modification_file("war3mapSkin.w3h", buff_slk, buff_meta_slk, false);
	}

	if (hierarchy.map_file_exists("war3map.w3q")) {
		load_modification_file("war3map.w3q", upgrade_slk, upgrade_meta_slk, true);
	}

	if (hierarchy.map_file_exists("war3mapSkin.w3q")) {
		load_modification_file("war3mapSkin.w3q", upgrade_slk, upgrade_meta_slk, true);
	}
}
