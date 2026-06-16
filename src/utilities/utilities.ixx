export module Utilities;

import std;
import <glm/glm.hpp>;
import BinaryReader;
import no_init_allocator;
import types;

namespace fs = std::filesystem;

// String functions
export std::string_view trimmed(const std::string_view string) {
	size_t start = 0;
	while (start < string.size() && std::isspace(string[start])) {
		start += 1;
	}

	size_t end = string.size();
	while (end > start && std::isspace(string[end - 1])) {
		end -= 1;
	}

	return string.substr(start, end - start);
}

export std::string string_replaced(const std::string_view source, const std::string_view from, const std::string_view to) {
	std::string new_string;
	new_string.reserve(source.length()); // avoids a few memory allocations

	size_t lastPos = 0;
	size_t findPos;

	while (std::string::npos != (findPos = source.find(from, lastPos))) {
		new_string.append(source, lastPos, findPos - lastPos);
		new_string += to;
		lastPos = findPos + from.length();
	}

	// Care for the rest after last occurrence
	new_string += source.substr(lastPos);

	return new_string;
}

export std::string to_lowercase_copy(const std::string_view string) {
	std::string output(string);
	std::ranges::transform(output, output.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
	return output;
}

export void to_lowercase(std::string& string) {
	std::ranges::transform(string, string.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
}

export void normalize_path_to_backslash(std::string& path) {
	std::ranges::transform(path, path.begin(), [](const unsigned char c) {
		return c == '/' ? '\\' : c;
	});
}

export void normalize_path_to_forward_slash(std::string& path) {
	std::ranges::transform(path, path.begin(), [](const unsigned char c) {
		return c == '\\' ? '/' : c;
	});
}

// trim from start (in place)
export void ltrim(std::string& s) {
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](const int ch) {
				return !std::isspace(ch);
			}));
}

// trim from end (in place)
export void rtrim(std::string& s) {
	s.erase(
		std::find_if(
			s.rbegin(),
			s.rend(),
			[](const int ch) {
				return !std::isspace(ch);
			}
		).base(),
		s.end()
	);
}

// trim from both ends (in place)
export void trim(std::string& s) {
	ltrim(s);
	rtrim(s);
}

export bool is_number(const std::string& s) {
	return !s.empty()
		&& std::find_if(
			   s.begin(),
			   s.end(),
			   [](const char c) {
				   return !std::isdigit(c);
			   }
		   )
		== s.end();
}

export std::vector<std::string> split_string_escaped(const std::string_view input) {
	std::vector<std::string> result;
	// Pre-allocate space for some fields to avoid initial reallocations
	result.reserve(std::count(input.begin(), input.end(), ',') + 1);

	const char* start = input.data();
	const char* curr = start;
	const char* end = start + input.size();
	bool in_quotes = false;

	while (curr < end) {
		if (*curr == '"') {
			in_quotes = !in_quotes;
		} else if (*curr == ',' && !in_quotes) {
			// Create the string in one shot from the range
			result.emplace_back(start, curr - start);
			start = curr + 1;
		}
		curr++;
	}

	// Add the final piece
	result.emplace_back(start, end - start);
	return result;
}

// Validates that a byte sequence is well-formed UTF-8 (rejecting overlong
// encodings). Used to decide whether a localized object-data string is already
// UTF-8 or still in the map's legacy CP1251 encoding.
export bool is_valid_utf8(std::string_view sv) noexcept {
	for (std::size_t i = 0; i < sv.size();) {
		const unsigned char c = static_cast<unsigned char>(sv[i]);
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
		if (len == 2) {
			const unsigned int cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(sv[i + 1]) & 0x3F);
			if (cp < 0x80) return false;
		} else if (len == 3) {
			const unsigned int cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(sv[i + 1]) & 0x3F) << 6)
				| (static_cast<unsigned char>(sv[i + 2]) & 0x3F);
			if (cp < 0x800) return false;
		} else if (len == 4) {
			const unsigned int cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(sv[i + 1]) & 0x3F) << 12)
				| ((static_cast<unsigned char>(sv[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(sv[i + 3]) & 0x3F);
			if (cp < 0x10000) return false;
		}
		i += len;
	}
	return true;
}

