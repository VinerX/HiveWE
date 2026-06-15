module;

#include <QMessageBox>

export module Map;

import std;
import types;
import MDX;
import SLK;
import INI;
import BinaryReader;
import GameCameras;
import Imports;
import MapInfo;
import Doodad;
import Sounds;
import Regions;
import WorldUndoManager;
import TriggerStrings;
import PathingMap;
import ShadowMap;
import Physics;
import Hierarchy;
import Camera;
import Timer;
import Physics;
import ModificationTables;
import ObjectData;
import RenderManager;
import TableModel;
import Globals;
import Units;
import Doodads;
import Triggers;
import Terrain;
import GameplayConstants;
import Utilities;
import UnorderedMap;
import "brush.h";
import <glad/glad.h>;
import <bullet/btBulletDynamicsCommon.h>;
import <glm/glm.hpp>;
import <glm/gtc/matrix_transform.hpp>;

namespace fs = std::filesystem;
using namespace std::literals::string_literals;

namespace {
	void log_map_load_line(const std::string& message) {
		std::ofstream log("hivewe.log", std::ios::app);
		log << message << '\n';
		log.flush();
		std::println("{}", message);
	}
}

export struct FileUsage {
	std::string path;
	std::unordered_set<std::string> used_by; // empty = unused
};

export class Map: public QObject {
	Q_OBJECT

  private:
	void create_object_tables() {
		units_table = new TableModel(&units_slk, &units_meta_slk, &trigger_strings);
		items_table = new TableModel(&items_slk, &items_meta_slk, &trigger_strings);
		abilities_table = new TableModel(&abilities_slk, &abilities_meta_slk, &trigger_strings);
		doodads_table = new TableModel(&doodads_slk, &doodads_meta_slk, &trigger_strings);
		destructibles_table = new TableModel(&destructibles_slk, &destructibles_meta_slk, &trigger_strings);
		upgrade_table = new TableModel(&upgrade_slk, &upgrade_meta_slk, &trigger_strings);
		buff_table = new TableModel(&buff_slk, &buff_meta_slk, &trigger_strings);
	}

  public:
	bool loaded = false;

	// Re-reads object data (base game SLK + the map's modification tables) from
	// disk in place, keeping terrain, placements, triggers, etc. untouched. Used
	// by the editor's "Merge object data from disk" button to pick up changes the
	// CLI/agent made to object definitions without discarding in-editor work.
	// The existing TableModels are reused (their SLK pointers stay valid); each is
	// wrapped in a model reset so open Object/Spreadsheet editors refresh.
	void reload_object_data() {
		TableModel* tables[] = { units_table, items_table, abilities_table,
								 doodads_table, destructibles_table, upgrade_table, buff_table };
		for (TableModel* t : tables) {
			if (t) t->begin_reset();
		}
		load_base_object_data([](std::string_view) {});
		load_map_object_data();
		for (TableModel* t : tables) {
			if (t) t->end_reset();
		}
	}

	TriggerStrings trigger_strings;
	Triggers triggers;
	MapInfo info;
	Terrain terrain;
	PathingMap pathing_map;
	Imports imports;
	Doodads doodads;
	Units units;
	Regions regions;
	GameCameras cameras;
	Sounds sounds;
	GameplayConstants gameplay_constants;
	ShadowMap shadow_map;
	WorldUndoManager world_undo;
	Brush* brush = nullptr;
	Physics physics;

	bool enforce_water_height_limits = true;

	bool render_doodads = true;
	bool render_units = true;
	bool render_pathing = false;
	bool render_brush = true;
	bool render_lighting = true;
	bool render_water = true;
	bool render_click_helpers = true;
	bool render_wireframe = false;
	bool render_debug = false;

	glm::vec3 light_direction = glm::normalize(glm::vec3(1.f, 1.f, -3.f));

	fs::path filesystem_path;
	std::string name;

	RenderManager render_manager;

