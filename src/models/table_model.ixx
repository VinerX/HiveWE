module;

#include <QAbstractTableModel>

export module TableModel;

import std;
import Globals;
import QIconResource;
import SLK;
import TriggerStrings;
import QIconResource;
import Hierarchy;
import ResourceManager;
import UnorderedMap;
import <absl/strings/str_split.h>;
import <absl/strings/str_join.h>;

namespace fs = std::filesystem;

hive::unordered_map<std::string, std::shared_ptr<QIconResource>> path_to_icon;

// Decode raw SLK bytes to a QString, tolerating both UTF-8 (Reforged) and
// legacy 8-bit (e.g. Windows-1251 for Russian classic maps). If UTF-8 decoding
// yields no characters or the replacement character, fall back to CP1251.
static QString decode_cp1251_text(std::string_view sv) {
	static constexpr char32_t cp1251_table[128] = {
		0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
		0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
		0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
		0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
		0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
		0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
		0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
		0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
		0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
		0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
		0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
		0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
		0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
		0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
		0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
	};

	QString out;
	out.reserve(static_cast<int>(sv.size()));
	for (unsigned char ch : sv) {
		if (ch < 0x80) {
			out.append(QChar(ch));
			continue;
		}
		const char32_t codepoint = cp1251_table[ch - 0x80];
		if (codepoint == 0) {
			out.append(QChar::ReplacementCharacter);
			continue;
		}
		out.append(QString::fromUcs4(&codepoint, 1));
	}
	return out;
}

static QString decode_slk_text(std::string_view sv) {
	if (sv.empty()) return {};
	QString u = QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
	if (u.isEmpty() || u.contains(QChar(0xFFFD))) {
		return decode_cp1251_text(sv);
	}
	return u;
}

export class TableModel;

export inline TableModel* units_table;
export inline TableModel* items_table;
export inline TableModel* abilities_table;
export inline TableModel* doodads_table;
export inline TableModel* destructibles_table;
export inline TableModel* upgrade_table;
export inline TableModel* buff_table;

export class TableModel : public QAbstractTableModel {
	std::shared_ptr<QIconResource> invalid_icon;

  public:
	slk::SLK* meta_slk;
	slk::SLK* slk;
	TriggerStrings* trigger_strings;

	explicit TableModel(slk::SLK* slk, slk::SLK* meta_slk, TriggerStrings* trigger_strings, QObject* parent = nullptr)
		: QAbstractTableModel(parent), slk(slk), meta_slk(meta_slk), trigger_strings(trigger_strings) {

		invalid_icon = resource_manager.load<QIconResource>("ReplaceableTextures/WorldEditUI/DoodadPlaceholder.dds").value();
	}