// Returns a UTF-8 string. If the input is already valid UTF-8 it passes through
// unchanged; otherwise it is assumed to be Windows-1251 (the legacy encoding of
// Russian/localized WC3 maps) and transcoded. Lets the headless data layer emit
// correct unit/ability names regardless of how the map stored them.
export std::string to_utf8(std::string_view sv) {
	if (sv.empty() || is_valid_utf8(sv)) {
		return std::string(sv);
	}
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
	std::string out;
	out.reserve(sv.size() + sv.size() / 2);
	for (unsigned char b : sv) {
		if (b < 0x80) {
			out.push_back(static_cast<char>(b));
		} else {
			const unsigned short u = cp1251_uni[b - 0x80];
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

export std::string read_text_file(const fs::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);

	if (!file) {
		return "";
	}

	const auto size = file.tellg();
	if (size <= 0) {
		return "";
	}

	std::string text;
	text.resize(size);

	file.seekg(0, std::ios::beg);
	file.read(text.data(), size);

	return text;
}

export auto read_file(const fs::path& path) -> std::expected<BinaryReader, std::string> {
	// Open at the end (ate) to get size in one go
	std::ifstream stream(path, std::ios::binary | std::ios::ate);

	if (!stream.is_open()) {
		return std::unexpected("Unable to open file: " + path.string());
	}

	const auto size = static_cast<std::size_t>(stream.tellg());

	// Handle empty files gracefully
	if (size == 0) {
		return BinaryReader(std::vector<u8, default_init_allocator<u8>> {});
	}

	// Rewind to beginning
	stream.seekg(0, std::ios::beg);

	std::vector<u8, default_init_allocator<u8>> buffer(size);

	// Use the stream buffer directly for maximum throughput
	if (!stream.read(reinterpret_cast<char*>(buffer.data()), size)) {
		return std::unexpected("Error during read of file: " + path.string());
	}

	return BinaryReader(std::move(buffer));
}

export struct ItemSet {
	std::vector<std::pair<int, std::string>> items;
};

// Returns 1 or -1
export glm::vec2 sign_not_zero(glm::vec2 v) {
	return glm::vec2((v.x >= 0.f) ? +1.f : -1.f, (v.y >= 0.f) ? +1.f : -1.f);
}

// Assume normalized input. Output is on [-1, 1] for each component.
export glm::vec2 float32x3_to_oct(glm::vec3 v) {
	// Project the sphere onto the octahedron, and then onto the xy plane
	glm::vec2 p = glm::vec2(v) * (1.f / (std::abs(v.x) + std::abs(v.y) + std::abs(v.z)));
	// Reflect the folds of the lower hemisphere over the diagonals
	return (v.z <= 0.f) ? ((1.f - glm::abs(glm::vec2(p.y, p.x))) * sign_not_zero(p)) : p;
}

/// 21 bits per component ~= 2,097,152 distinct values
/// With an extent of 4096 that would give a precision of ~0.0019
export glm::uvec2 pack_vec3_to_uvec2(const glm::vec3 v, const float extent) {
	glm::vec3 normalized = glm::clamp(v / extent, glm::vec3(-1.0f), glm::vec3(1.0f));
	normalized = (normalized + 1.0f) * 0.5f;

	const uint64_t x = normalized.x * static_cast<float>((1ull << 21) - 1); // 21 bits
	const uint64_t y = normalized.y * static_cast<float>((1ull << 21) - 1); // 21 bits
	const uint64_t z = normalized.z * static_cast<float>((1ull << 22) - 1); // 22 bits
	const uint64_t packed = x << 43 | y << 22 | z;

	return glm::uvec2(packed & 0xFFFFFFFF, packed >> 32);
}

// From http://www.jcgt.org/published/0006/02/01/
export bool intersect_aabb(const glm::vec3& aabb_min, const glm::vec3& aabb_max, const glm::vec3& origin, const glm::vec3& direction) {
	const glm::vec3 t1 = (aabb_min - origin) / direction;
	const glm::vec3 t2 = (aabb_max - origin) / direction;
	const glm::vec3 tmin = glm::min(t1, t2);
	const glm::vec3 tmax = glm::max(t1, t2);

	return glm::min(tmax.x, glm::min(tmax.y, tmax.z)) > glm::max(glm::max(tmin.x, 0.f), glm::max(tmin.y, tmin.z));
}

export bool intersect_sphere(const glm::vec3& origin, const glm::vec3& direction, glm::vec3 position, float radius) {
	glm::vec3 op = position - origin;
	float b = glm::dot(op, direction);
	float disc = b * b - dot(op, op) + radius * radius;
	return disc >= 0.0f;
}

// Only works with uniform scaling
export void
transform_aabb_uniform(const glm::vec3& min, const glm::vec3& max, glm::vec3& new_min, glm::vec3& new_max, const glm::mat4& transform) {
	new_min = transform[3];
	new_max = transform[3];
	for (size_t i = 0; i < 3; i++) {
		for (size_t j = 0; j < 3; j++) {
			float e = transform[i][j] * min[j];
			float f = transform[i][j] * max[j];
			if (e < f) {
				new_min[i] += e;
				new_max[i] += f;
			} else {
				new_min[i] += f;
				new_max[i] += e;
			}
		}
	}
}

// Works with non uniform scaling. For uniform scaling use the faster transform_aabb_uniform()
export void
transform_aabb_non_uniform(const glm::vec3& min, const glm::vec3& max, glm::vec3& new_min, glm::vec3& new_max, const glm::mat4& transform) {
	glm::vec4 p1 = transform * glm::vec4(min, 1.f);
	glm::vec4 p2 = transform * glm::vec4(max.x, min.y, min.z, 1.f);
	glm::vec4 p3 = transform * glm::vec4(max.x, max.y, min.z, 1.f);
	glm::vec4 p4 = transform * glm::vec4(min.x, max.y, min.z, 1.f);
	glm::vec4 p5 = transform * glm::vec4(min.x, min.y, max.z, 1.f);
	glm::vec4 p6 = transform * glm::vec4(max.x, min.y, max.z, 1.f);
	glm::vec4 p7 = transform * glm::vec4(max, 1.f);
	glm::vec4 p8 = transform * glm::vec4(min.x, max.y, max.z, 1.f);

	new_min = glm::min(p1, glm::min(p2, glm::min(p3, glm::min(p4, glm::min(p5, glm::min(p6, glm::min(p7, p8)))))));
	new_max = glm::max(p1, glm::max(p2, glm::max(p3, glm::max(p4, glm::max(p5, glm::max(p6, glm::max(p7, p8)))))));
}

// Best-effort guess of the Warcraft III install directory from well-known
// filesystem locations. Qt-free so the headless data layer (and CLI) can reuse
// it. The GUI prefers a user-configured path (QSettings "warcraftDirectory")
// and only falls back to this probe; that lookup lives in main.cpp.
// True when `directory` plausibly holds an installed Warcraft III (CASC). The
// same three locations open_casc() probes; cheap filesystem check, no CASC open.
export bool looks_like_warcraft_directory(const fs::path& directory) {
	if (directory.empty()) {
		return false;
	}
	std::error_code ec;
	return fs::exists(directory / ".build.info", ec)
		|| fs::exists(directory / "Data", ec)
		|| fs::exists(directory / "_retail_", ec);
}

// Well-known filesystem locations to probe for a Warcraft III install, in
// preference order. Registry-based detection (custom install dirs) is layered on
// top by the GUI; this stays Qt-free so the headless data layer/CLI can reuse it.
export std::vector<fs::path> find_warcraft_directories() {
	std::vector<fs::path> result;
	for (const fs::path candidate : {
			 "C:/Program Files/Warcraft III",
			 "C:/Program Files (x86)/Warcraft III",
			 "C:/Program Files/Warcraft III Public Test",
		 }) {
		std::error_code ec;
		if (fs::exists(candidate, ec)) {
			result.push_back(candidate);
		}
	}
	return result;
}

export fs::path find_warcraft_directory() {
	const auto candidates = find_warcraft_directories();
	return candidates.empty() ? fs::path{} : candidates.front();
}

// Read the installed game version from `<directory>/.build.info`. That file is a
// '|'-delimited table whose header names columns as "Name!TYPE:size"; we locate
// the "Version" column and return it for the active (Active==1) record, falling
// back to the first record. Returns "" when the file is absent/unparseable (e.g.
// a classic, pre-Reforged MPQ install that has no .build.info). Lets the GUI show
// which version a candidate is so the user can reject an old/wrong one.
export std::string read_warcraft_version(const fs::path& directory) {
	std::ifstream file(directory / ".build.info");
	if (!file) {
		return "";
	}

	const auto split = [](const std::string& line) {
		std::vector<std::string> fields;
		size_t start = 0;
		while (true) {
			const size_t bar = line.find('|', start);
			std::string field = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
			fields.push_back(std::string{ trimmed(field) });
			if (bar == std::string::npos) {
				break;
			}
			start = bar + 1;
		}
		return fields;
	};

	std::string header;
	if (!std::getline(file, header)) {
		return "";
	}

	const std::vector<std::string> columns = split(header);
	int version_col = -1;
	int active_col = -1;
	for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
		const std::string name{ columns[i].substr(0, columns[i].find('!')) };
		if (name == "Version") {
			version_col = i;
		} else if (name == "Active") {
			active_col = i;
		}
	}
	if (version_col < 0) {
		return "";
	}

	std::string fallback;
	std::string line;
	while (std::getline(file, line)) {
		if (trimmed(line).empty()) {
			continue;
		}
		const std::vector<std::string> fields = split(line);
		if (version_col >= static_cast<int>(fields.size())) {
			continue;
		}
		const std::string& version = fields[version_col];
		if (active_col >= 0 && active_col < static_cast<int>(fields.size()) && fields[active_col] == "1") {
			return version;
		}
		if (fallback.empty()) {
			fallback = version;
		}
	}
	return fallback;
}