	void load(const fs::path& path) {
		Timer full_timer;
		Timer timer;

		const auto log_phase = [&](std::string_view message) {
			log_map_load_line(std::format("[MAP] {}", message));
		};
		const auto run_phase = [&](std::string_view phase_name, auto&& fn) {
			log_phase(std::format("BEGIN {}", phase_name));
			try {
				std::forward<decltype(fn)>(fn)();
				log_phase(std::format("END {}", phase_name));
			} catch (const std::exception& ex) {
				log_phase(std::format("FAIL {}: {}", phase_name, ex.what()));
				throw std::runtime_error(std::format("Map load phase '{}' failed: {}", phase_name, ex.what()));
			}
		};

		hierarchy.map_directory = path;
		filesystem_path = fs::absolute(path) / "";
		name = (--(--filesystem_path.end()))->string();
		log_phase(std::format("Loading map directory {}", filesystem_path.string()));

		// ToDo So for the game data files we should actually load from _balance/custom_v0.w3mod/Units, _balance/custom_v1.w3mod/Units, _balance/melee_v0.w3mod/units or /Units depending on the Game Data set and Game Data Versions
		// Maybe just ignore RoC so we only need to choose between _balance/custom_v1.w3mod/Units and /Units
		// Maybe just force everyone to suck it up and use /Units

		load_base_object_data(log_phase);
		create_object_tables();

		std::println("\nSLK loading:\t {:>5}ms", timer.elapsed_ms());
		timer.reset();

		run_phase("trigger strings", [&] {
			if (hierarchy.map_file_exists("war3map.wts")) {
				trigger_strings.load();
			}
		});

		run_phase("triggers", [&] {
			if (hierarchy.map_file_exists("war3map.wtg")) {
				triggers.load();

				// Custom text triggers (JASS)
				if (hierarchy.map_file_exists("war3map.wct")) {
					triggers.load_scripts();
				}
			}
		});

		std::println("Trigger loading: {:>5}ms", timer.elapsed_ms());
		timer.reset();

		run_phase("gameplay constants", [&] {
			gameplay_constants.load();
		});

		run_phase("map info", [&] {
			info.load();
		});
		profile_reset();
		run_phase("terrain", [&] {
			terrain.load(physics);
		});

		std::println("Terrain loading: {:>5}ms", timer.elapsed_ms());
		profile_print();
		timer.reset();

		run_phase("pathing map", [&] {
			if (hierarchy.map_file_exists("war3map.wpm")) {
				pathing_map.load(terrain.width, terrain.height);
			} else {
				pathing_map.resize(terrain.width * 4, terrain.height * 4);
			}
		});

		std::println("Pathing loading: {:>5}ms", timer.elapsed_ms());
		timer.reset();

		run_phase("object data", [&] { load_map_object_data(); });

		profile_reset();
		run_phase("doodad instances", [&] {
			doodads.load(terrain, info);
			doodads.create(terrain, pathing_map);
			glFinish(); // Ensure all GL work submitted on worker contexts is visible to the main context
		});

		std::println("Doodad loading:\t {:>5}ms", timer.elapsed_ms());
		profile_print();
		timer.reset();

		// Units/Items
		profile_reset();
		run_phase("unit/item instances", [&] {
			if (hierarchy.map_file_exists("war3mapUnits.doo")) {
				units.load(terrain, info);
				units.create();
				glFinish(); // Ensure all GL work submitted on worker contexts is visible to the main context
			}
		});

		std::println("Unit loading:\t {:>5}ms", timer.elapsed_ms());
		profile_print();
		timer.reset();

		run_phase("regions/cameras/sounds", [&] {
			if (hierarchy.map_file_exists("war3map.w3r")) {
				regions.load(terrain.offset.x, terrain.offset.y);
			}

			if (hierarchy.map_file_exists("war3map.w3c")) {
				cameras.load(info.game_version_major, info.game_version_minor, terrain.offset.x, terrain.offset.y);
			}

			if (hierarchy.map_file_exists("war3map.w3s")) {
				sounds.load();
			}
		});

		std::println("Misc loading:\t {:>5}ms", timer.elapsed_ms());
		timer.reset();

		run_phase("shadow map", [&] {
			if (hierarchy.map_file_exists("war3map.shd")) {
				shadow_map.load(terrain.width - 1, terrain.height - 1);
			} else {
				shadow_map.resize((terrain.width - 1) * 4, (terrain.height - 1) * 4);
			}
		});

		std::println("Shadows loading: {:>5}ms", timer.elapsed_ms());
		timer.reset();

		std::println("Full loading: {:>5}ms", full_timer.elapsed_ms());

		// Center camera
		camera.position = glm::vec3(terrain.width / 2, terrain.height / 2, 0);
		camera.position.z = terrain.interpolated_height(camera.position.x, camera.position.y, true);

		loaded = true;

		connect(
			units_table,
			&TableModel::dataChanged,
			[&](const QModelIndex& top_left, const QModelIndex& top_right, const QVector<int>& roles) {
				const std::string& id = units_slk.index_to_row.at(top_left.row());
				const std::string& field = units_slk.index_to_column.at(top_left.column());
				units.process_unit_field_change(id, field);
			}
		);

		connect(units_table, &TableModel::rowsAboutToBeRemoved, [&](const QModelIndex& parent, int first, int last) {
			for (size_t i = first; i <= last; i++) {
				const std::string& id = units_slk.index_to_row.at(i);
				std::erase_if(units.units, [&](Unit& unit) {
					return unit.id == id;
				});

				if (brush) {
					brush->unselect_id(id);
				}
			}
		});

		connect(
			items_table,
			&TableModel::dataChanged,
			[&](const QModelIndex& top_left, const QModelIndex& top_right, const QVector<int>& roles) {
				const std::string& id = items_slk.index_to_row.at(top_left.row());
				const std::string& field = items_slk.index_to_column.at(top_left.column());
				units.process_item_field_change(id, field);
			}
		);

		connect(items_table, &TableModel::rowsAboutToBeRemoved, [&](const QModelIndex& parent, int first, int last) {
			for (size_t i = first; i <= last; i++) {
				const std::string& id = items_slk.index_to_row.at(i);
				std::erase_if(units.items, [&](Unit& item) {
					return item.id == id;
				});
			}
		});

		connect(
			doodads_table,
			&TableModel::dataChanged,
			[&](const QModelIndex& top_left, const QModelIndex& top_right, const QVector<int>& roles) {
				const std::string& id = doodads_slk.index_to_row.at(top_left.row());
				const std::string& field = doodads_slk.index_to_column.at(top_left.column());
				doodads.process_doodad_field_change(id, field, terrain);
			}
		);

		connect(doodads_table, &TableModel::rowsAboutToBeRemoved, [&](const QModelIndex& parent, int first, int last) {
			for (size_t i = first; i <= last; i++) {
				const std::string& id = doodads_slk.index_to_row.at(i);
				std::erase_if(doodads.doodads, [&](Doodad& doodad) {
					return doodad.id == id;
				});

				if (brush) {
					brush->unselect_id(id);
				}
			}
		});

		connect(
			destructibles_table,
			&TableModel::dataChanged,
			[&](const QModelIndex& top_left, const QModelIndex& top_right, const QVector<int>& roles) {
				const std::string& id = destructibles_slk.index_to_row.at(top_left.row());
				const std::string& field = destructibles_slk.index_to_column.at(top_left.column());
				doodads.process_destructible_field_change(id, field, terrain);
			}
		);

		connect(destructibles_table, &TableModel::rowsAboutToBeRemoved, [&](const QModelIndex& parent, int first, int last) {
			for (size_t i = first; i <= last; i++) {
				const std::string& id = destructibles_slk.index_to_row.at(i);
				std::erase_if(doodads.doodads, [&](Doodad& destructable) {
					return destructable.id == id;
				});

				if (brush) {
					brush->unselect_id(id);
				}
			}
		});
	}

