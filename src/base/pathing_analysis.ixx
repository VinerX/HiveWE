// Qt-free terrain-traversability analysis over war3map.wpm, for the CLI/agent.
//
// The pathing map (war3map.wpm) is the engine's ground truth for movement: a grid
// at 4x terrain-tile resolution where each cell is a byte of flags. This module
// loads that grid (no OpenGL, no Qt — just the cell bytes), maps the flags to a
// movement type, and offers the two things an agent needs to reason about portals
// and region placement: connected components ("islands") and A* pathfinding.
//
// Coordinate systems (see Regions::load / Terrain): a terrain tile is 128 world
// units; a pathing cell is 1/4 of a tile = 32 world units. World -> cell is
// (world - terrain_offset) / 32, where terrain_offset is the centre offset stored
// in the war3map.w3e header.

module;

#include <cstdint>

export module PathingAnalysis;

import std;
import BinaryReader;
import Hierarchy;

export namespace pathing {

// war3map.wpm cell flags (mirror of PathingMap::Flags).
enum Flags : uint8_t {
	unwalkable  = 0b00000010,
	unflyable   = 0b00000100,
	unbuildable = 0b00001000,
	harvestable = 0b00010000,
	blight      = 0b00100000,
	water       = 0b01000000,
	amphibious  = 0b10000000,
};

// Movement classes. Mapping to the raw flags is a documented heuristic (the engine
// has more nuance), tuned to be useful rather than bit-exact:
//   foot       - dry land only: not unwalkable and not water
//   water      - naval/float: water cells only
//   amphibious - land or water (cliffs still block): foot OR water
//   fly        - anything that is not unflyable
enum class MoveType { foot, water, amphibious, fly };

std::optional<MoveType> parse_move_type(std::string_view s) {
	if (s == "foot" || s == "ground" || s == "walk") return MoveType::foot;
	if (s == "water" || s == "float" || s == "naval") return MoveType::water;
	if (s == "amphibious" || s == "amph") return MoveType::amphibious;
	if (s == "fly" || s == "air") return MoveType::fly;
	return std::nullopt;
}

std::string move_type_name(MoveType mt) {
	switch (mt) {
		case MoveType::foot: return "foot";
		case MoveType::water: return "water";
		case MoveType::amphibious: return "amphibious";
		case MoveType::fly: return "fly";
	}
	return "foot";
}

struct Grid {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> cells; // war3map.wpm flags, size width*height
	// Per-cell water flag taken from the terrain (war3map.w3e), NOT the unreliable
	// wpm water bit. Empty when the terrain could not be read (then we fall back to
	// the wpm bit). Size width*height when present.
	std::vector<uint8_t> water_mask;
	bool has_water_mask = false;
	// Terrain centre offset from war3map.w3e, used for world <-> cell conversion.
	float offset_x = 0.f;
	float offset_y = 0.f;
	bool has_offset = false;

	uint8_t at(int x, int y) const { return cells[static_cast<std::size_t>(y) * width + x]; }
	bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
	int index(int x, int y) const { return y * width + x; }

	bool is_water(int x, int y) const {
		if (has_water_mask) {
			return water_mask[static_cast<std::size_t>(y) * width + x] != 0;
		}
		return (at(x, y) & water) != 0; // fallback to the (unreliable) wpm bit
	}

	// Whether the given movement type can occupy cell (x, y). The water surface comes
	// from the terrain; ground/air blocking comes from the wpm. Deep water is already
	// flagged unwalkable in the wpm, so foot = "not unwalkable" (it includes shallow
	// wadeable water and excludes deep ocean).
	bool passable(int x, int y, MoveType mt) const {
		const uint8_t cell = at(x, y);
		switch (mt) {
			case MoveType::foot: return (cell & unwalkable) == 0;
			case MoveType::water: return is_water(x, y);
			case MoveType::amphibious: return is_water(x, y) || (cell & unwalkable) == 0;
			case MoveType::fly: return (cell & unflyable) == 0;
		}
		return false;
	}

