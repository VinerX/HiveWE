// SafeMove — Qt-free core for "safely moving" an imported file inside a map.
//
// Moving an import on disk normally breaks every path reference to it. This
// module renames the file *and* rewrites all references that HiveWE can detect
// confidently, so the moved import keeps working. It is the single source of
// truth shared by both the CLI (`safe-move`) and the GUI (Asset Manager).
//
// What gets updated (mirrors the detection in Map::get_file_usage):
//   * object data (units/items/abilities/doodads/destructibles/upgrades/buffs)
//     — every custom (shadow) field whose value equals the old path
//   * sounds        — war3map.w3s `file` paths (raw byte rewrite; HiveWE never
//                     re-serialises w3s itself and load() discards v2/v3 extras)
//   * map info      — war3map.w3i loading_screen_model / prologue_screen_model /
//                     custom_sound_environment
//   * map script    — war3map.j / war3map.lua textual references (boundary-aware)
//   * war3map.imp   — regenerated from the directory after the rename
//
// What it deliberately does NOT touch (see [[mdx-internal-refs]] and the plan):
//   * paths embedded *inside* .mdx binaries (textures / attachments / emitters)
//   * raw JASS/Lua hardcoded in custom triggers (war3map.wtg / war3map.wct) —
//     those round-trip through the trigger format and substring edits there are
//     unsafe; a warning is emitted when the script references the moved file.
//
// The operation is reversible: run it again with --from/--to swapped.

export module SafeMove;

import std;
import types;
import Globals;
import Hierarchy;
import ModificationTables;
import MapInfo;
import Sounds;
import Imports;
import BinaryReader;
import BinaryWriter;
import SLK;

namespace fs = std::filesystem;

// One reference that was (or, in dry-run, would be) rewritten.
export struct MoveReference {
	std::string location;  // "unit:hfoo:art", "sound:MySound", "info:loading_screen_model", "script"
	std::string old_value;
	std::string new_value;
};

export struct MoveReport {
	bool ok = false;
	bool dry_run = false;
	bool renamed = false;
	std::string from;
	std::string to;
	std::string error;                       // set when ok == false
	std::vector<MoveReference> references;    // all updated references
	std::vector<std::string> warnings;

	std::size_t reference_count() const {
		return references.size();
	}
};