	bool save(const fs::path& path) {
		Timer timer;
		if (!fs::equivalent(path, filesystem_path)) {
			try {
				fs::copy(filesystem_path, fs::absolute(path), fs::copy_options::recursive);
			} catch (fs::filesystem_error& e) {
				QMessageBox msgbox;
				msgbox.setText(e.what());
				msgbox.exec();
				return false;
			}
			filesystem_path = fs::absolute(path) / "";
			name = (*--(--filesystem_path.end())).string();
		}

		pathing_map.save();
		terrain.save();
		shadow_map.save();

		save_modification_file("war3map.w3d", doodads_slk, doodads_meta_slk, true, false);
		save_modification_file("war3mapSkin.w3d", doodads_slk, doodads_meta_slk, true, true);
		save_modification_file("war3map.w3b", destructibles_slk, destructibles_meta_slk, false, false);
		save_modification_file("war3mapSkin.w3b", destructibles_slk, destructibles_meta_slk, false, true);
		doodads.save(terrain);

		save_modification_file("war3map.w3u", units_slk, units_meta_slk, false, false);
		save_modification_file("war3mapSkin.w3u", units_slk, units_meta_slk, false, true);
		save_modification_file("war3map.w3t", items_slk, items_meta_slk, false, false);
		save_modification_file("war3mapSkin.w3t", items_slk, items_meta_slk, false, true);
		units.save(terrain);

		save_modification_file("war3map.w3a", abilities_slk, abilities_meta_slk, true, false);
		save_modification_file("war3mapSkin.w3a", abilities_slk, abilities_meta_slk, true, true);

		save_modification_file("war3map.w3h", buff_slk, buff_meta_slk, false, false);
		save_modification_file("war3mapSkin.w3h", buff_slk, buff_meta_slk, false, true);
		save_modification_file("war3map.w3q", upgrade_slk, upgrade_meta_slk, true, false);
		save_modification_file("war3mapSkin.w3q", upgrade_slk, upgrade_meta_slk, true, true);

		info.save(terrain.tileset);
		trigger_strings.save();
		triggers.save();
		triggers.save_scripts();
		ScriptMode mode = ScriptMode::jass;
		if (info.lua) {
			mode = ScriptMode::lua;
		}

		const auto result = triggers.generate_map_script(terrain, units, doodads, info, sounds, regions, cameras, mode);
		if (!result.has_value()) {
			QMessageBox::information(
				nullptr,
				"vJass output",
				"There were compilation errors:\n" + QString::fromStdString(result.error()),
				QMessageBox::StandardButton::Ok
			);
		}

		gameplay_constants.save();

		imports.save(filesystem_path);

		std::println("Saving took: {:>5}ms", timer.elapsed_ms());

		return true;
	}