	// World units -> pathing cell (rounded). Requires has_offset.
	int world_to_cell_x(float wx) const { return static_cast<int>(std::lround((wx - offset_x) / 32.f)); }
	int world_to_cell_y(float wy) const { return static_cast<int>(std::lround((wy - offset_y) / 32.f)); }
	// Cell centre -> world units.
	float cell_to_world_x(int cx) const { return offset_x + (cx + 0.5f) * 32.f; }
	float cell_to_world_y(int cy) const { return offset_y + (cy + 0.5f) * 32.f; }
};

// Reads the terrain (war3map.w3e): the centre offset (for world<->cell) and the
// authoritative per-tile water flag, which it rasterises onto the pathing grid
// (4x resolution). The wpm "water" bit is unreliable — open ocean is often not
// flagged there and some land is — so the terrain flag is the correct source for
// where water actually is. Best-effort: on failure has_offset/has_water_mask stay
// false and callers fall back.
inline void load_terrain_water(Grid& grid) {
	auto result = hierarchy.map_file_read("war3map.w3e");
	if (!result.has_value()) {
		return;
	}
	try {
		BinaryReader reader = std::move(result.value());
		if (reader.read_string(4) != "W3E!") {
			return;
		}
		const uint32_t version = reader.read<uint32_t>();
		(void)reader.read<char>();        // tileset
		reader.advance(4);                // custom tileset flag
		const uint32_t ground_textures = reader.read<uint32_t>();
		for (uint32_t i = 0; i < ground_textures; ++i) (void)reader.read_string(4);
		const uint32_t cliff_textures = reader.read<uint32_t>();
		for (uint32_t i = 0; i < cliff_textures; ++i) (void)reader.read_string(4);
		const uint32_t tw = reader.read<uint32_t>(); // corner columns
		const uint32_t th = reader.read<uint32_t>(); // corner rows
		grid.offset_x = reader.read<float>();
		grid.offset_y = reader.read<float>();
		grid.has_offset = true;

		if (tw == 0 || th == 0) {
			return;
		}

		// Per-corner water flag.
		std::vector<uint8_t> tile_water(static_cast<std::size_t>(tw) * th, 0);
		for (std::size_t i = 0; i < static_cast<std::size_t>(tw) * th; ++i) {
			(void)reader.read<uint16_t>(); // ground height
			(void)reader.read<uint16_t>(); // water height + edge flag
			if (version >= 11) {
				const uint16_t flags = reader.read<uint16_t>();
				tile_water[i] = (flags & 0x0100) ? 1 : 0;
			} else {
				const uint8_t flags = reader.read<uint8_t>();
				tile_water[i] = (flags & 0x40) ? 1 : 0;
			}
			(void)reader.read<uint8_t>();  // variation
			(void)reader.read<uint8_t>();  // misc (cliff texture / layer height)
		}

		// Rasterise onto the 4x pathing grid: cell (cx, cy) samples corner (cx/4, cy/4).
		grid.water_mask.assign(static_cast<std::size_t>(grid.width) * grid.height, 0);
		for (int cy = 0; cy < grid.height; ++cy) {
			const std::size_t ty = std::min<std::size_t>(static_cast<std::size_t>(cy) / 4, th - 1);
			for (int cx = 0; cx < grid.width; ++cx) {
				const std::size_t tx = std::min<std::size_t>(static_cast<std::size_t>(cx) / 4, tw - 1);
				grid.water_mask[static_cast<std::size_t>(cy) * grid.width + cx] = tile_water[ty * tw + tx];
			}
		}
		grid.has_water_mask = true;
	} catch (...) {
		grid.has_water_mask = false;
	}
}

// Loads war3map.wpm into a Grid. Returns nullopt and sets `error` on failure.
inline std::optional<Grid> load_grid(std::string& error) {
	auto result = hierarchy.map_file_read("war3map.wpm");
	if (!result.has_value()) {
		error = "could not read war3map.wpm: " + result.error();
		return std::nullopt;
	}

	try {
		BinaryReader reader = std::move(result.value());
		if (reader.read_string(4) != "MP3W") {
			error = "war3map.wpm has an invalid magic number (expected MP3W)";
			return std::nullopt;
		}
		(void)reader.read<uint32_t>(); // version (0)

		Grid grid;
		grid.width = static_cast<int>(reader.read<uint32_t>());
		grid.height = static_cast<int>(reader.read<uint32_t>());
		if (grid.width <= 0 || grid.height <= 0) {
			error = "war3map.wpm has no pathing data (zero dimensions)";
			return std::nullopt;
		}
		grid.cells = reader.read_vector<uint8_t>(static_cast<std::size_t>(grid.width) * grid.height);
		load_terrain_water(grid);
		return grid;
	} catch (const std::exception& e) {
		error = std::string("failed parsing war3map.wpm: ") + e.what();
		return std::nullopt;
	}
}

// Finds the nearest passable cell to (x, y) within `max_radius` (Chebyshev rings).
// Returns the cell itself when already passable, or nullopt if nothing passable is
// found in range. Agents often pass approximate coordinates (a unit position that
// sits on an unwalkable footprint), so snapping makes path queries robust.
inline std::optional<std::pair<int, int>> nearest_passable(const Grid& grid, MoveType mt,
														   int x, int y, int max_radius) {
	if (grid.in_bounds(x, y) && grid.passable(x, y, mt)) {
		return std::pair{ x, y };
	}
	for (int r = 1; r <= max_radius; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring only
				const int nx = x + dx;
				const int ny = y + dy;
				if (grid.in_bounds(nx, ny) && grid.passable(nx, ny, mt)) {
					return std::pair{ nx, ny };
				}
			}
		}
	}
	return std::nullopt;
}