namespace {

// WC3 paths are case-insensitive and mix '\' / '/'. Normalise for *comparison*
// only (lowercase + forward slashes); the stored value's own separator style is
// preserved on write so we match the surrounding data's convention.
std::string norm(std::string_view s) {
	std::string o;
	o.reserve(s.size());
	for (char c : s) {
		if (c == '\\') {
			c = '/';
		}
		o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return o;
}

// Rewrites `replacement` to use the same path separator style as `like`.
std::string with_separators_like(std::string_view replacement, std::string_view like) {
	const bool backslash = like.find('\\') != std::string_view::npos;
	std::string out(replacement);
	for (char& c : out) {
		if (backslash && c == '/') {
			c = '\\';
		} else if (!backslash && c == '\\') {
			c = '/';
		}
	}
	return out;
}

struct ObjectType {
	slk::SLK* slk;
	const slk::SLK* meta;
	const char* name;
	const char* mod_file;
	const char* skin_file;
	bool optional_ints;
};

// Matches the per-type flags/files used by Map::save().
std::array<ObjectType, 7> object_types() {
	return {{
		{&units_slk, &units_meta_slk, "unit", "war3map.w3u", "war3mapSkin.w3u", false},
		{&items_slk, &items_meta_slk, "item", "war3map.w3t", "war3mapSkin.w3t", false},
		{&abilities_slk, &abilities_meta_slk, "ability", "war3map.w3a", "war3mapSkin.w3a", true},
		{&doodads_slk, &doodads_meta_slk, "doodad", "war3map.w3d", "war3mapSkin.w3d", true},
		{&destructibles_slk, &destructibles_meta_slk, "destructible", "war3map.w3b", "war3mapSkin.w3b", false},
		{&upgrade_slk, &upgrade_meta_slk, "upgrade", "war3map.w3q", "war3mapSkin.w3q", true},
		{&buff_slk, &buff_meta_slk, "buff", "war3map.w3h", "war3mapSkin.w3h", false},
	}};
}

bool is_path_char(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' || c == '/' || c == '\\';
}

// Replaces every occurrence of `needle` that is delimited by non-path characters
// on both sides (so "foo/bar.blp" never matches inside "foo/barbaz.blp"). When
// `apply` is false it only counts. Returns the number of matches.
std::size_t replace_token(std::string& text, std::string_view needle, std::string_view replacement, bool apply) {
	if (needle.empty()) {
		return 0;
	}
	std::size_t count = 0;
	std::string out;
	if (apply) {
		out.reserve(text.size());
	}
	std::size_t i = 0;
	while (i < text.size()) {
		if (i + needle.size() <= text.size() && text.compare(i, needle.size(), needle) == 0) {
			const char before = (i == 0) ? '\0' : text[i - 1];
			const char after = (i + needle.size() >= text.size()) ? '\0' : text[i + needle.size()];
			if (!is_path_char(before) && !is_path_char(after)) {
				++count;
				if (apply) {
					out.append(replacement);
				}
				i += needle.size();
				continue;
			}
		}
		if (apply) {
			out.push_back(text[i]);
		}
		++i;
	}
	if (apply) {
		text = std::move(out);
	}
	return count;
}

// Re-emits war3map.w3s byte-for-byte, swapping `file` paths that match
// `from_key`. Unknown v2/v3 trailing fields are copied verbatim so nothing is
// lost (unlike Sounds::load, which discards them). Returns the number changed.
int rewrite_sound_paths(std::string_view from_key, std::string_view to, bool apply, std::vector<MoveReference>& refs) {
	if (!hierarchy.map_file_exists("war3map.w3s")) {
		return 0;
	}

	BinaryReader reader = hierarchy.map_file_read_or_throw("war3map.w3s", "safe_move w3s");
	BinaryWriter writer;

	const u32 version = reader.read<u32>();
	writer.write<u32>(version);
	const u32 count = reader.read<u32>();
	writer.write<u32>(count);

	int changed = 0;
	for (u32 i = 0; i < count; ++i) {
		const std::string name = reader.read_c_string();
		const std::string file = reader.read_c_string();
		const std::string eax = reader.read_c_string();

		writer.write_c_string(name);
		if (!file.empty() && norm(file) == from_key) {
			const std::string nv = with_separators_like(to, file);
			refs.push_back({std::string("sound:") + name, file, nv});
			writer.write_c_string(nv);
			++changed;
		} else {
			writer.write_c_string(file);
		}
		writer.write_c_string(eax);

		// Fixed 68-byte block (flags + 16 audio params), copied verbatim.
		for (int b = 0; b < 68; ++b) {
			writer.write<u8>(reader.read<u8>());
		}

		if (version >= 2) {
			const auto copy_cstr = [&] { writer.write_c_string(reader.read_c_string()); };
			const auto copy_int = [&] { writer.write<u32>(reader.read<u32>()); };
			copy_cstr(); copy_cstr(); copy_cstr();
			copy_int();
			copy_cstr(); copy_int();
			copy_cstr(); copy_int();
			copy_cstr(); copy_cstr(); copy_cstr(); copy_cstr();
			if (version >= 3) {
				copy_int();
			}
		}
	}

	// Defensive: preserve any trailing bytes we didn't model.
	while (reader.remaining() > 0) {
		writer.write<u8>(reader.read<u8>());
	}

	if (apply && changed > 0) {
		hierarchy.map_file_write("war3map.w3s", writer.buffer);
	}
	return changed;
}

} // namespace

// Moves `from` to `to` (both relative to the loaded map directory) and rewrites
// all detectable references. `info` and `sounds` are the live in-memory map
// objects (the GUI passes map->info / map->sounds; the CLI loads its own) so
// their state and the generated script stay consistent. The object-data SLKs are
// the process-global singletons (Globals) shared with the GUI.
export MoveReport safe_move_file(std::string from, std::string to, bool dry_run, MapInfo& info, Sounds& sounds) {
	MoveReport report;
	report.from = from;
	report.to = to;
	report.dry_run = dry_run;

	const auto fail = [&](std::string message) -> MoveReport {
		report.ok = false;
		report.error = std::move(message);
		return report;
	};

	if (hierarchy.map_directory.empty()) {
		return fail("no map is loaded (hierarchy.map_directory is empty)");
	}
	if (from.empty() || to.empty()) {
		return fail("both --from and --to are required");
	}

	const std::string from_key = norm(from);
	const std::string to_key = norm(to);
	if (from_key == to_key) {
		return fail("source and destination are the same path");
	}

	// Refuse to move reserved map files (war3map.*, etc.).
	const Imports imports;
	if (imports.blacklist.contains(fs::path(from).filename().string())) {
		return fail("refusing to move a reserved map file: " + from);
	}

	const fs::path src = hierarchy.map_directory / fs::path(from);
	const fs::path dst = hierarchy.map_directory / fs::path(to);
	std::error_code ec;
	if (!fs::is_regular_file(src, ec)) {
		return fail("source file not found in map: " + from);
	}
	if (fs::exists(dst, ec)) {
		return fail("destination already exists: " + to);
	}

	// ---- collect (read-only) -------------------------------------------------
	struct SlkEdit {
		slk::SLK* slk;
		std::string id;
		std::string col;
		std::string value;
	};
	std::vector<SlkEdit> slk_edits;
	std::array<bool, 7> type_changed{};

	const auto types = object_types();
	for (std::size_t t = 0; t < types.size(); ++t) {
		for (const auto& [id, props] : types[t].slk->shadow_data) {
			for (const auto& [col, value] : props) {
				if (col == "oldid" || value.empty()) {
					continue;
				}
				if (norm(value) == from_key) {
					const std::string nv = with_separators_like(to, value);
					report.references.push_back({std::string(types[t].name) + ":" + id + ":" + col, value, nv});
					slk_edits.push_back({types[t].slk, id, col, nv});
					type_changed[t] = true;
				}
			}
		}
	}

	bool info_changed = false;
	const auto match_info = [&](const char* field_name, std::string& field) {
		if (!field.empty() && norm(field) == from_key) {
			const std::string nv = with_separators_like(to, field);
			report.references.push_back({std::string("info:") + field_name, field, nv});
			if (!dry_run) {
				field = nv;
			}
			info_changed = true;
		}
	};
	match_info("loading_screen_model", info.loading_screen_model);
	match_info("prologue_screen_model", info.prologue_screen_model);
	match_info("custom_sound_environment", info.custom_sound_environment);

	// Sounds: report (and, when applying, rewrite the w3s on disk).
	const int sound_changes = rewrite_sound_paths(from_key, to, !dry_run, report.references);

	// Map script (war3map.j / war3map.lua).
	const std::string script_name = info.lua ? "war3map.lua" : "war3map.j";
	std::size_t script_changes = 0;
	if (hierarchy.map_file_exists(script_name)) {
		BinaryReader sr = hierarchy.map_file_read_or_throw(script_name, "safe_move script");
		std::string text(reinterpret_cast<const char*>(sr.buffer.data()), sr.buffer.size());

		const std::string from_fwd = with_separators_like(from, "/");
		const std::string from_back = with_separators_like(from, "\\");
		const std::string to_fwd = with_separators_like(to, "/");
		const std::string to_back = with_separators_like(to, "\\");

		script_changes += replace_token(text, from_fwd, to_fwd, !dry_run);
		if (from_back != from_fwd) {
			script_changes += replace_token(text, from_back, to_back, !dry_run);
		}

		if (script_changes > 0) {
			report.references.push_back({"script", from, to});
			report.warnings.push_back(
				"Found " + std::to_string(script_changes) + " reference(s) in " + script_name
				+ ". If these come from custom triggers (war3map.wtg/wct) they may reappear when the map is re-saved; verify in the trigger editor."
			);
			if (!dry_run) {
				const std::vector<u8> bytes(text.begin(), text.end());
				hierarchy.map_file_write(script_name, bytes);
			}
		}
	}

	if (dry_run) {
		report.ok = true;
		return report;
	}

	// ---- apply ---------------------------------------------------------------
	try {
		for (const auto& edit : slk_edits) {
			edit.slk->set_shadow_data(edit.col, edit.id, edit.value);
		}
		for (std::size_t t = 0; t < types.size(); ++t) {
			if (type_changed[t]) {
				save_modification_file(types[t].mod_file, *types[t].slk, *types[t].meta, types[t].optional_ints, false);
				save_modification_file(types[t].skin_file, *types[t].slk, *types[t].meta, types[t].optional_ints, true);
			}
		}

		if (info_changed) {
			info.save();
		}

		// Keep the in-memory sounds (used by script generation) in sync with the
		// w3s we just rewrote on disk.
		if (sound_changes > 0) {
			for (auto& s : sounds.sounds) {
				if (!s.file.empty() && norm(s.file) == from_key) {
					s.file = with_separators_like(to, s.file);
				}
			}
		}

		// Move the file itself, then regenerate war3map.imp from the directory.
		fs::create_directories(dst.parent_path(), ec);
		hierarchy.map_file_rename(from, to);
		report.renamed = true;

		imports.save(hierarchy.map_directory);
	} catch (const std::exception& e) {
		return fail(std::string("failed while applying move: ") + e.what());
	}

	report.ok = true;
	return report;
}
