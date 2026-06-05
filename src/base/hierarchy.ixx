export module Hierarchy;

import std;
import types;
import JSON;
import BinaryReader;
import CASC;
import no_init_allocator;
import Utilities;

using namespace std::literals::string_literals;
namespace fs = std::filesystem;

export class Hierarchy {
  public:
	char tileset = 'L';
	std::string locale = "enus";
	casc::CASC game_data;
	json::JSON aliases;

	fs::path map_directory;
	fs::path warcraft_directory;
	fs::path root_directory;

	bool ptr = false;
	bool hd = true;
	bool teen = false;
	bool local_files = true;

	Hierarchy() = default;

	// Flavour flags (ptr/hd/teen) and local_files are configured by the caller before
	// open_casc. The GUI fills them from QSettings / the WC3 registry in main.cpp; the
	// headless CLI sets them from arguments. This keeps Hierarchy free of any Qt dependency.
	bool open_casc(const fs::path& directory) {
		warcraft_directory = directory;

		bool open = game_data.open(warcraft_directory / (ptr ? ":w3t" : ":w3"));
		root_directory = warcraft_directory / (ptr ? "_ptr_" : "_retail_");

		if (open) {
			aliases.load(open_file("filealiases.json").value());

			// Auto-detect the installed locale. WC3's CASC manifest lists every
			// locale, but only the language(s) the user actually installed have local
			// data; reading a non-downloaded locale fails (error 4350). HiveWE used to
			// hardcode "enus", so on a non-English install all strings fell back to raw
			// keys. Probe a preference list (English first) and pick the first locale
			// whose string files actually read. Falls back to the previous default.
			static constexpr std::array locale_preference = {
				"enus", "engb", "ruru", "dede", "frfr", "eses", "esmx",
				"itit", "ptbr", "ptpt", "plpl", "zhcn", "zhtw", "kokr"
			};
			bool locale_found = false;
			for (const auto* loc : locale_preference) {
				const auto probe = game_data.open_file(std::format("war3.w3mod:_locales/{}.w3mod:ui/worldeditstrings.txt", loc));
				if (probe.has_value()) {
					locale = loc;
					locale_found = true;
					break;
				}
			}
			{
				std::ofstream log("hivewe.log", std::ios::app);
				if (locale_found) {
					log << "[INFO] Detected game locale: " << locale << "\n";
				} else {
					log << "[WARN] No readable locale found, falling back to: " << locale << "\n";
				}
				log.flush();
			}
		}
		return open;
	}

	[[nodiscard]]
	auto open_file(const fs::path& path) const -> std::expected<BinaryReader, std::string> {
		const std::string path_str = path.string();

		#define TRY_OPEN(expr)                \
				if (auto file = (expr); file) {   \
				return file;                   \
				}

		TRY_OPEN(read_file("data/overrides" / path));

		if (local_files) {
			TRY_OPEN(read_file(root_directory / path));
		}

		if (hd && teen) {
			TRY_OPEN(map_file_read("_hd.w3mod:_teen.w3mod:" + path_str));
		}

		if (hd) {
			TRY_OPEN(map_file_read("_hd.w3mod:" + path_str));
		}

		TRY_OPEN(map_file_read(path));

		if (hd) {
			TRY_OPEN(game_data.open_file(std::format("war3.w3mod:_hd.w3mod:_tilesets/{}.w3mod:{}", tileset, path_str)));
		}

		if (hd && teen) {
			TRY_OPEN(game_data.open_file("war3.w3mod:_hd.w3mod:_teen.w3mod:"s + path_str));
		}

		if (hd) {
			TRY_OPEN(game_data.open_file("war3.w3mod:_hd.w3mod:"s + path_str));
		}

		TRY_OPEN(game_data.open_file(std::format("war3.w3mod:_tilesets/{}.w3mod:{}", tileset, path_str)));
		TRY_OPEN(game_data.open_file(std::format("war3.w3mod:_locales/{}.w3mod:{}", locale, path_str)));

		if (teen) {
			TRY_OPEN(game_data.open_file("war3.w3mod:_teen.w3mod:"s + path_str));
		}

		TRY_OPEN(game_data.open_file("war3.w3mod:"s + path_str));
		TRY_OPEN(game_data.open_file("war3.w3mod:_deprecated.w3mod:"s + path_str));

		#undef TRY_OPEN

		if (aliases.exists(path_str)) {
			return open_file(aliases.alias(path_str));
		}

		return std::unexpected(path_str + " could not be found in the hierarchy");
	}