	void update(double delta, int width, int height) {
		if (!loaded) {
			return;
		}

		camera.position.z = terrain.interpolated_height(camera.position.x, camera.position.y, true);
		camera.update(delta);

		// Update current water texture index
		terrain.current_texture += std::max(0.0, terrain.animation_rate * delta);
		if (terrain.current_texture >= terrain.water_textures_nr) {
			terrain.current_texture = 0;
		}

		/*auto current_time = std::chrono::steady_clock::now().time_since_epoch();
		auto seconds = std::chrono::duration_cast<std::chrono::milliseconds>(current_time).count() / 1000.f;
		light_direction = glm::normalize(glm::vec3(std::cos(seconds), std::sin(seconds), -2.f));*/

		// Map mouse coordinates to world coordinates
		if (input_handler.mouse != input_handler.previous_mouse) {
			glm::vec3 window = {input_handler.mouse.x, height - input_handler.mouse.y, 1.f};
			glm::vec3 pos = glm::unProject(window, camera.view, camera.projection, glm::vec4(0, 0, width, height));
			glm::vec3 origin = camera.position - camera.direction * camera.distance;
			glm::vec3 direction = glm::normalize(pos - origin);
			glm::vec3 toto = origin + direction * 2000.f;

			btVector3 from(origin.x, origin.y, origin.z);
			btVector3 to(toto.x, toto.y, toto.z);

			btCollisionWorld::ClosestRayResultCallback res(from, to);
			res.m_collisionFilterGroup = 32;
			res.m_collisionFilterMask = 32;
			physics.dynamicsWorld->rayTest(from, to, res);

			if (res.hasHit()) {
				auto& hit = res.m_hitPointWorld;
				input_handler.previous_mouse_world = input_handler.mouse_world;
				input_handler.mouse_world = glm::vec3(hit.x(), hit.y(), hit.z());
			}
		}

		// Animate units
		std::for_each(std::execution::par_unseq, units.units.begin(), units.units.end(), [&](Unit& i) {
			if (i.id == "sloc") {
				return;
			} // ToDo handle starting locations

			mdx::Extent& extent = i.mesh->mdx->sequences[i.skeleton.sequence_index].extent;
			if (!camera.inside_frustrum_transform(extent.minimum, extent.maximum, i.skeleton.matrix)) {
				return;
			}

			i.skeleton.update(delta);
		});

		// Animate items
		for (auto& i : units.items) {
			i.skeleton.update(delta);
		}

		// Animate doodads
		std::for_each(std::execution::par_unseq, doodads.doodads.begin(), doodads.doodads.end(), [&](Doodad& i) {
			mdx::Extent& extent = i.mesh->mdx->sequences[i.skeleton.sequence_index].extent;
			if (!camera.inside_frustrum_transform(extent.minimum, extent.maximum, i.skeleton.matrix)) {
				return;
			}

			i.skeleton.update(delta);
		});
	}