// ---- connected components ("islands") -------------------------------------

struct Component {
	int id = 0;
	std::size_t cell_count = 0;
	int min_x = 0, min_y = 0, max_x = 0, max_y = 0; // inclusive bbox in cells
	double sum_x = 0, sum_y = 0;                     // for centroid (cells)
};

struct Components {
	int count = 0;
	std::vector<int> labels;          // size width*height, -1 = impassable for this move type
	std::vector<Component> components; // sorted by cell_count descending
};

// 4-neighbour connected components over passable cells.
inline Components connected_components(const Grid& grid, MoveType mt) {
	Components out;
	out.labels.assign(static_cast<std::size_t>(grid.width) * grid.height, -1);

	std::vector<Component> comps;
	std::vector<int> stack;
	const int w = grid.width;
	const int h = grid.height;

	for (int start = 0; start < w * h; ++start) {
		if (out.labels[start] != -1 || !grid.passable(start % w, start / w, mt)) {
			continue;
		}
		const int label = static_cast<int>(comps.size());
		Component c;
		c.id = label;
		c.min_x = c.max_x = start % w;
		c.min_y = c.max_y = start / w;

		stack.push_back(start);
		out.labels[start] = label;
		while (!stack.empty()) {
			const int idx = stack.back();
			stack.pop_back();
			const int x = idx % w;
			const int y = idx / w;
			c.cell_count++;
			c.sum_x += x;
			c.sum_y += y;
			c.min_x = std::min(c.min_x, x);
			c.min_y = std::min(c.min_y, y);
			c.max_x = std::max(c.max_x, x);
			c.max_y = std::max(c.max_y, y);

			const int nx[4] = { x - 1, x + 1, x, x };
			const int ny[4] = { y, y, y - 1, y + 1 };
			for (int n = 0; n < 4; ++n) {
				if (!grid.in_bounds(nx[n], ny[n])) continue;
				const int nidx = grid.index(nx[n], ny[n]);
				if (out.labels[nidx] == -1 && grid.passable(nx[n], ny[n], mt)) {
					out.labels[nidx] = label;
					stack.push_back(nidx);
				}
			}
		}
		comps.push_back(c);
	}

	std::sort(comps.begin(), comps.end(), [](const Component& a, const Component& b) {
		return a.cell_count > b.cell_count;
	});
	out.components = std::move(comps);
	out.count = static_cast<int>(out.components.size());
	return out;
}

// ---- A* pathfinding -------------------------------------------------------

struct Portal {
	int from_x, from_y, to_x, to_y;
};

struct PathResult {
	bool reachable = false;
	double cost = 0.0;                       // in cells (orthogonal = 1, diagonal = sqrt2)
	std::vector<std::pair<int, int>> cells;  // simplified waypoints (cell coords)
	bool used_portal = false;
};