	[[nodiscard]]
	BinaryReader open_file_or_throw(const fs::path& path, std::string_view context = {}) const {
		auto result = open_file(path);
		if (!result) {
			if (context.empty()) {
				throw std::runtime_error(result.error());
			}
			throw std::runtime_error(std::format("{}: {}", context, result.error()));
		}
		return std::move(result.value());
	}

	bool file_exists(const fs::path& path) const {
		if (path.empty()) {
			return false;
		}

		const auto path_str = path.string();

		return fs::is_regular_file("data/overrides" / path) || (local_files && fs::is_regular_file(root_directory / path))
			|| (hd && teen && map_file_exists("_hd.w3mod:_teen.w3mod:" + path_str))
			|| (hd && map_file_exists("_hd.w3mod:" + path_str)) || map_file_exists(path)
			|| (hd && game_data.file_exists("war3.w3mod:_hd.w3mod:_tilesets/"s + tileset + ".w3mod:"s + path_str))
			|| (hd && teen && game_data.file_exists("war3.w3mod:_hd.w3mod:_teen.w3mod:"s + path_str))
			|| (hd && game_data.file_exists("war3.w3mod:_hd.w3mod:"s + path_str))
			|| game_data.file_exists("war3.w3mod:_tilesets/"s + tileset + ".w3mod:"s + path_str)
			|| game_data.file_exists(std::format("war3.w3mod:_locales/{}.w3mod:{}", locale, path_str))
			|| (teen && game_data.file_exists("war3.w3mod:_teen.w3mod:"s + path_str))
			|| game_data.file_exists("war3.w3mod:"s + path_str)
			|| game_data.file_exists("war3.w3mod:_deprecated.w3mod:"s + path_str)
			|| (aliases.exists(path_str) ? file_exists(aliases.alias(path_str)) : false);
	}

	[[nodiscard]]
	auto map_file_read(const fs::path& path) const -> std::expected<BinaryReader, std::string> {
		return read_file(map_directory / path);
	}

	[[nodiscard]]
	BinaryReader map_file_read_or_throw(const fs::path& path, std::string_view context = {}) const {
		auto result = map_file_read(path);
		if (!result) {
			if (context.empty()) {
				throw std::runtime_error(result.error());
			}
			throw std::runtime_error(std::format("{}: {}", context, result.error()));
		}
		return std::move(result.value());
	}

	/// source somewhere on disk, destination relative to the map
	void map_file_add(const fs::path& source, const fs::path& destination) const {
		fs::copy_file(source, map_directory / destination, fs::copy_options::overwrite_existing);
	}

	void map_file_write(const fs::path& path, const std::span<const u8> data) const {
		std::ofstream outfile(map_directory / path, std::ios::binary);

		if (!outfile) {
			throw std::runtime_error("Error writing file " + path.string());
		}

		outfile.write(reinterpret_cast<char const*>(data.data()), data.size());
	}

	void map_file_remove(const fs::path& path) const {
		fs::remove(map_directory / path);
	}

	bool map_file_exists(const fs::path& path) const {
		return fs::is_regular_file(map_directory / path);
	}

	void map_file_rename(const fs::path& original, const fs::path& renamed) const {
		fs::rename(map_directory / original, map_directory / renamed);
	}
};

export inline Hierarchy hierarchy;

export inline void hierarchy_set_map_directory(const fs::path& directory) {
	hierarchy.map_directory = fs::absolute(directory);
}