	void render() {
		// While switching maps it may happen that render is called before loading has finished.
		if (!loaded) {
			return;
		}

		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glPolygonMode(GL_FRONT_AND_BACK, render_wireframe ? GL_LINE : GL_FILL);

		terrain.render_ground(render_pathing, render_lighting, light_direction, brush, pathing_map);

		if (render_doodads) {
			for (const auto& i : doodads.doodads) {
				render_manager.queue_render(*i.mesh, i.skeleton, i.color, 0);
				if (render_click_helpers) {
					const bool is_doodad = doodads_slk.row_headers.contains(i.id);
					const slk::SLK& slk = is_doodad ? doodads_slk : destructibles_slk;
					if (slk.data<bool>("useclickhelper", i.id)) {
						render_manager.queue_click_helper(i.skeleton.matrix);
					}
				}
			}
			for (const auto& i : doodads.special_doodads) {
				render_manager.queue_render(*i.mesh, i.skeleton, glm::vec3(1.f), 0);
			}
		}

		if (render_units) {
			for (auto& i : units.units) {
				if (i.id == "sloc") {
					continue;
				} // ToDo handle starting locations

				render_manager.queue_render(*i.mesh, i.skeleton, i.color, i.player);
			}
			for (auto& i : units.items) {
				render_manager.queue_render(*i.mesh, i.skeleton, i.color, i.player);
			}
		}

		if (render_brush && brush) {
			brush->render();
		}

		render_manager.render(render_lighting, light_direction);
		if (render_water) {
			terrain.render_water();
		}

		// physics.dynamicsWorld->debugDrawWorld();
		// physics.draw->render();
	}

	/// Resizes the entire map by expanding/shirnking it from all sides
	/// Handles terrain, pathing map, shadow map and preplaced objects
	/// Also, as per vanilla WE behaviour, clears the entire world undo stack
	void resize(int delta_left, int delta_right, int delta_top, int delta_bottom);

	/// Sets the playable area (shadowed map bounds)
	/// Handles the terrain flags, camera bounds and map info
	/// Also updates the pathing map and deletes units/items which are now out of bounds
	void set_playable_area(int unplayable_left, int unplayable_right, int unplayable_top, int unplayable_bottom);

	std::string get_unique_id(bool first_uppercase) {
		std::random_device rd;
		std::mt19937 mt(rd());
		std::uniform_int_distribution<int> dist(0, 25);
	again:

		std::string id =
			""s + char((first_uppercase ? 'A' : 'a') + dist(mt)) + char('a' + dist(mt)) + char('a' + dist(mt)) + char('a' + dist(mt));

		if (units_slk.row_headers.contains(id) || items_slk.row_headers.contains(id) || abilities_slk.row_headers.contains(id)
			|| doodads_slk.row_headers.contains(id) || destructibles_slk.row_headers.contains(id) || upgrade_slk.row_headers.contains(id)
			|| buff_slk.row_headers.contains(id)) {
			std::print("Generated an existing ID: {} what're the odds\n", id);
			goto again;
		}

		return id;
	}

	/// Returns a list containing all the custom resources in the map folder with how many times they are referenced.
	/// Note that this isn't exhaustive as we cannot detect confidently whether all game file overrides are used.
	/// We also scan the map code for the file path (with forward slashed),
	/// but map code can do arbitrary file loading in ways that are hard to detect such as composing a file path by appending two strings
	std::vector<FileUsage> get_file_usage() const;

  private:
	int update_object_positions(int delta_left, int delta_right, int delta_top, int delta_bottom);
	void reset_map_edge_pathing(
		int old_left,
		int old_right,
		int old_top,
		int old_bottom,
		int new_left,
		int new_right,
		int new_top,
		int new_bottom
	);
};

#include "map.moc"