	[[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override {
		return slk->rows();
	}

	[[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override {
		return slk->columns();
	}

	[[nodiscard]] QVariant data(const QModelIndex& index, const int role = Qt::DisplayRole) const override {
		if (!index.isValid()) {
			return {};
		}

		const std::string& id = slk->index_to_row.at(index.row());
		const std::string& field = slk->index_to_column.at(index.column());
		return data(id, field, role);
	}

	[[nodiscard]] QVariant data(const std::string_view id, const std::string_view field, const int role = Qt::DisplayRole) const {
		switch (role) {
			case Qt::DisplayRole: {
				const std::string_view field_data = slk->data<std::string_view>(field, id);
				const std::string_view meta_id = slk->field_to_meta_id(*meta_slk, field, id).value();
				const std::string_view type = meta_slk->data<std::string_view>("type", meta_id);
				if (type == "string" || type == "stringList") {
					QString qt_string;
					if (field_data.starts_with("TRIGSTR")) {
						const auto data = trigger_strings->string(field_data);
						qt_string = decode_slk_text(data);
					} else {
						qt_string = decode_slk_text(field_data);
					}

					qt_string.replace("|n", "\n");
					return qt_string;
				} else if (type == "bool") {
					return field_data == "1";
				} else if (type == "unitList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());

					QStringList result;
					for (const auto& part : parts) {
						result.append(units_table->data(part, "name", role).toString());
					}
					return result.join('\n');
				} else if (type == "abilityList" || type == "abilitySkinList" || type == "heroAbilityList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());

					QStringList result;
					for (const auto& part : parts) {
						result.append(abilities_table->data(part, "name", role).toString());
					}
					return result.join('\n');
				} else if (type == "upgradeList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());

					QStringList result;
					for (const auto& part : parts) {
						result.append(upgrade_table->data(part, "name1", role).toString());
					}
					return result.join('\n');
				} else if (type == "buffList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());

					QStringList result;
					for (const auto& part : parts) {
						QString editor_name = buff_table->data(part, "editorname", role).toString();
						if (editor_name.isEmpty()) {
							result += buff_table->data(part, "bufftip", role).toString();
						} else {
							result += editor_name;
						}
					}
					return result.join('\n');
				} else if (type == "techList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());

					QStringList result;
					for (const auto& part : parts) {
						if (units_slk.row_headers.contains(part)) {
							result += units_table->data(part, "name", role).toString();
						} else if (upgrade_slk.row_headers.contains(part)) {
							result += upgrade_table->data(part, "name1", role).toString();
						} else {
							result += QString::fromStdString(std::string(part));
						}
					}
					return result.join('\n');
				} else if (type == "targetList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());
					QStringList result;
					for (size_t i = 0; i < parts.size(); i++) {
						for (const auto& [key, value] : unit_editor_data.section(type)) {
							if (key == "NumValues" || key == "Sort" || key.ends_with("_Alt")) {
								continue;
							}

							if (value[0] == parts[i]) {
								result += QString::fromStdString(value[1]);
							}
						}
					}
					QString result_qstring = result.join(", ");
					result_qstring.replace('&', "");
					return result_qstring;
				} else if (type == "tilesetList") {
					std::vector<std::string_view> parts = absl::StrSplit(field_data, ',', absl::SkipEmpty());
					QStringList result;
					for (const auto& part : parts) {
						if (part == "*") {
							result += "All";
						} else {
							result += QString::fromStdString(world_edit_data.data<std::string>("TileSets", part));
						}
					}
					return result.join(", ");
				} else if (unit_editor_data.section_exists(type)) {
					for (const auto& [key, value] : unit_editor_data.section(type)) {
						if (key == "NumValues" || key == "Sort" || key.ends_with("_Alt")) {
							continue;
						}

						if (slk->data<std::string_view>(field, id) == value[0]) {
							QString displayText = QString::fromStdString(value[1]);
							displayText.replace('&', "");
							return displayText;
						}
					}
				} else if (type == "doodadCategory") {
					for (auto&& [key, value] : world_edit_data.section("DoodadCategories")) {
						if (field_data == key) {
							return QString::fromStdString(value[0]);
						}
					}
				} else if (type == "destructableCategory") {
					for (auto&& [key, value] : world_edit_data.section("DestructibleCategories")) {
						if (field_data == key) {
							return QString::fromStdString(value[0]);
						}
					}
				}

				return decode_slk_text(field_data);
			}
			case Qt::EditRole: {
				const std::string_view raw = slk->data<std::string_view>(field, id);
				if (raw.starts_with("TRIGSTR")) {
					return decode_slk_text(trigger_strings->string(raw));
				}
				return decode_slk_text(raw);
			}
			case Qt::CheckStateRole: {
				const std::string_view meta_id = slk->field_to_meta_id(*meta_slk, field, id).value();
				const std::string_view type = meta_slk->data<std::string_view>("type", meta_id);
				if (type != "bool") {
					return {};
				}

				return (slk->data<std::string_view>(field, id) == "1") ? Qt::Checked : Qt::Unchecked;
			}
			case Qt::DecorationRole:
				const std::string_view meta_id = slk->field_to_meta_id(*meta_slk, field, id).value();
				const std::string_view type = meta_slk->data<std::string_view>("type", meta_id);
				if (type != "icon") {
					return {};
				}

				const std::string_view icon = slk->data<std::string_view>(field, id);
				if (icon.empty()) {
					return invalid_icon->icon;
				}

				if (path_to_icon.contains(icon)) {
					return path_to_icon.at(icon)->icon;
				}

				fs::path icon_path = icon;
				if (!hierarchy.file_exists(icon)) {
					icon_path.replace_extension(".dds");
					if (!hierarchy.file_exists(icon_path)) {
						return invalid_icon->icon;
					}
				}

				path_to_icon[icon_path.string()] = resource_manager.load<QIconResource>(icon_path).value();
				return path_to_icon.at(icon_path.string())->icon;
		}

		return {};
	}

	bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override {
		if (!index.isValid()) {
			return {};
		}

		switch (role) {
			case Qt::EditRole: {
				const std::string& row_id = slk->index_to_row.at(index.row());
				const std::string& col_field = slk->index_to_column.at(index.column());
				const std::string raw = std::string(slk->data<std::string_view>(col_field, row_id));
				if (raw.starts_with("TRIGSTR")) {
					std::string key = raw;
					trigger_strings->set_string(key, value.toString().toStdString());
				} else {
					slk->set_shadow_data(index.column(), index.row(), value.toString().toStdString());
				}
				emit dataChanged(index, index, { Qt::DisplayRole, Qt::EditRole, Qt::DecorationRole });
				return true;
			}
			case Qt::CheckStateRole: {
				const std::string& id = slk->index_to_row.at(index.row());
				const std::string& field = slk->index_to_column.at(index.column());
				const std::string_view meta_id = slk->field_to_meta_id(*meta_slk, field, id).value();
				const std::string_view type = meta_slk->data<std::string_view>("type", meta_id);
				if (type != "bool") {
					return false;
				}

				slk->set_shadow_data(index.column(), index.row(), (value.toInt() == Qt::Checked) ? "1" : "0");
				emit dataChanged(index, index, { role });
				return true;
			}
		}
		return false;
	}

	QVariant headerData(int section, const Qt::Orientation orientation, const int role = Qt::DisplayRole) const override {
		if (role != Qt::DisplayRole) {
			return {};
		}

		if (orientation == Qt::Orientation::Horizontal) {
			return decode_slk_text(slk->data<std::string_view>(section, 0));
		} else {
			return decode_slk_text(slk->data<std::string_view>(0, section));
		}
	}

	[[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
		if (!index.isValid()) {
			return Qt::NoItemFlags;
		}

		Qt::ItemFlags flags = QAbstractTableModel::flags(index);

		const std::string_view id = slk->index_to_row.at(index.row());
		const std::string_view field = slk->index_to_column.at(index.column());
		const std::string_view meta_id = slk->field_to_meta_id(*meta_slk, field, id).value();
		const std::string_view type = meta_slk->data<std::string_view>("type", meta_id);
		if (type == "bool") {
			flags |= Qt::ItemIsUserCheckable;
		}

		if (!(flags & Qt::ItemIsUserCheckable)) {
			flags |= Qt::ItemIsEditable;
		}

		return flags;
	}

	template<typename F>
	void addRow(F f) {
		beginInsertRows(QModelIndex(), rowCount(), rowCount());
		f();
		endInsertRows();
	}

	void copyRow(std::string_view row_header, std::string_view new_row_header) {
		beginInsertRows(QModelIndex(), rowCount(), rowCount());

		slk->copy_row(row_header, new_row_header, true);
		endInsertRows();
	}

	void deleteRow(const std::string_view row_header) {
		const int row = slk->row_headers.at(row_header);
		beginRemoveRows(QModelIndex(), row, row);
		slk->remove_row(row_header);
		endRemoveRows();
	}

	// Returns the model index belonging to the row with the given id
	QModelIndex rowIDToIndex(const std::string_view id) const {
		return createIndex(slk->row_headers.at(id), 0);
	}
};