// 8-neighbour A* with corner-cutting prevention and optional portal edges.
inline PathResult find_path(const Grid& grid, MoveType mt, int sx, int sy, int gx, int gy,
							 const std::vector<Portal>& portals = {}) {
	PathResult res;
	const int w = grid.width;
	const int h = grid.height;
	if (!grid.in_bounds(sx, sy) || !grid.in_bounds(gx, gy)) {
		return res;
	}
	if (!grid.passable(sx, sy, mt) || !grid.passable(gx, gy, mt)) {
		return res;
	}

	// Portal lookup by source cell index (bidirectional).
	std::unordered_map<int, std::vector<int>> portal_edges;
	for (const auto& p : portals) {
		if (!grid.in_bounds(p.from_x, p.from_y) || !grid.in_bounds(p.to_x, p.to_y)) continue;
		const int a = grid.index(p.from_x, p.from_y);
		const int b = grid.index(p.to_x, p.to_y);
		portal_edges[a].push_back(b);
		portal_edges[b].push_back(a);
	}

	const int start = grid.index(sx, sy);
	const int goal = grid.index(gx, gy);
	const std::size_t n = static_cast<std::size_t>(w) * h;

	std::vector<double> g_score(n, std::numeric_limits<double>::infinity());
	std::vector<int> came_from(n, -1);
	std::vector<char> closed(n, 0);
	std::vector<char> via_portal(n, 0);

	auto heuristic = [&](int idx) {
		const int x = idx % w, y = idx / w;
		const double dx = std::abs(x - gx), dy = std::abs(y - gy);
		// Octile distance.
		return (dx + dy) + (std::numbers::sqrt2 - 2.0) * std::min(dx, dy);
	};

	using Node = std::pair<double, int>; // (f_score, index)
	std::priority_queue<Node, std::vector<Node>, std::greater<>> open;
	g_score[start] = 0.0;
	open.push({ heuristic(start), start });

	static constexpr int dx8[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
	static constexpr int dy8[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };

	while (!open.empty()) {
		const int current = open.top().second;
		open.pop();
		if (closed[current]) continue;
		closed[current] = 1;
		if (current == goal) break;

		const int cx = current % w;
		const int cy = current / w;

		for (int d = 0; d < 8; ++d) {
			const int nx = cx + dx8[d];
			const int ny = cy + dy8[d];
			if (!grid.in_bounds(nx, ny) || !grid.passable(nx, ny, mt)) continue;
			const bool diagonal = d >= 4;
			if (diagonal) {
				// Prevent cutting across a blocked corner.
				if (!grid.passable(cx, ny, mt) || !grid.passable(nx, cy, mt)) continue;
			}
			const int nidx = grid.index(nx, ny);
			if (closed[nidx]) continue;
			const double step = diagonal ? std::numbers::sqrt2 : 1.0;
			const double tentative = g_score[current] + step;
			if (tentative < g_score[nidx]) {
				g_score[nidx] = tentative;
				came_from[nidx] = current;
				via_portal[nidx] = 0;
				open.push({ tentative + heuristic(nidx), nidx });
			}
		}

		// Portal edges from the current cell (small fixed cost).
		if (const auto it = portal_edges.find(current); it != portal_edges.end()) {
			for (const int nidx : it->second) {
				if (closed[nidx] || !grid.passable(nidx % w, nidx / w, mt)) continue;
				const double tentative = g_score[current] + 1.0;
				if (tentative < g_score[nidx]) {
					g_score[nidx] = tentative;
					came_from[nidx] = current;
					via_portal[nidx] = 1;
					open.push({ tentative + heuristic(nidx), nidx });
				}
			}
		}
	}

	if (!closed[goal] && came_from[goal] == -1 && goal != start) {
		return res; // unreachable
	}

	// Reconstruct, then simplify collinear runs into waypoints.
	std::vector<int> raw;
	for (int cur = goal; cur != -1; cur = came_from[cur]) {
		raw.push_back(cur);
		if (via_portal[cur]) res.used_portal = true;
		if (cur == start) break;
	}
	std::reverse(raw.begin(), raw.end());
	if (raw.empty() || raw.front() != start) {
		return res; // failed to reconstruct
	}

	res.reachable = true;
	res.cost = g_score[goal];

	auto cell_xy = [&](int idx) { return std::pair<int, int>{ idx % w, idx / w }; };
	for (std::size_t i = 0; i < raw.size(); ++i) {
		if (i == 0 || i + 1 == raw.size()) {
			res.cells.push_back(cell_xy(raw[i]));
			continue;
		}
		const auto [px, py] = cell_xy(raw[i - 1]);
		const auto [cx, cy] = cell_xy(raw[i]);
		const auto [nx, ny] = cell_xy(raw[i + 1]);
		// Keep the point only if the direction changes (or a portal jump happened).
		const bool dir_change = (cx - px) * (ny - cy) != (cy - py) * (nx - cx);
		if (dir_change || via_portal[raw[i]]) {
			res.cells.push_back(cell_xy(raw[i]));
		}
	}
	return res;
}

} // namespace pathing
