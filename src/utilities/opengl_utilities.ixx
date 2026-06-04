module;

#include <QPainter>
#include <QIcon>

export module OpenGLUtilities;

import std;
import ResourceManager;
import Texture;
import <glad/glad.h>;
import <glm/glm.hpp>;

namespace fs = std::filesystem;

export class Shapes {
  public:
	void init() {
		glCreateBuffers(1, &vertex_buffer);
		glNamedBufferData(vertex_buffer, quad_vertices.size() * sizeof(glm::vec2), quad_vertices.data(), GL_STATIC_DRAW);

		glCreateBuffers(1, &index_buffer);
		glNamedBufferData(index_buffer, quad_indices.size() * sizeof(unsigned int) * 3, quad_indices.data(), GL_STATIC_DRAW);
	}

	GLuint vertex_buffer;
	GLuint index_buffer;

	const std::vector<glm::vec2> quad_vertices = {
		{ 1, 1 },
		{ 0, 1 },
		{ 0, 0 },
		{ 1, 0 }
	};

	const std::vector<glm::uvec3> quad_indices = {
		{ 0, 1, 2 },
		{ 2, 3, 0 }
	};
};

export inline Shapes shapes;

/// Convert a Tground texture into an QIcon with two states
export QIcon ground_texture_to_icon(uint8_t* data, const int width, const int height) {
	QImage temp_image = QImage(data, width, height, QImage::Format::Format_RGBA8888);
	const int size = height / 4;

	auto pix = QPixmap::fromImage(temp_image.copy(0, 0, size, size));

	QIcon icon;
	icon.addPixmap(pix, QIcon::Normal, QIcon::Off);

	QPainter painter(&pix);
	painter.fillRect(0, 0, size, size, QColor(255, 255, 0, 64));
	painter.end();

	icon.addPixmap(pix, QIcon::Normal, QIcon::On);

	return icon;
}

/// Loads a texture from the hierarchy and returns an icon
export QIcon texture_to_icon(const fs::path& path) {
	if (path.empty()) {
		return {};
	}

	if (const auto tex = resource_manager.load<Texture>(path); tex) {
		const QImage temp_image = QImage(
			tex.value()->data.data(),
			tex.value()->width,
			tex.value()->height,
			tex.value()->channels == 3 ? QImage::Format::Format_RGB888 : QImage::Format::Format_RGBA8888
		);
		const auto pix = QPixmap::fromImage(temp_image);
		return QIcon(pix);
	}

	return {};
};
