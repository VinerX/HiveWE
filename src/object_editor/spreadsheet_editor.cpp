#include "spreadsheet_editor.h"

#include <QHeaderView>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QSettings>
#include <QPainter>
#include <QWheelEvent>
#include <QCheckBox>
#include <QStyleOptionHeader>
#include <QFontMetrics>
#include <QApplication>
#include <QInputDialog>
#include <QDateTime>
#include <QUuid>
#include <functional>
#include <QGroupBox>
#include <QTreeWidget>
#include <QSplitter>
#include <QScrollBar>
#include <QPushButton>
#include <QSharedPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <map>
#include <algorithm>

import std;
import Globals;
import Hierarchy;

// Quiet per-map file holding "in-map" editor-column definitions and their per-row values.
// It is an ordinary file in the map folder (not object data), so it travels with the map,
// is ignored by the game/World Editor, and never touches the modification tables.
static constexpr char kInMapFieldsFile[] = "war3map.hivewe_fields.json";

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

// Decode raw SLK bytes to a QString, tolerating both UTF-8 (Reforged) and local
// 8-bit (e.g. Windows-1251 for Russian classic maps). If UTF-8 decoding yields the
// replacement character the bytes were almost certainly local 8-bit, so fall back.
static QString decode_slk_text(std::string_view sv) {
	if (sv.empty()) return {};
	QString u = QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
	// Some Qt builds truncate at the first invalid byte instead of inserting U+FFFD,
	// so we must also fall back when the result is empty.
	if (u.isEmpty() || u.contains(QChar(0xFFFD))) {
		return decode_cp1251_text(sv);
	}
	return u;
}

static QString normalize_column_key(QString key) {
	return key.trimmed().toLower();
}

static QString format_numeric_value(double value) {
	if (!std::isfinite(value)) {
		return {};
	}
	if (std::abs(value - std::round(value)) < 1e-9) {
		return QString::number(static_cast<qlonglong>(std::llround(value)));
	}
	QString text = QString::number(value, 'f', 4);
	while (text.contains('.') && (text.endsWith('0') || text.endsWith('.'))) {
		text.chop(1);
	}
	return text;
}

static QString stable_formula_key(const QString& title) {
	QString slug;
	slug.reserve(title.size());
	for (const QChar ch : title.toLower()) {
		if (ch.isLetterOrNumber()) {
			slug.append(ch);
		} else if (!slug.endsWith('_')) {
			slug.append('_');
		}
	}
	while (slug.startsWith('_')) slug.remove(0, 1);
	while (slug.endsWith('_')) slug.chop(1);
	if (slug.isEmpty()) {
		slug = "formula";
	}
	return QString("__formula_%1_%2")
		.arg(slug, QString::number(QDateTime::currentMSecsSinceEpoch()));
}

static QString stable_editor_key(const QString& title) {
	QString key = stable_formula_key(title);
	key.replace("__formula_", "__editor_");
	return key;
}

// ============================================================================
// SpreadsheetProxy
// ============================================================================

SpreadsheetProxy::SpreadsheetProxy(QAbstractItemModel* source_model,
                                   slk::SLK* data_slk, slk::SLK* meta_slk,
                                   std::string name_field,
                                   std::string icon_field,
                                   QString category_name,
                                   QObject* parent)
	: QIdentityProxyModel(parent)
	, slk(data_slk)
	, meta_slk(meta_slk)
	, name_field(std::move(name_field))
	, icon_field(std::move(icon_field))
	, category_name(std::move(category_name))
{
	setSourceModel(source_model);

	if (const auto found = slk->column_headers.find(this->name_field);
		found != slk->column_headers.end()) {
		name_column = static_cast<int>(found->second);
	}
	if (!this->icon_field.empty()) {
		if (const auto found = slk->column_headers.find(this->icon_field);
			found != slk->column_headers.end()) {
			icon_column = static_cast<int>(found->second);
		}
	}

	populateBuiltInComputedColumns();
	loadCustomComputedColumns();
	loadInMapColumns();

	beginResetModel();
	rebuildVisibleRows();
	endResetModel();
}

int SpreadsheetProxy::sourceColumnCount() const {
	return sourceModel() ? sourceModel()->columnCount() : 0;
}

bool SpreadsheetProxy::isComputedColumn(int column) const {
	return column >= sourceColumnCount()
		&& column < sourceColumnCount() + static_cast<int>(computed_columns_.size());
}

bool SpreadsheetProxy::isEditorColumn(int column) const {
	return isComputedColumn(column)
		&& computed_columns_.at(static_cast<size_t>(column - sourceColumnCount())).isEditor();
}

bool SpreadsheetProxy::isEditableColumn(int column) const {
	// Real map fields and editor (user-stored) virtual columns are editable;
	// formula/combined virtual columns are read-only.
	return !isComputedColumn(column) || isEditorColumn(column);
}

QString SpreadsheetProxy::columnKey(int column) const {
	if (isComputedColumn(column)) {
		return computed_columns_.at(static_cast<size_t>(column - sourceColumnCount())).key;
	}
	if (!slk || column < 0 || static_cast<size_t>(column) >= slk->index_to_column.size()) {
		return {};
	}
	return QString::fromStdString(slk->index_to_column.at(static_cast<size_t>(column)));
}

QString SpreadsheetProxy::groupName(int column) const {
	if (isComputedColumn(column)) {
		return computed_columns_.at(static_cast<size_t>(column - sourceColumnCount())).group;
	}
	return {};
}

int SpreadsheetProxy::findColumnByKey(const QString& key) const {
	const QString normalized = normalize_column_key(key);
	for (int c = 0; c < columnCount(); ++c) {
		if (columnKey(c) == normalized) {
			return c;
		}
	}
	return -1;
}

void SpreadsheetProxy::rebuildVisibleRows() {
	visible_rows_.clear();
	if (!slk) return;

	const int total = static_cast<int>(slk->index_to_row.size());
	visible_rows_.reserve(total);

	for (int r = 0; r < total; ++r) {
		const std::string& id = slk->index_to_row.at(static_cast<size_t>(r));

		if (custom_only) {
			const auto it = slk->shadow_data.find(id);
			if (it == slk->shadow_data.end() || !it->second.contains("oldid")) continue;
		}

		if (!race_filter.empty() && slk->column_headers.contains("race")) {
			auto race_val = slk->data<std::string_view>("race", id);
			if (race_val != race_filter) continue;
		}

		if (slk->column_headers.contains("isbldg") && (!show_buildings || !show_units)) {
			auto isbldg = slk->data<std::string_view>("isbldg", id);
			bool is_building = (isbldg == "1");
			if (is_building && !show_buildings) continue;
			if (!is_building && !show_units) continue;
		}

		if (!editor_suffix.isEmpty()
			&& (slk->column_headers.contains("editorsuffix") || slk->column_headers.contains("version"))) {
			std::string_view sv;
			int suffix_col = -1;
			if (slk->column_headers.contains("editorsuffix")) {
				sv = slk->data<std::string_view>("editorsuffix", id);
				suffix_col = static_cast<int>(slk->column_headers.at("editorsuffix"));
			}
			if (sv.empty() && slk->column_headers.contains("version")) {
				sv = slk->data<std::string_view>("version", id);
				suffix_col = static_cast<int>(slk->column_headers.at("version"));
			}

			QString qval;
			if (sv.starts_with("TRIGSTR")) {
				// Russian (localized) maps store editor suffixes as a war3map.wts
				// reference like "TRIGSTR_008", not the literal text. The raw SLK value
				// would never contain the suffix the user typed, so resolve it through the
				// source model (EditRole resolves the TRIGSTR and decodes cp1251/UTF-8) —
				// the same path the grid uses to display it.
				qval = sourceModel()->data(sourceModel()->index(r, suffix_col), Qt::EditRole).toString();
			} else {
				qval = decode_slk_text(sv);
			}
			if (!qval.contains(editor_suffix, Qt::CaseInsensitive)) continue;
		}

		if (!field_filter_name.empty() && !field_filter_text.isEmpty() && slk->column_headers.contains(field_filter_name)) {
			auto val = slk->data<std::string_view>(field_filter_name, id);
			QString qval = decode_slk_text(val);
			if (!qval.contains(field_filter_text, Qt::CaseInsensitive)) continue;
		}

		if (!text_filter.isEmpty() && name_column >= 0) {
			const QModelIndex src_idx = sourceModel()->index(r, name_column);
			const QString name = sourceModel()->data(src_idx, Qt::DisplayRole).toString();
			if (!name.contains(text_filter, Qt::CaseInsensitive)) continue;
		}

		visible_rows_.push_back(r);
	}

	if (sort_column >= 0) {
		rebuildSortOrder();
	}
}

void SpreadsheetProxy::rebuildSortOrder() {
	if (sort_column < 0) return;

	std::stable_sort(visible_rows_.begin(), visible_rows_.end(),
		[this](int a, int b) {
			QString va = displayData(a, sort_column).toString();
			QString vb = displayData(b, sort_column).toString();

			bool ok_a = false, ok_b = false;
			double na = va.toDouble(&ok_a);
			double nb = vb.toDouble(&ok_b);

			if (ok_a && ok_b) {
				return sort_order == Qt::AscendingOrder ? na < nb : na > nb;
			}
			int cmp = QString::localeAwareCompare(va, vb);
			return sort_order == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
		});
}

void SpreadsheetProxy::reapplyFilters() {
	beginResetModel();
	rebuildVisibleRows();
	endResetModel();
}

void SpreadsheetProxy::setTextFilter(const QString& text) {
	if (text_filter == text) return;
	text_filter = text;
	reapplyFilters();
}

void SpreadsheetProxy::setCustomOnly(bool only) {
	if (custom_only == only) return;
	custom_only = only;
	reapplyFilters();
}

void SpreadsheetProxy::setRaceFilter(const QString& race_key) {
	auto s = race_key.toStdString();
	if (race_filter == s) return;
	race_filter = std::move(s);
	reapplyFilters();
}

void SpreadsheetProxy::setUnitTypeFilter(bool buildings, bool units) {
	if (show_buildings == buildings && show_units == units) return;
	show_buildings = buildings;
	show_units = units;
	reapplyFilters();
}

void SpreadsheetProxy::setEditorSuffixFilter(const QString& suffix) {
	if (editor_suffix == suffix) return;
	editor_suffix = suffix;
	reapplyFilters();
}

void SpreadsheetProxy::setFieldFilter(const QString& field_name, const QString& text) {
	auto s = field_name.toStdString();
	if (field_filter_name == s && field_filter_text == text) return;
	field_filter_name = std::move(s);
	field_filter_text = text;
	reapplyFilters();
}

void SpreadsheetProxy::populateBuiltInComputedColumns() {
	if (category_name != "Units") {
		return;
	}

	computed_columns_.push_back({
		"__builtin_abilities",
		QString::fromUtf8(u8"Способности"),
		"General",
		{},
		SpreadsheetComputedColumn::Kind::CombinedText,
		true,
	});
	computed_columns_.push_back({
		"__builtin_spellcount",
		QString::fromUtf8(u8"Кол-во способностей"),
		"Computed",
		"count(abillist, heroabillist, abilskinlist, heroabilskinlist)",
		SpreadsheetComputedColumn::Kind::FormulaNumber,
		true,
	});
	computed_columns_.push_back({
		"__builtin_avgattack",
		QString::fromUtf8(u8"Средняя атака"),
		"Combat",
		"dmgplus1 + dice1 * (sides1 + 1) / 2",
		SpreadsheetComputedColumn::Kind::FormulaNumber,
		true,
	});
	computed_columns_.push_back({
		"__builtin_dps",
		"DPS",
		"Combat",
		"(dmgplus1 + dice1 * (sides1 + 1) / 2) / max(cool1, 0.01)",
		SpreadsheetComputedColumn::Kind::FormulaNumber,
		true,
	});
}

void SpreadsheetProxy::loadCustomComputedColumns() {
	if (category_name.isEmpty()) {
		return;
	}

	QSettings settings;
	const QVariantList stored = settings.value("Spreadsheet/formulas/" + category_name).toList();
	for (const QVariant& entry : stored) {
		const QVariantMap map = entry.toMap();
		const QString title = map.value("title").toString().trimmed();
		if (title.isEmpty()) {
			continue;
		}

		const QString group = map.value("group", "Computed").toString().trimmed().isEmpty()
			? "Computed"
			: map.value("group").toString().trimmed();

		// Editor (user-stored) columns carry a "kind" of editortext/editornumber and a
		// per-row "values" map; everything else is a read-only formula column.
		const QString kind_str = map.value("kind").toString().trimmed().toLower();
		if (kind_str == "editortext" || kind_str == "editornumber") {
			QString key = normalize_column_key(map.value("key").toString());
			if (key.isEmpty()) {
				key = stable_editor_key(title);
			}
			const auto kind = (kind_str == "editornumber")
				? SpreadsheetComputedColumn::Kind::EditorNumber
				: SpreadsheetComputedColumn::Kind::EditorText;
			const bool in_map = map.value("storage").toString().trimmed().toLower() == "inmap";
			// In-map columns live in the per-map file, not QSettings; skip any that leaked here.
			if (in_map) {
				continue;
			}

			SpreadsheetComputedColumn column;
			column.key = key;
			column.title = title;
			column.group = group;
			column.kind = kind;
			column.builtin = false;
			column.storage = in_map ? SpreadsheetComputedColumn::Storage::InMap
			                        : SpreadsheetComputedColumn::Storage::Local;

			const QVariantMap values = map.value("values").toMap();
			for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
				column.values[it.key().toStdString()] = it.value().toString();
			}
			computed_columns_.push_back(std::move(column));
			continue;
		}

		const QString formula = map.value("formula").toString().trimmed();
		if (formula.isEmpty()) {
			continue;
		}
		QString error;
		if (!validateFormula(formula, &error)) {
			continue;
		}

		QString key = normalize_column_key(map.value("key").toString());
		if (key.isEmpty()) {
			key = stable_formula_key(title);
		}

		computed_columns_.push_back({
			key,
			title,
			group,
			formula,
			SpreadsheetComputedColumn::Kind::FormulaNumber,
			false,
		});
	}
}

void SpreadsheetProxy::saveCustomComputedColumns() const {
	if (category_name.isEmpty()) {
		return;
	}

	QVariantList stored;
	for (const auto& column : computed_columns_) {
		if (column.builtin) continue;
		// In-map editor columns are persisted to the per-map file, not QSettings.
		if (column.isEditor() && column.storage == SpreadsheetComputedColumn::Storage::InMap) {
			continue;
		}
		QVariantMap entry;
		entry.insert("key", column.key);
		entry.insert("title", column.title);
		entry.insert("group", column.group);
		if (column.isEditor()) {
			entry.insert("kind", column.kind == SpreadsheetComputedColumn::Kind::EditorNumber
			                         ? "editornumber" : "editortext");
			entry.insert("storage", "local");
			QVariantMap values;
			for (const auto& [row_id, value] : column.values) {
				values.insert(QString::fromStdString(row_id), value);
			}
			entry.insert("values", values);
		} else {
			entry.insert("formula", column.formula);
		}
		stored.push_back(entry);
	}

	QSettings settings;
	settings.setValue("Spreadsheet/formulas/" + category_name, stored);

	// Keep the in-map file in sync in the same pass so a single persist call covers both
	// storage backends.
	saveInMapColumns();
}

void SpreadsheetProxy::loadInMapColumns() {
	if (category_name.isEmpty() || hierarchy.map_directory.empty()) {
		return;
	}
	auto res = hierarchy.map_file_read(kInMapFieldsFile);
	if (!res) {
		return;
	}
	const auto& buffer = res->buffer;
	const QByteArray bytes(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(buffer.size()));
	QJsonParseError parse_error;
	const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}

	const QJsonArray columns = doc.object().value(category_name).toArray();
	for (const QJsonValue& value : columns) {
		const QJsonObject obj = value.toObject();
		const QString title = obj.value("title").toString().trimmed();
		if (title.isEmpty()) {
			continue;
		}
		const QString kind_str = obj.value("kind").toString().trimmed().toLower();
		if (kind_str != "editortext" && kind_str != "editornumber") {
			continue;  // only editor columns are stored in the map file
		}

		QString key = normalize_column_key(obj.value("key").toString());
		if (key.isEmpty()) {
			key = stable_editor_key(title);
		}

		SpreadsheetComputedColumn column;
		column.key = key;
		column.title = title;
		column.group = obj.value("group").toString().trimmed().isEmpty()
			? "Editor" : obj.value("group").toString().trimmed();
		column.kind = (kind_str == "editornumber")
			? SpreadsheetComputedColumn::Kind::EditorNumber
			: SpreadsheetComputedColumn::Kind::EditorText;
		column.builtin = false;
		column.storage = SpreadsheetComputedColumn::Storage::InMap;

		const QJsonObject values = obj.value("values").toObject();
		for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
			column.values[it.key().toStdString()] = it.value().toString();
		}
		computed_columns_.push_back(std::move(column));
	}
}

void SpreadsheetProxy::saveInMapColumns() const {
	if (category_name.isEmpty() || hierarchy.map_directory.empty()) {
		return;
	}

	// Re-read the existing file so other categories' sections are preserved.
	QJsonObject root;
	if (auto res = hierarchy.map_file_read(kInMapFieldsFile)) {
		const auto& buffer = res->buffer;
		const QByteArray bytes(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(buffer.size()));
		const QJsonDocument doc = QJsonDocument::fromJson(bytes);
		if (doc.isObject()) {
			root = doc.object();
		}
	}

	QJsonArray columns;
	for (const auto& column : computed_columns_) {
		if (column.builtin || !column.isEditor()
			|| column.storage != SpreadsheetComputedColumn::Storage::InMap) {
			continue;
		}
		QJsonObject obj;
		obj.insert("key", column.key);
		obj.insert("title", column.title);
		obj.insert("group", column.group);
		obj.insert("kind", column.kind == SpreadsheetComputedColumn::Kind::EditorNumber
		                       ? "editornumber" : "editortext");
		QJsonObject values;
		for (const auto& [row_id, value] : column.values) {
			values.insert(QString::fromStdString(row_id), value);
		}
		obj.insert("values", values);
		columns.append(obj);
	}

	if (columns.isEmpty()) {
		root.remove(category_name);
	} else {
		root.insert(category_name, columns);
	}

	if (root.isEmpty()) {
		if (hierarchy.map_file_exists(kInMapFieldsFile)) {
			hierarchy.map_file_remove(kInMapFieldsFile);
		}
		return;
	}

	const QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Indented);
	hierarchy.map_file_write(kInMapFieldsFile,
		std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(out.constData()),
		                              static_cast<size_t>(out.size())));
}

void SpreadsheetProxy::persistEditorColumn(const SpreadsheetComputedColumn& column) const {
	// A full rewrite of both backends is the simplest correct persistence; the QSettings
	// pass and the in-map file pass each only touch the columns they own.
	Q_UNUSED(column);
	saveCustomComputedColumns();
}

std::string SpreadsheetProxy::rowIdForSourceRow(int source_row) const {
	if (!slk || source_row < 0 || static_cast<size_t>(source_row) >= slk->index_to_row.size()) {
		return {};
	}
	return slk->index_to_row.at(static_cast<size_t>(source_row));
}

double SpreadsheetProxy::numericFieldValue(const QString& field_name, int source_row) const {
	if (!slk) return 0.0;

	const QString normalized = normalize_column_key(field_name);
	if (normalized.isEmpty()) return 0.0;

	const std::string row_id = rowIdForSourceRow(source_row);
	if (row_id.empty()) return 0.0;

	const std::string key = normalized.toStdString();
	const std::string_view raw = slk->data<std::string_view>(key, row_id);
	if (raw.empty()) return 0.0;

	QString text = QString::fromLatin1(raw.data(), static_cast<int>(raw.size())).trimmed();
	text.replace(',', '.');
	bool ok = false;
	const double value = text.toDouble(&ok);
	return ok ? value : 0.0;
}

bool SpreadsheetProxy::evaluateFormula(const QString& formula, int source_row, double& value,
                                       QString* error_message) const {
	struct FormulaParser {
		const SpreadsheetProxy& proxy;
		const int source_row;
		const QString formula;
		QString* error_message = nullptr;
		int pos = 0;
		bool ok = true;

		void fail(const QString& message) {
			if (!ok) return;
			ok = false;
			if (error_message) {
				*error_message = message;
			}
		}

		void skipSpaces() {
			while (pos < formula.size() && formula.at(pos).isSpace()) {
				++pos;
			}
		}

		bool consume(QChar ch) {
			skipSpaces();
			if (pos < formula.size() && formula.at(pos) == ch) {
				++pos;
				return true;
			}
			return false;
		}

		QString parseIdentifier() {
			skipSpaces();
			const int start = pos;
			while (pos < formula.size()) {
				const QChar ch = formula.at(pos);
				if (!(ch.isLetterOrNumber() || ch == '_' || ch == ':')) {
					break;
				}
				++pos;
			}
			return formula.mid(start, pos - start);
		}

		QString parseFieldToken() {
			skipSpaces();
			if (pos >= formula.size()) {
				fail("Expected field name.");
				return {};
			}
			const QChar current = formula.at(pos);
			if (current == '\'' || current == '"') {
				const QChar quote = current;
				++pos;
				const int start = pos;
				while (pos < formula.size() && formula.at(pos) != quote) {
					++pos;
				}
				if (pos >= formula.size()) {
					fail("Unterminated field name literal.");
					return {};
				}
				const QString token = formula.mid(start, pos - start);
				++pos;
				return normalize_column_key(token);
			}
			const QString identifier = normalize_column_key(parseIdentifier());
			if (identifier.isEmpty()) {
				fail("Expected field name.");
			}
			return identifier;
		}

		double parseExpression() {
			double lhs = parseTerm();
			while (ok) {
				skipSpaces();
				if (consume('+')) {
					lhs += parseTerm();
				} else if (consume('-')) {
					lhs -= parseTerm();
				} else {
					break;
				}
			}
			return lhs;
		}

		double parseTerm() {
			double lhs = parseFactor();
			while (ok) {
				skipSpaces();
				if (consume('*')) {
					lhs *= parseFactor();
				} else if (consume('/')) {
					const double rhs = parseFactor();
					if (std::abs(rhs) < 1e-12) {
						lhs = 0.0;
					} else {
						lhs /= rhs;
					}
				} else {
					break;
				}
			}
			return lhs;
		}

		std::vector<double> parseNumericArgumentList() {
			std::vector<double> args;
			if (consume(')')) {
				return args;
			}
			while (ok) {
				args.push_back(parseExpression());
				skipSpaces();
				if (consume(')')) {
					break;
				}
				if (!consume(',')) {
					fail("Expected ',' or ')' in function call.");
					break;
				}
			}
			return args;
		}

		double countListValues() {
			std::set<QString> unique_values;
			if (consume(')')) {
				return 0.0;
			}

			const std::string row_id = proxy.rowIdForSourceRow(source_row);
			while (ok) {
				const QString field_name = parseFieldToken();
				if (field_name.isEmpty()) break;

				if (!proxy.slk || !proxy.slk->column_headers.contains(field_name.toStdString())) {
					fail(QString("Unknown field '%1'.").arg(field_name));
					break;
				}

				const std::string_view raw = proxy.slk->data<std::string_view>(field_name.toStdString(), row_id);
				const QString text = QString::fromLatin1(raw.data(), static_cast<int>(raw.size()));
				for (const QString& entry : text.split(',', Qt::SkipEmptyParts)) {
					const QString normalized = entry.trimmed().toLower();
					if (!normalized.isEmpty()) {
						unique_values.insert(normalized);
					}
				}

				skipSpaces();
				if (consume(')')) {
					break;
				}
				if (!consume(',')) {
					fail("Expected ',' or ')' in count().");
					break;
				}
			}
			return static_cast<double>(unique_values.size());
		}

		double applyFunction(const QString& identifier) {
			const QString name = normalize_column_key(identifier);
			if (name == "count" || name == "countlist") {
				return countListValues();
			}

			const std::vector<double> args = parseNumericArgumentList();
			if (!ok) return 0.0;

			if (name == "min") {
				return args.empty() ? 0.0 : *std::min_element(args.begin(), args.end());
			}
			if (name == "max") {
				return args.empty() ? 0.0 : *std::max_element(args.begin(), args.end());
			}
			if (name == "avg") {
				if (args.empty()) return 0.0;
				const double sum = std::accumulate(args.begin(), args.end(), 0.0);
				return sum / static_cast<double>(args.size());
			}
			if (name == "sum") {
				return std::accumulate(args.begin(), args.end(), 0.0);
			}

			fail(QString("Unknown function '%1'.").arg(identifier));
			return 0.0;
		}

		double parseFactor() {
			skipSpaces();
			if (pos >= formula.size()) {
				fail("Unexpected end of formula.");
				return 0.0;
			}

			if (consume('+')) return parseFactor();
			if (consume('-')) return -parseFactor();
			if (consume('(')) {
				const double nested = parseExpression();
				if (!consume(')')) {
					fail("Expected ')'.");
				}
				return nested;
			}

			const QChar current = formula.at(pos);
			if (current.isDigit() || current == '.') {
				const int start = pos;
				while (pos < formula.size()) {
					const QChar ch = formula.at(pos);
					if (!(ch.isDigit() || ch == '.' || ch == ',')) {
						break;
					}
					++pos;
				}
				QString number = formula.mid(start, pos - start);
				number.replace(',', '.');
				bool number_ok = false;
				const double parsed = number.toDouble(&number_ok);
				if (!number_ok) {
					fail(QString("Invalid number '%1'.").arg(number));
					return 0.0;
				}
				return parsed;
			}

			const QString identifier = parseIdentifier();
			if (identifier.isEmpty()) {
				fail(QString("Unexpected token '%1'.").arg(formula.mid(pos, 1)));
				return 0.0;
			}

			skipSpaces();
			if (consume('(')) {
				return applyFunction(identifier);
			}

			const QString field_name = normalize_column_key(identifier);
			if (!proxy.slk || !proxy.slk->column_headers.contains(field_name.toStdString())) {
				fail(QString("Unknown field '%1'.").arg(identifier));
				return 0.0;
			}
			return proxy.numericFieldValue(field_name, source_row);
		}
	};

	FormulaParser parser{*this, source_row, formula, error_message};
	value = parser.parseExpression();
	parser.skipSpaces();
	if (parser.ok && parser.pos != formula.size()) {
		parser.fail(QString("Unexpected trailing token '%1'.").arg(formula.mid(parser.pos).trimmed()));
	}
	return parser.ok;
}

bool SpreadsheetProxy::validateFormula(const QString& formula, QString* error_message) const {
	double value = 0.0;
	return evaluateFormula(formula, 0, value, error_message);
}

QString SpreadsheetProxy::computedTextValue(const SpreadsheetComputedColumn& column, int source_row) const {
	if (column.key != "__builtin_abilities" || !sourceModel()) {
		return {};
	}

	const std::array<QString, 4> fields = {
		"abillist",
		"heroabillist",
		"abilskinlist",
		"heroabilskinlist",
	};

	QSet<QString> seen;
	QStringList values;
	for (const QString& field : fields) {
		const int col = findColumnByKey(field);
		if (col < 0 || col >= sourceColumnCount()) continue;
		const QString display = sourceModel()->data(sourceModel()->index(source_row, col), Qt::DisplayRole).toString();
		for (const QString& line : display.split('\n', Qt::SkipEmptyParts)) {
			const QString trimmed = line.trimmed();
			if (trimmed.isEmpty() || seen.contains(trimmed)) continue;
			seen.insert(trimmed);
			values.push_back(trimmed);
		}
	}
	return values.join('\n');
}

QVariant SpreadsheetProxy::computedData(int source_row, const SpreadsheetComputedColumn& column, int role) const {
	if (column.isEditor()) {
		if (role == Qt::DisplayRole || role == Qt::EditRole) {
			const std::string row_id = rowIdForSourceRow(source_row);
			const auto it = column.values.find(row_id);
			return it != column.values.end() ? it->second : QString{};
		}
		if (role == Qt::TextAlignmentRole && column.kind == SpreadsheetComputedColumn::Kind::EditorNumber) {
			return QVariant::fromValue(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
		}
		if (role == Qt::ToolTipRole) {
			const bool in_map = column.storage == SpreadsheetComputedColumn::Storage::InMap;
			return QString("%1\n%2").arg(column.title,
				in_map ? QObject::tr("Editor field (stored in map, stripped on game export)")
				       : QObject::tr("Editor field (stored locally, never exported)"));
		}
		return {};
	}

	if (role == Qt::DisplayRole || role == Qt::EditRole) {
		if (column.kind == SpreadsheetComputedColumn::Kind::CombinedText) {
			return computedTextValue(column, source_row);
		}

		double value = 0.0;
		QString error;
		if (!evaluateFormula(column.formula, source_row, value, &error)) {
			return role == Qt::DisplayRole ? QString("ERR") : QVariant{};
		}
		return format_numeric_value(value);
	}
	if (role == Qt::TextAlignmentRole && column.kind == SpreadsheetComputedColumn::Kind::FormulaNumber) {
		return QVariant::fromValue(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
	}
	if (role == Qt::ToolTipRole) {
		if (column.kind == SpreadsheetComputedColumn::Kind::FormulaNumber) {
			return QString("%1\n%2").arg(column.title, column.formula);
		}
		return column.title;
	}
	return {};
}

int SpreadsheetProxy::addCustomFormulaColumn(const QString& title, const QString& formula,
                                             const QString& group, QString* error_message) {
	const QString trimmed_title = title.trimmed();
	const QString trimmed_formula = formula.trimmed();
	if (trimmed_title.isEmpty()) {
		if (error_message) *error_message = "Title cannot be empty.";
		return -1;
	}
	if (trimmed_formula.isEmpty()) {
		if (error_message) *error_message = "Formula cannot be empty.";
		return -1;
	}
	if (!validateFormula(trimmed_formula, error_message)) {
		return -1;
	}

	const int insert_at = columnCount();
	beginInsertColumns(QModelIndex(), insert_at, insert_at);
	computed_columns_.push_back({
		stable_formula_key(trimmed_title),
		trimmed_title,
		group.trimmed().isEmpty() ? "Computed" : group.trimmed(),
		trimmed_formula,
		SpreadsheetComputedColumn::Kind::FormulaNumber,
		false,
	});
	endInsertColumns();
	saveCustomComputedColumns();
	return insert_at;
}

int SpreadsheetProxy::addEditorColumn(const QString& title, const QString& group,
                                      SpreadsheetComputedColumn::Kind kind,
                                      SpreadsheetComputedColumn::Storage storage,
                                      QString* error_message) {
	const QString trimmed_title = title.trimmed();
	if (trimmed_title.isEmpty()) {
		if (error_message) *error_message = "Title cannot be empty.";
		return -1;
	}
	if (kind != SpreadsheetComputedColumn::Kind::EditorText
		&& kind != SpreadsheetComputedColumn::Kind::EditorNumber) {
		if (error_message) *error_message = "Not an editor column kind.";
		return -1;
	}

	const int insert_at = columnCount();
	beginInsertColumns(QModelIndex(), insert_at, insert_at);
	SpreadsheetComputedColumn column;
	column.key = stable_editor_key(trimmed_title);
	column.title = trimmed_title;
	column.group = group.trimmed().isEmpty() ? "Editor" : group.trimmed();
	column.kind = kind;
	column.builtin = false;
	column.storage = storage;
	computed_columns_.push_back(std::move(column));
	endInsertColumns();
	saveCustomComputedColumns();
	return insert_at;
}

bool SpreadsheetProxy::setData(const QModelIndex& index, const QVariant& value, int role) {
	if (index.isValid() && (role == Qt::EditRole || role == Qt::DisplayRole)
		&& isEditorColumn(index.column())) {
		auto& column = computed_columns_.at(static_cast<size_t>(index.column() - sourceColumnCount()));
		const int src_row = visible_rows_.at(static_cast<size_t>(index.row()));
		const std::string row_id = rowIdForSourceRow(src_row);
		if (row_id.empty()) return false;

		QString text = value.toString();
		if (column.kind == SpreadsheetComputedColumn::Kind::EditorNumber && !text.trimmed().isEmpty()) {
			QString normalized = text.trimmed();
			normalized.replace(',', '.');
			bool ok = false;
			const double number = normalized.toDouble(&ok);
			if (!ok) return false;  // reject non-numeric input for number columns
			text = format_numeric_value(number);
		}

		if (text.isEmpty()) {
			column.values.erase(row_id);
		} else {
			column.values[row_id] = text;
		}
		persistEditorColumn(column);
		emit dataChanged(index, index, { Qt::DisplayRole, Qt::EditRole });
		return true;
	}
	return QIdentityProxyModel::setData(index, value, role);
}

bool SpreadsheetProxy::removeCustomFormulaColumn(const QString& key) {
	for (int i = 0; i < static_cast<int>(computed_columns_.size()); ++i) {
		const auto& column = computed_columns_.at(static_cast<size_t>(i));
		if (column.builtin || column.key != normalize_column_key(key)) {
			continue;
		}

		const int logical = sourceColumnCount() + i;
		beginRemoveColumns(QModelIndex(), logical, logical);
		computed_columns_.erase(computed_columns_.begin() + i);
		endRemoveColumns();
		saveCustomComputedColumns();
		return true;
	}
	return false;
}

void SpreadsheetProxy::sort(int column, Qt::SortOrder order) {
	if (sort_column == column && sort_order == order) return;
	sort_column = column;
	sort_order = order;
	reapplyFilters();
}

QModelIndex SpreadsheetProxy::index(int row, int column, const QModelIndex& parent) const {
	if (parent.isValid() || row < 0 || column < 0 || row >= static_cast<int>(visible_rows_.size()))
		return {};
	return createIndex(row, column);
}

int SpreadsheetProxy::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) return 0;
	return static_cast<int>(visible_rows_.size());
}

int SpreadsheetProxy::columnCount(const QModelIndex& parent) const {
	Q_UNUSED(parent);
	return sourceColumnCount() + static_cast<int>(computed_columns_.size());
}

QModelIndex SpreadsheetProxy::mapToSource(QModelIndex proxyIndex) const {
	if (!proxyIndex.isValid()) return {};
	if (isComputedColumn(proxyIndex.column())) return {};
	const int src_row = visible_rows_.at(static_cast<size_t>(proxyIndex.row()));
	return sourceModel()->index(src_row, proxyIndex.column());
}

QVariant SpreadsheetProxy::displayData(int source_row, int source_column) const {
	if (isComputedColumn(source_column)) {
		return computedData(source_row,
		                    computed_columns_.at(static_cast<size_t>(source_column - sourceColumnCount())),
		                    Qt::DisplayRole);
	}
	const QModelIndex src = sourceModel()->index(source_row, source_column);
	return sourceModel()->data(src, Qt::DisplayRole);
}

QVariant SpreadsheetProxy::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || !sourceModel()) return {};

	if (isComputedColumn(index.column())) {
		const int src_row = visible_rows_.at(static_cast<size_t>(index.row()));
		return computedData(src_row,
		                    computed_columns_.at(static_cast<size_t>(index.column() - sourceColumnCount())),
		                    role);
	}

	if (role == Qt::DecorationRole) {
		// Show icon in the name column (decoration comes from the icon field)
		if (icon_column >= 0 && name_column >= 0 && index.column() == name_column) {
			const int src_row = visible_rows_.at(static_cast<size_t>(index.row()));
			return sourceModel()->data(sourceModel()->index(src_row, icon_column), Qt::DecorationRole);
		}
		// For all other columns, let source model decide (icon column returns its own icon)
		if (index.column() != icon_column) return {};
	}

	try {
		const QModelIndex src = mapToSource(index);
		return sourceModel()->data(src, role);
	} catch (...) {
		return {};
	}
}

Qt::ItemFlags SpreadsheetProxy::flags(const QModelIndex& index) const {
	if (!index.isValid() || !sourceModel()) return Qt::NoItemFlags;
	if (isComputedColumn(index.column())) {
		Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
		if (isEditorColumn(index.column())) {
			f |= Qt::ItemIsEditable;  // editor columns hold user-entered values
		}
		return f;
	}
	try {
		const QModelIndex src = mapToSource(index);
		return sourceModel()->flags(src);
	} catch (...) {
		return Qt::NoItemFlags;
	}
}

QVariant SpreadsheetProxy::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role == Qt::DisplayRole) {
		if (orientation == Qt::Horizontal) {
			return fieldDisplayName(section);
		} else {
			if (section >= 0 && section < static_cast<int>(visible_rows_.size())) {
				const std::string& id = slk->index_to_row.at(static_cast<size_t>(visible_rows_.at(static_cast<size_t>(section))));
				return QString::fromStdString(id);
			}
		}
	}
	return QIdentityProxyModel::headerData(section, orientation, role);
}

QString SpreadsheetProxy::fieldDisplayName(int source_column) const {
	if (isComputedColumn(source_column)) {
		return computed_columns_.at(static_cast<size_t>(source_column - sourceColumnCount())).title;
	}
	if (!slk) return {};
	if (source_column < 0 || static_cast<size_t>(source_column) >= slk->index_to_column.size()) {
		return {};
	}

	const std::string& field = slk->index_to_column.at(source_column);

	if (slk->rows() > 0) {
		try {
			const std::string& rep_id = slk->index_to_row.at(0);
			if (const auto meta_id = slk->field_to_meta_id(*meta_slk, field, rep_id)) {
				const std::string_view dn = meta_slk->data<std::string_view>("displayname", *meta_id);
				if (!dn.empty()) {
					QString s = decode_slk_text(dn);
					s.replace('&', "");
					return s;
				}
			}
		} catch (...) {}
	}

	return QString::fromStdString(field);
}

// ============================================================================
// WordWrapHeader — word wrap + font shrink for column headers
// ============================================================================

void WordWrapHeader::paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const {
	if (!rect.isValid()) return;
	painter->save();

	// Custom columns get a tinted header + italic accent text so they stand out from real
	// map fields: violet = read-only formula, teal = editable editor (user-stored) field.
	const auto* proxy = qobject_cast<const SpreadsheetProxy*>(model());
	const bool computed = proxy && proxy->isComputedColumn(logicalIndex);
	const bool editor = proxy && proxy->isEditorColumn(logicalIndex);
	const QColor accent_fill = editor ? QColor(60, 170, 160, 70) : QColor(140, 100, 220, 60);
	const QColor accent_text = editor ? QColor(150, 225, 210) : QColor(196, 170, 255);

	QStyleOptionHeader opt;
	initStyleOption(&opt);
	opt.rect = rect;
	opt.section = logicalIndex;
	opt.text = ""; // suppress default text; we draw manually

	// Draw background + sort arrow (CE_HeaderSection + CE_HeaderLabel without text)
	style()->drawControl(QStyle::CE_HeaderSection, &opt, painter, this);

	if (computed) {
		painter->fillRect(rect, accent_fill);
	}

	// Draw sort indicator via CE_HeaderLabel with empty text
	if (opt.sortIndicator != QStyleOptionHeader::None) {
		style()->drawControl(QStyle::CE_HeaderLabel, &opt, painter, this);
	}

	// Compute text rect (leave room for sort arrow on the right)
	QRect textRect = rect.adjusted(4, 2, -4, -2);
	if (opt.sortIndicator != QStyleOptionHeader::None) {
		textRect.adjust(0, 0, -18, 0);
	}

	const QString text = model() ? model()->headerData(logicalIndex, orientation(), Qt::DisplayRole).toString() : QString{};
	if (text.isEmpty()) { painter->restore(); return; }

	// Try word wrap at current font; shrink font until it fits
	QFont f = font();
	if (computed) f.setItalic(true);
	for (;;) {
		QFontMetrics fm(f);
		QRect br = fm.boundingRect(textRect, Qt::AlignCenter | Qt::TextWordWrap, text);
		if (br.height() <= textRect.height() || f.pointSize() <= 6) break;
		f.setPointSize(f.pointSize() - 1);
	}
	painter->setFont(f);
	painter->setPen(computed ? accent_text : palette().buttonText().color());
	painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, text);

	painter->restore();
}

QSize WordWrapHeader::sectionSizeFromContents(int logicalIndex) const {
	QSize s = QHeaderView::sectionSizeFromContents(logicalIndex);
	s.setHeight(std::max(s.height(), 40));
	return s;
}

// ============================================================================
// SpreadsheetDelegate
// ============================================================================

void SpreadsheetDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
	auto opts = option;
	painter->save();

	QColor text_color;
	QString field;
	bool computed = false;
	bool editor = false;

	if (const auto* proxy = qobject_cast<const SpreadsheetProxy*>(index.model())) {
		computed = proxy->isComputedColumn(index.column());
		editor = proxy->isEditorColumn(index.column());
		QModelIndex src = proxy->mapToSource(index);
		if (src.isValid() && proxy->slk &&
			static_cast<size_t>(src.column()) < proxy->slk->index_to_column.size()) {
			field = QString::fromStdString(proxy->slk->index_to_column.at(src.column()));
		}
	}

	static const std::map<std::string, QColor> text_colours = {
		// Economy
		{"goldcost",   QColor(255, 200, 60)},
		{"lumbercost", QColor(120, 210, 100)},
		{"goldbase",   QColor(255, 200, 60)},
		{"lumberbase", QColor(120, 210, 100)},
		{"fmade",      QColor(255, 200, 60)},
		{"fused",      QColor(255, 200, 60)},
		// HP / mana
		{"hp",         QColor(80,  210, 140)},
		{"hpmax",      QColor(80,  210, 140)},
		{"starthp",    QColor(80,  210, 140)},
		{"manan",      QColor(100, 160, 255)},
		{"startmana",  QColor(100, 160, 255)},
		{"regenhp",    QColor(60,  190, 120)},
		{"regenmana",  QColor(80,  140, 230)},
		// Level / stats
		{"level",      QColor(230, 180, 80)},
		{"primary",    QColor(200, 150, 255)},
		// Combat
		{"dmgplus1",   QColor(230, 100, 100)},
		{"def",        QColor(100, 180, 230)},
	};
	auto it = text_colours.find(field.toStdString());
	if (it != text_colours.end()) {
		text_color = it->second;
		bool selected = opts.state & QStyle::State_Selected;
		if (selected) {
			text_color = text_color.lighter(140);
		}
		opts.palette.setColor(QPalette::Text, text_color);
	}

	if (opts.state & QStyle::State_Selected) {
		painter->fillRect(opts.rect, opts.palette.highlight());
	} else if (opts.features & QStyleOptionViewItem::Alternate) {
		painter->fillRect(opts.rect, opts.palette.alternateBase());
	}

	// Faint wash + italics on custom cells so they read as "not a real map field" at a
	// glance, matching the tinted header (teal = editor field, violet = formula).
	if (computed) {
		if (!(opts.state & QStyle::State_Selected)) {
			painter->fillRect(opts.rect, editor ? QColor(60, 170, 160, 28) : QColor(140, 100, 220, 28));
		}
		opts.font.setItalic(true);
	}

	opts.decorationAlignment = Qt::AlignLeft | Qt::AlignVCenter;
	opts.decorationPosition = QStyleOptionViewItem::Left;
	// decorationSize is left at option.decorationSize (= view->iconSize()), so icons
	// scale together with the Ctrl+wheel zoom.

	QStyledItemDelegate::paint(painter, opts, index);
	painter->restore();
}

QSize SpreadsheetDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
	QSize s = QStyledItemDelegate::sizeHint(option, index);
	s.setHeight(std::max(s.height(), 20));
	return s;
}

QWidget* SpreadsheetDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                           const QModelIndex& index) const {
	QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
	// option.font carries the view's (possibly zoomed) font — apply it so the inline
	// editor doesn't snap back to the default size while editing.
	if (editor) {
		editor->setFont(option.font);
	}
	return editor;
}

// ============================================================================
// SpreadsheetView — Ctrl+wheel zoom (syncs peer), Shift+wheel horizontal scroll
// ============================================================================

void SpreadsheetView::wheelEvent(QWheelEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	int delta = event->angleDelta().y();
#else
	int delta = event->delta();
#endif
	if (event->modifiers() & Qt::ControlModifier) {
		if (delta != 0) {
			const int old_h = verticalHeader()->defaultSectionSize();
			const int h = std::clamp(old_h + (delta > 0 ? 2 : -2), 10, 80);
			if (h == old_h) { event->accept(); return; }
			const double ratio = static_cast<double>(h) / old_h;

			const int icon_sz = std::clamp(h - 6, 8, 64);
			const int base_pt = QApplication::font().pointSize();
			const int font_pt = std::clamp(base_pt + (h - 22) / 4, 6, base_pt + 10);

			// Apply row height, icon size, font and column-width scaling to a view so the
			// whole grid grows/shrinks together (Excel-like zoom, not just taller rows).
			auto apply = [&](SpreadsheetView* v) {
				if (!v) return;
				v->verticalHeader()->setDefaultSectionSize(h);
				v->setIconSize({icon_sz, icon_sz});
				QFont f = v->font();
				f.setPointSize(font_pt);
				v->setFont(f);
				v->horizontalHeader()->setFont(f);
				v->verticalHeader()->setFont(f);

				QHeaderView* hh = v->horizontalHeader();
				const int n = v->model() ? v->model()->columnCount() : 0;
				for (int c = 0; c < n; ++c) {
					if (v->isColumnHidden(c)) continue;
					const int w = v->columnWidth(c);
					const int nw = std::clamp(static_cast<int>(std::lround(w * ratio)),
					                          hh->minimumSectionSize(), 1000);
					if (nw != w) v->setColumnWidth(c, nw);
				}
			};
			apply(this);
			apply(peer);
		}
		event->accept();
		return;
	}
	if (event->modifiers() & Qt::ShiftModifier) {
		// Shift+wheel = horizontal scroll. Scrolling by the raw delta (120px per notch)
		// felt too fast, so scale it down to a calmer step while still keeping a minimal
		// move for high-resolution / trackpad devices that send small deltas.
		int step = delta / 3;
		if (step == 0 && delta != 0) step = (delta > 0) ? 1 : -1;
		horizontalScrollBar()->setValue(horizontalScrollBar()->value() - step);
		event->accept();
		return;
	}
	QTableView::wheelEvent(event);
}

// ============================================================================
// SpreadsheetEditor
// ============================================================================

static QString groupForField(const std::string& field) {
	std::string lower = field;
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

	if (lower.starts_with("__builtin_") || lower.starts_with("__formula_"))
		return "Computed";

	if (lower == "name" || lower == "name1" || lower == "editorname" ||
	    lower == "bufftip" || lower == "buff" || lower == "code" || lower == "comment" ||
	    lower == "version" || lower == "editorsuffix" || lower == "usespecific" ||
	    lower == "missiletype" || lower == "isbldg" || lower == "unitsound" ||
	    lower == "unittype" || lower == "unitclass")
		return "General";

	if (lower == "race" || lower == "class" || lower == "level" || lower == "levels" ||
	    lower == "primary" || lower == "tilesets" || lower == "campaign" ||
	    lower == "special" || lower == "inbeta")
		return "Race / Class";

	if (lower.starts_with("def") || lower.starts_with("dmg") || lower.starts_with("dice") ||
	    lower.starts_with("sides") || lower.starts_with("range") || lower.starts_with("targs") ||
	    lower.starts_with("weap") || lower.starts_with("cool") || lower.starts_with("dur") ||
	    lower.starts_with("herodur") || lower.starts_with("cost") || lower.starts_with("area") ||
	    lower.starts_with("cast") || lower == "spd" || lower == "sight" ||
	    lower.starts_with("armor") || lower.starts_with("backsw") ||
	    lower.starts_with("atk") || lower == "attacks" || lower == "preventdef" ||
	    lower == "nbrandom" || lower == "half" || lower == "naval")
		return "Combat";

	if (lower.starts_with("hp") || lower.starts_with("mana") || lower == "regenhp" ||
	    lower == "regenmana" || lower == "regentype" || lower == "regen" ||
	    lower == "starthp" || lower == "startmana" || lower == "food" ||
	    lower == "bldtm")
		return "HP / Mana / Regen";

	if (lower.starts_with("gold") || lower.starts_with("lumber") ||
	    lower.starts_with("fmad") || lower.starts_with("fuse") ||
	    lower.starts_with("stock") || lower.starts_with("sell") ||
	    lower == "pickrandom" || lower == "perishable" || lower == "prio" ||
	    lower == "used" || lower == "uses" || lower == "usecost")
		return "Cost / Resources";

	if (lower == "file" || lower.starts_with("art") || lower.starts_with("icon") ||
	    lower.starts_with("model") || lower == "portrait" ||
	    lower.starts_with("button") || lower.starts_with("anim") || lower == "extra" ||
	    lower.starts_with("scale") || lower == "shadow" || lower == "deathtype" ||
	    lower == "death" || lower.starts_with("specialart") ||
	    lower.starts_with("missileart") || lower.starts_with("projectile") ||
	    lower.starts_with("targetart") || lower.starts_with("effect") ||
	    lower.starts_with("caster") || lower.starts_with("lightning") ||
	    lower.starts_with("spawn") || lower == "occh" || lower == "tilesetspecific")
		return "Art / Model";

	if (lower.starts_with("sound") || lower == "movesound" ||
	    lower == "constructionsound" || lower == "loopsound" ||
	    lower == "randomsound")
		return "Sound";

	if (lower.starts_with("path") || lower.starts_with("coll") ||
	    lower == "turnrate" || lower == "propwin" || lower == "movetp" ||
	    lower == "moveheight" || lower == "walk" || lower == "targtype" ||
	    lower == "setu" || lower == "setd")
		return "Pathing / Movement";

	if (lower.starts_with("research") || lower.starts_with("upgrade") ||
	    lower.starts_with("require") || lower.starts_with("dep") ||
	    lower.starts_with("techtree") || lower.starts_with("parent") ||
	    lower.starts_with("check") || lower == "tier" || lower == "hero" ||
	    lower.starts_with("inherit") || lower.starts_with("train") ||
	    lower.starts_with("build") || lower.starts_with("revive"))
		return "Techtree";

	if (lower.starts_with("tooltip") || lower.starts_with("tip") ||
	    lower.starts_with("ubertip") || lower.starts_with("description") ||
	    lower.starts_with("hotkey") || lower.starts_with("name") ||
	    lower.starts_with("awaken") || lower.starts_with("revivetip") ||
	    lower.starts_with("editor"))
		return "Text / Tooltips";

	return "Other";
}

static QSet<QString> balancePresetColumns(const QString& tab_name) {
	if (tab_name != "Units") {
		return {};
	}

	return {
		"name",
		"race",
		"unitclass",
		"bldtm",
		"goldcost",
		"lumbercost",
		"hp",
		"manan",
		"regenhp",
		"regenmana",
		"def",
		"deftype",
		"atktype1",
		"weaptp1",
		"cool1",
		"dmgplus1",
		"dice1",
		"sides1",
		"movetp",
		"fmade",
		"fused",
		"level",
		"primary",
		"__builtin_abilities",
		"__builtin_avgattack",
		"__builtin_dps",
	};
}

SpreadsheetEditor::SpreadsheetEditor(QWidget* parent) : QMainWindow(parent) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle("Spreadsheet");
	resize(1280, 800);

	if (!units_table) {
		QMessageBox::warning(nullptr, "Spreadsheet", "Please load a map first.");
		close();
		return;
	}

	tabs = new QTabWidget;
	tabs->setDocumentMode(true);
	setCentralWidget(tabs);

	using S = std::vector<std::string>;

	auto safe_add = [this](const QString& name, TableModel* table,
	                        const std::string& nf, const std::string& iconf,
	                        const std::vector<std::string>& curated, bool rf = false) {
		try {
			addCategoryTab(name, table, nf, iconf, curated, rf);
		} catch (const std::exception& e) {
			QMessageBox::warning(this, "Spreadsheet",
				QString("Tab '%1' failed: %2").arg(name, e.what()));
		} catch (...) {
			QMessageBox::warning(this, "Spreadsheet",
				QString("Tab '%1' failed: unknown error").arg(name));
		}
	};

	safe_add("Units", units_table, "name", "art",
		S{ "name", "race", "hp", "manan", "regenhp", "regenmana", "def", "dmgplus1", "dice1", "sides1",
		   "spd", "goldcost", "lumbercost", "fmade", "fused", "level", "primary", "sight" }, true);
	safe_add("Items", items_table, "name", "art",
		S{ "name", "goldcost", "lumbercost", "level", "class", "prio", "perishable", "pickrandom", "hp" });
	safe_add("Abilities", abilities_table, "name", "Art",
		S{ "name", "code", "race", "levels", "targs1" });
	safe_add("Doodads", doodads_table, "name", "",
		S{ "name", "category", "file" });
	safe_add("Destructibles", destructibles_table, "name", "",
		S{ "name", "category", "file" });
	safe_add("Upgrades", upgrade_table, "name1", "Art",
		S{ "name1", "race", "class", "maxlevel", "goldbase", "lumberbase" });
	safe_add("Buffs", buff_table, "editorname", "Art",
		S{ "editorname", "bufftip", "race" });

	show();
}

void SpreadsheetEditor::addCategoryTab(
	const QString& name,
	TableModel* table,
	const std::string& name_field,
	const std::string& icon_field,
	const std::vector<std::string>& curated,
	bool race_filter
) {
	if (!table || !table->slk) {
		return;
	}

	SpreadsheetProxy* proxy = new SpreadsheetProxy(table, table->slk, table->meta_slk,
	                                               name_field, icon_field, name, this);

	const int columns = proxy->columnCount();

	// Only the name column is frozen by default (icon shows as decoration next to name text)
	auto frozen_cols = QSharedPointer<QSet<int>>::create();
	frozen_cols->insert(proxy->nameColumn());

	// --- frozen view: rawcode in vertical header + frozen columns ---
	SpreadsheetView* frozen_view = new SpreadsheetView;
	frozen_view->setHorizontalHeader(new WordWrapHeader(Qt::Horizontal, frozen_view));
	frozen_view->setModel(proxy);
	frozen_view->setSortingEnabled(false);
	frozen_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	frozen_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	frozen_view->setContextMenuPolicy(Qt::CustomContextMenu);
	frozen_view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	frozen_view->setAlternatingRowColors(true);
	frozen_view->setIconSize({16, 16});
	frozen_view->setItemDelegate(new SpreadsheetDelegate(frozen_view));
	frozen_view->setFrameShape(QFrame::NoFrame);
	frozen_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	frozen_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	frozen_view->verticalHeader()->setDefaultSectionSize(22);
	frozen_view->verticalHeader()->setVisible(true);   // shows rawcode
	frozen_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	frozen_view->horizontalHeader()->setMinimumSectionSize(24);
	frozen_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	// Last frozen column fills the panel width → no empty gap before the splitter handle.
	frozen_view->horizontalHeader()->setSectionsMovable(true);
	frozen_view->horizontalHeader()->setStretchLastSection(false);

	// Show only frozen columns in frozen view
	for (int c = 0; c < columns; c++) {
		frozen_view->setColumnHidden(c, !frozen_cols->contains(c));
	}
	if (proxy->nameColumn() >= 0) {
		frozen_view->setColumnWidth(proxy->nameColumn(), 180);
	}

	// --- main view ---
	SpreadsheetView* view = new SpreadsheetView;
	view->setHorizontalHeader(new WordWrapHeader(Qt::Horizontal, view));
	view->setFrameShape(QFrame::NoFrame);
	view->setModel(proxy);
	view->setSortingEnabled(false);
	view->setAlternatingRowColors(true);
	view->setSelectionBehavior(QAbstractItemView::SelectRows);
	view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	view->setContextMenuPolicy(Qt::CustomContextMenu);
	view->horizontalHeader()->setSectionsMovable(true);
	view->horizontalHeader()->setStretchLastSection(false);
	view->horizontalHeader()->setMinimumSectionSize(40);
	view->verticalHeader()->setDefaultSectionSize(22);
	view->verticalHeader()->setVisible(false);  // rawcode shown in frozen view
	view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	view->setIconSize({16, 16});
	view->setItemDelegate(new SpreadsheetDelegate(view));

	QString settings_key = "Spreadsheet/columns/" + name;
	QSettings settings;
	const QStringList saved_hidden = settings.value(settings_key).toString().split(',',
		Qt::SkipEmptyParts);

	QSet<QString> default_visible;
	for (const std::string& field : curated) {
		default_visible.insert(QString::fromStdString(field));
	}
	bool has_saved = !saved_hidden.isEmpty();

	for (int c = 0; c < columns; c++) {
		const bool is_name = (c == proxy->nameColumn());
		const QString key = proxy->columnKey(c);
		const bool hidden = is_name ? false : (has_saved ? saved_hidden.contains(key) : !default_visible.contains(key));
		const bool frozen = frozen_cols->contains(c);
		frozen_view->setColumnHidden(c, !frozen || hidden);
		view->setColumnHidden(c, frozen || hidden);
	}

	bool any_visible = false;
	for (int c = 0; c < columns; c++) {
		if (!frozen_cols->contains(c) && !view->isColumnHidden(c)) {
			any_visible = true; break;
		}
	}
	if (!any_visible) {
		for (int c = 0; c < columns; c++) {
			if (!frozen_cols->contains(c)) view->setColumnHidden(c, false);
		}
	}

	// Link peer views for zoom sync
	frozen_view->peer = view;
	view->peer = frozen_view;

	auto update_frozen_geometry = [frozen_view, proxy]() {
		int width = frozen_view->frameWidth() * 2 + 2;
		if (frozen_view->verticalHeader()->isVisible()) {
			width += frozen_view->verticalHeader()->width();
		}
		for (int c = 0; c < proxy->columnCount(); ++c) {
			if (!frozen_view->isColumnHidden(c)) {
				width += frozen_view->columnWidth(c);
			}
		}
		frozen_view->setMinimumWidth(width);
		frozen_view->setMaximumWidth(width);
	};
	connect(frozen_view->horizontalHeader(), &QHeaderView::sectionResized,
	        frozen_view, [update_frozen_geometry](int, int, int) { update_frozen_geometry(); });

	auto move_sync_guard = QSharedPointer<bool>::create(false);
	auto sync_header_moves = [move_sync_guard](QHeaderView* source, QHeaderView* target) {
		QObject::connect(source, &QHeaderView::sectionMoved, target,
		                 [move_sync_guard, target](int logical, int, int new_visual_index) {
			if (*move_sync_guard) return;
			const int current_visual = target->visualIndex(logical);
			if (current_visual < 0 || current_visual == new_visual_index) return;
			*move_sync_guard = true;
			target->moveSection(current_visual, new_visual_index);
			*move_sync_guard = false;
		});
	};
	sync_header_moves(view->horizontalHeader(), frozen_view->horizontalHeader());
	sync_header_moves(frozen_view->horizontalHeader(), view->horizontalHeader());

	// Sync vertical scrolling
	connect(view->verticalScrollBar(), &QScrollBar::valueChanged,
	        frozen_view->verticalScrollBar(), &QScrollBar::setValue);
	connect(frozen_view->verticalScrollBar(), &QScrollBar::valueChanged,
	        view->verticalScrollBar(), &QScrollBar::setValue);

	// Sync row selection
	connect(frozen_view->selectionModel(), &QItemSelectionModel::selectionChanged,
	        view, [frozen_view, view]() {
		QModelIndexList sel = frozen_view->selectionModel()->selectedRows();
		if (sel.isEmpty()) return;
		QItemSelection selection;
		for (const QModelIndex& idx : sel) selection.select(idx, idx);
		view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	});
	connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
	        frozen_view, [frozen_view, view]() {
		QModelIndexList sel = view->selectionModel()->selectedRows();
		if (sel.isEmpty()) return;
		QItemSelection selection;
		for (const QModelIndex& idx : sel) selection.select(idx, idx);
		frozen_view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	});

	// Sort via header clicks (LMB on a column header toggles asc/desc)
	// NOTE: when setSortingEnabled(false) the header is NOT clickable by default, so
	// sectionClicked never fires. We must enable clickability explicitly.
	view->horizontalHeader()->setSectionsClickable(true);
	frozen_view->horizontalHeader()->setSectionsClickable(true);
	view->horizontalHeader()->setSortIndicatorShown(true);
	frozen_view->horizontalHeader()->setSortIndicatorShown(true);
	auto sort_fn = [proxy, view, frozen_view](int logicalIndex) {
		Qt::SortOrder order = Qt::AscendingOrder;
		if (proxy->sort_column == logicalIndex) {
			order = (proxy->sort_order == Qt::AscendingOrder)
			            ? Qt::DescendingOrder : Qt::AscendingOrder;
		}
		proxy->sort(logicalIndex, order);
		// Reflect the indicator in whichever header owns the column; clear the other.
		const bool frozen_owns = frozen_view->isColumnHidden(logicalIndex) == false;
		view->horizontalHeader()->setSortIndicator(
			frozen_owns ? -1 : logicalIndex, order);
		frozen_view->horizontalHeader()->setSortIndicator(
			frozen_owns ? logicalIndex : -1, order);
	};
	connect(view->horizontalHeader(), &QHeaderView::sectionClicked, proxy, sort_fn);
	connect(frozen_view->horizontalHeader(), &QHeaderView::sectionClicked, proxy, sort_fn);

	// ---- Right-click on main view column header → Freeze ----
	view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(view->horizontalHeader(), &QHeaderView::customContextMenuRequested,
	        this, [=](const QPoint& pos) {
		int logical = view->horizontalHeader()->logicalIndexAt(pos);
		if (logical < 0) return;
		QMenu menu;
		QAction* freeze_act = menu.addAction("Freeze column");
		if (menu.exec(view->horizontalHeader()->mapToGlobal(pos)) == freeze_act) {
			frozen_cols->insert(logical);
			view->setColumnHidden(logical, true);
			frozen_view->setColumnHidden(logical, false);
			update_frozen_geometry();
		}
	});

	// ---- Right-click on frozen view column header → Unfreeze ----
	frozen_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(frozen_view->horizontalHeader(), &QHeaderView::customContextMenuRequested,
	        this, [=](const QPoint& pos) {
		int logical = frozen_view->horizontalHeader()->logicalIndexAt(pos);
		if (logical < 0) return;
		// Don't allow unfreezing the name column — it must stay frozen (like the rawcode)
		if (logical == proxy->nameColumn()) return;
		QMenu menu;
		QAction* unfreeze_act = menu.addAction("Unfreeze column");
		if (menu.exec(frozen_view->horizontalHeader()->mapToGlobal(pos)) == unfreeze_act) {
			frozen_cols->remove(logical);
			frozen_view->setColumnHidden(logical, true);
			view->setColumnHidden(logical, false);
			update_frozen_geometry();
		}
	});

	// ---- Right-click on cells → Batch edit + column filter ----
	auto batch_menu = [this](SpreadsheetView* v, SpreadsheetProxy* px, TableModel* tbl, const QPoint& pos) {
		QMenu menu;
		QAction* batch = menu.addAction("Batch edit...");
		batch->setEnabled(!v->selectionModel()->selectedRows().isEmpty());
		const QModelIndex at = v->indexAt(pos);
		const int preferred_column = at.isValid() ? at.column() : -1;
		connect(batch, &QAction::triggered, this,
		        [this, v, px, tbl, preferred_column]() {
			openBatchDialog(v, px, tbl, preferred_column);
		});

		// --- column filter actions ---
		if (at.isValid() && static_cast<size_t>(at.column()) < tbl->slk->index_to_column.size()) {
			const std::string field = tbl->slk->index_to_column.at(static_cast<size_t>(at.column()));
			const QString value = at.data(Qt::DisplayRole).toString();
			const QString col_label = px->fieldDisplayName(at.column());

			menu.addSeparator();
			if (!value.isEmpty()) {
				QAction* eq = menu.addAction(QString("Filter %1 = \"%2\"").arg(col_label, value));
				connect(eq, &QAction::triggered, px, [px, field, value]() {
					px->setFieldFilter(QString::fromStdString(field), value);
				});
			}
			QAction* custom = menu.addAction(QString("Filter %1 by…").arg(col_label));
			connect(custom, &QAction::triggered, this, [this, px, field, col_label, value]() {
				bool ok = false;
				const QString text = QInputDialog::getText(this, "Column filter",
					QString("Show rows where %1 contains:").arg(col_label),
					QLineEdit::Normal, value, &ok);
				if (ok) px->setFieldFilter(QString::fromStdString(field), text);
			});
		}
		if (!px->field_filter_name.empty() && !px->field_filter_text.isEmpty()) {
			QAction* clear = menu.addAction("Clear column filter");
			connect(clear, &QAction::triggered, px, [px]() { px->setFieldFilter("", ""); });
		}

		menu.exec(v->viewport()->mapToGlobal(pos));
	};

	connect(view, &QTableView::customContextMenuRequested, this,
	        [=](const QPoint& pos) { batch_menu(view, proxy, table, pos); });
	connect(frozen_view, &QTableView::customContextMenuRequested, this,
	        [=](const QPoint& pos) { batch_menu(frozen_view, proxy, table, pos); });

	// ---- Toolbar ----
	QToolBar* bar = new QToolBar;
	bar->setMovable(false);

	QLineEdit* search = new QLineEdit;
	search->setPlaceholderText("Search " + name);
	search->setClearButtonEnabled(true);
	search->setMaximumWidth(260);
	connect(search, &QLineEdit::textChanged, proxy, &SpreadsheetProxy::setTextFilter);
	bar->addWidget(search);

	if (race_filter && unit_editor_data.section_exists("unitRace")) {
		QComboBox* race_combo = new QComboBox;
		race_combo->addItem("All races", QString());
		for (const auto& [key, value] : unit_editor_data.section("unitRace")) {
			if (key == "NumValues" || key == "Sort" || key.ends_with("_Alt")) continue;
			QString label = QString::fromStdString(value[1]);
			label.replace('&', "");
			race_combo->addItem(label, QString::fromStdString(value[0]));
		}
		connect(race_combo, &QComboBox::currentIndexChanged, proxy,
		        [proxy, race_combo](int) {
			proxy->setRaceFilter(race_combo->currentData().toString());
		});
		bar->addSeparator();
		bar->addWidget(new QLabel(" Race: "));
		bar->addWidget(race_combo);
	}

	if (race_filter) {
		// Two checkboxes: Buildings + Units (replacing the single "Building" toggle)
		QCheckBox* chk_buildings = new QCheckBox("Buildings");
		chk_buildings->setChecked(true);
		QCheckBox* chk_units = new QCheckBox("Units");
		chk_units->setChecked(true);

		auto update_filter = [proxy, chk_buildings, chk_units]() {
			proxy->setUnitTypeFilter(chk_buildings->isChecked(), chk_units->isChecked());
		};
		connect(chk_buildings, &QCheckBox::toggled, this, update_filter);
		connect(chk_units, &QCheckBox::toggled, this, update_filter);

		bar->addSeparator();
		bar->addWidget(chk_buildings);
		bar->addWidget(chk_units);
	}

	// Editor suffix filter
	{
		QLineEdit* suffix_edit = new QLineEdit;
		suffix_edit->setPlaceholderText("editor suffix");
		suffix_edit->setClearButtonEnabled(true);
		suffix_edit->setMaximumWidth(120);
		suffix_edit->setToolTip("Filter by editorSuffix / version field (substring, case-insensitive)");
		connect(suffix_edit, &QLineEdit::textChanged, proxy, &SpreadsheetProxy::setEditorSuffixFilter);
		bar->addSeparator();
		bar->addWidget(new QLabel(" Suffix: "));
		bar->addWidget(suffix_edit);
	}

	QToolButton* custom_only = new QToolButton;
	custom_only->setText("Custom only");
	custom_only->setCheckable(true);
	custom_only->setToolTip("Show only custom (modified) objects");
	connect(custom_only, &QToolButton::toggled, proxy, &SpreadsheetProxy::setCustomOnly);
	bar->addSeparator();
	bar->addWidget(custom_only);

	QWidget* spacer = new QWidget;
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	bar->addWidget(spacer);

	// Full layout reset: zoom, visibility, order, widths, freeze state, sort, filters.
	auto full_reset = [view, frozen_view, proxy, frozen_cols, columns, curated,
	                   settings_key, update_frozen_geometry]() {
		QSettings settings;
		settings.remove(settings_key);

		// 1) zoom → defaults
		const QFont f = QApplication::font();
		for (SpreadsheetView* sv : { view, frozen_view }) {
			sv->verticalHeader()->setDefaultSectionSize(22);
			sv->setIconSize({16, 16});
			sv->setFont(f);
			sv->horizontalHeader()->setFont(f);
			sv->verticalHeader()->setFont(f);
		}

		// 2) unfreeze everything except the name column
		const int total_columns = proxy->columnCount();
		for (int c = 0; c < total_columns; ++c) {
			if (c == proxy->nameColumn()) continue;
			if (frozen_cols->contains(c)) {
				frozen_cols->remove(c);
				frozen_view->setColumnHidden(c, true);
			}
		}

		// 3) visibility → curated defaults
		QSet<QString> visible;
		for (const std::string& field : curated) {
			visible.insert(QString::fromStdString(field));
		}
		for (int c = 0; c < total_columns; ++c) {
			if (c == proxy->nameColumn()) {
				frozen_view->setColumnHidden(c, false);
				view->setColumnHidden(c, true);
				continue;
			}
			frozen_view->setColumnHidden(c, true);
			view->setColumnHidden(c, !visible.contains(proxy->columnKey(c)));
		}

		// 4) column order → logical order
		QHeaderView* hh = view->horizontalHeader();
		for (int logical = 0; logical < total_columns; ++logical) {
			const int vis = hh->visualIndex(logical);
			if (vis != logical) hh->moveSection(vis, logical);
		}

		// 5) widths → fit contents; frozen name back to 180
		view->resizeColumnsToContents();
		frozen_view->resizeColumnsToContents();
		if (proxy->nameColumn() >= 0) {
			frozen_view->setColumnWidth(proxy->nameColumn(), 180);
		}

		// 6) sort + column filter cleared
		proxy->sort(-1, Qt::AscendingOrder);
		proxy->setFieldFilter("", "");
		view->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
		frozen_view->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
		update_frozen_geometry();
	};

	QToolButton* columns_button = new QToolButton;
	columns_button->setText("Columns...");
	connect(columns_button, &QToolButton::clicked, this,
	        [this, view, frozen_view, proxy, table, curated, full_reset]() {
		openColumnDialog(view, frozen_view, proxy, table, curated, full_reset);
	});
	bar->addWidget(columns_button);

	QWidget* table_row = new QWidget;
	QHBoxLayout* row_layout = new QHBoxLayout(table_row);
	row_layout->setContentsMargins(0, 0, 0, 0);
	row_layout->setSpacing(0);
	row_layout->addWidget(frozen_view);
	row_layout->addWidget(view, 1);

	QWidget* container = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(bar);
	layout->addWidget(table_row);

	tabs->addTab(container, name);
	update_frozen_geometry();
}

// ============================================================================
// Column visibility dialog (tree + search + restore defaults)
// ============================================================================

void SpreadsheetEditor::openColumnDialog(SpreadsheetView* view, SpreadsheetView* frozen_view,
                                          SpreadsheetProxy* proxy, TableModel* table,
                                          const std::vector<std::string>& curated,
                                          const std::function<void()>& full_reset) {
	QString tab_name = tabs->tabText(tabs->currentIndex());
	QString settings_key = "Spreadsheet/columns/" + tab_name;
	QSettings settings;

	QDialog dlg(this);
	dlg.setWindowTitle("Column visibility — " + tab_name);
	dlg.resize(460, 580);
	dlg.setStyleSheet(
		"QDialog { background-color: palette(window); }"
		"QTreeWidget { background-color: palette(base); color: palette(text); }"
		"QTreeWidget::item { padding: 2px 4px; }"
		"QTreeWidget::item:hover { background-color: palette(highlight); color: palette(highlighted-text); }"
		"QTreeWidget::branch { background-color: palette(window); }"
		"QLineEdit { padding: 4px 8px; }"
	);

	QVBoxLayout* main_layout = new QVBoxLayout(&dlg);

	QLineEdit* search_bar = new QLineEdit;
	search_bar->setPlaceholderText("Search fields...");
	search_bar->setClearButtonEnabled(true);
	main_layout->addWidget(search_bar);

	QTreeWidget* tree = new QTreeWidget;
	tree->setHeaderHidden(true);
	tree->setRootIsDecorated(true);
	tree->setSelectionMode(QAbstractItemView::NoSelection);
	main_layout->addWidget(tree);

	Q_UNUSED(table);
	Q_UNUSED(curated);

	const int columns = proxy->columnCount();

	// Build groups (exclude name column — managed by frozen view)
	std::map<QString, std::vector<std::pair<int, QString>>> groups;
	for (int c = 0; c < columns; c++) {
		if (c == proxy->nameColumn()) continue;
		const QString key = proxy->columnKey(c);
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) label = key;
		QString gname = proxy->groupName(c);
		if (gname.isEmpty()) {
			gname = groupForField(key.toStdString());
		}
		groups[gname].push_back({c, label});
	}

	std::vector<QString> ordered_groups = {
		"General", "Race / Class", "Combat", "HP / Mana / Regen",
		"Cost / Resources", "Art / Model", "Sound",
		"Pathing / Movement", "Techtree", "Text / Tooltips", "Editor", "Computed", "Other",
	};
	// Append any user-defined groups (e.g. custom formula/editor groups) not in the
	// predefined order so they are still manageable in the dialog.
	for (const auto& [gname, cols] : groups) {
		if (std::find(ordered_groups.begin(), ordered_groups.end(), gname) == ordered_groups.end()) {
			ordered_groups.push_back(gname);
		}
	}

	QMap<QString, QTreeWidgetItem*> group_items;
	std::map<QTreeWidgetItem*, int> item_to_col;

	for (const auto& gname : ordered_groups) {
		auto it = groups.find(gname);
		if (it == groups.end() || it->second.empty()) continue;

		QTreeWidgetItem* group_item = new QTreeWidgetItem(tree, {gname});
		group_item->setFlags(group_item->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
		QFont gf = group_item->font(0);
		gf.setBold(true);
		group_item->setFont(0, gf);
		group_items[gname] = group_item;

		for (const auto& [col, label] : it->second) {
			QTreeWidgetItem* child = new QTreeWidgetItem(group_item, {label});
			child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
			bool is_visible = !view->isColumnHidden(col) || !frozen_view->isColumnHidden(col);
			child->setCheckState(0, is_visible ? Qt::Checked : Qt::Unchecked);
			item_to_col[child] = col;
		}
	}

	tree->expandAll();

	connect(search_bar, &QLineEdit::textChanged, tree, [tree](const QString& text) {
		for (int i = 0; i < tree->topLevelItemCount(); ++i) {
			QTreeWidgetItem* gitem = tree->topLevelItem(i);
			bool any_child_visible = false;
			for (int j = 0; j < gitem->childCount(); ++j) {
				QTreeWidgetItem* child = gitem->child(j);
				bool match = text.isEmpty() ||
					child->text(0).contains(text, Qt::CaseInsensitive);
				child->setHidden(!match);
				if (match) any_child_visible = true;
			}
			gitem->setHidden(!any_child_visible);
		}
	});

	QHBoxLayout* btn_row = new QHBoxLayout;
	QPushButton* defaults_btn = new QPushButton("Restore defaults");
	defaults_btn->setToolTip("Full reset: column visibility, order, widths, freeze state, "
	                         "zoom, sort and filters");
	// Full reset applies live to the views, then closes the dialog (reject so the
	// checkbox state isn't re-applied over the freshly reset layout).
	connect(defaults_btn, &QPushButton::clicked, &dlg, [&dlg, &full_reset]() {
		full_reset();
		dlg.reject();
	});

	QPushButton* show_all_btn = new QPushButton("Show all");
	connect(show_all_btn, &QPushButton::clicked, tree, [tree]() {
		for (int i = 0; i < tree->topLevelItemCount(); ++i) {
			QTreeWidgetItem* gitem = tree->topLevelItem(i);
			for (int j = 0; j < gitem->childCount(); ++j) {
				gitem->child(j)->setCheckState(0, Qt::Checked);
			}
		}
	});

	QPushButton* balance_btn = new QPushButton(QString::fromUtf8(u8"Баланс"));
	balance_btn->setEnabled(!balancePresetColumns(tab_name).isEmpty());
	connect(balance_btn, &QPushButton::clicked, &dlg, [&, preset = balancePresetColumns(tab_name)]() {
		for (const auto& [item, col] : item_to_col) {
			item->setCheckState(0, preset.contains(proxy->columnKey(col)) ? Qt::Checked : Qt::Unchecked);
		}
	});

	QPushButton* formula_btn = new QPushButton("Add formula...");
	connect(formula_btn, &QPushButton::clicked, &dlg, [&, proxy, view, frozen_view]() {
		QDialog formula_dialog(&dlg);
		formula_dialog.setWindowTitle("Add formula column");
		QFormLayout* form = new QFormLayout(&formula_dialog);

		QLineEdit* title_edit = new QLineEdit;
		title_edit->setPlaceholderText("Column title");
		QLineEdit* formula_edit = new QLineEdit;
		formula_edit->setPlaceholderText("dmgplus1 + dice1 * (sides1 + 1) / 2");
		QLineEdit* group_edit = new QLineEdit("Computed");

		form->addRow("Title", title_edit);
		form->addRow("Formula", formula_edit);
		form->addRow("Group", group_edit);

		QDialogButtonBox* formula_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		form->addRow(formula_box);
		connect(formula_box, &QDialogButtonBox::accepted, &formula_dialog, &QDialog::accept);
		connect(formula_box, &QDialogButtonBox::rejected, &formula_dialog, &QDialog::reject);

		if (formula_dialog.exec() != QDialog::Accepted) return;

		QString error;
		const int new_column = proxy->addCustomFormulaColumn(title_edit->text(), formula_edit->text(),
		                                                     group_edit->text(), &error);
		if (new_column < 0) {
			QMessageBox::warning(&dlg, "Formula column", error.isEmpty() ? "Invalid formula." : error);
			return;
		}

		view->setColumnHidden(new_column, false);
		frozen_view->setColumnHidden(new_column, true);
		dlg.accept();
	});

	// Editor-only fields: editable, user-stored columns (notes / group / status / custom
	// numbers) that never become real map object data.
	QPushButton* field_btn = new QPushButton(QString::fromUtf8(u8"Своё поле..."));
	field_btn->setToolTip(QString::fromUtf8(
		u8"Добавить редактируемое поле (текст/число), которое сохраняется, "
		u8"но не попадает в финальные данные карты"));
	connect(field_btn, &QPushButton::clicked, &dlg, [&, proxy, view, frozen_view]() {
		QDialog field_dialog(&dlg);
		field_dialog.setWindowTitle(QString::fromUtf8(u8"Добавить своё поле"));
		QFormLayout* form = new QFormLayout(&field_dialog);

		QLineEdit* title_edit = new QLineEdit;
		title_edit->setPlaceholderText(QString::fromUtf8(u8"Заголовок (напр. Группа, Заметка)"));
		QLineEdit* group_edit = new QLineEdit("Editor");

		QComboBox* type_combo = new QComboBox;
		type_combo->addItem(QString::fromUtf8(u8"Текст"),
			static_cast<int>(SpreadsheetComputedColumn::Kind::EditorText));
		type_combo->addItem(QString::fromUtf8(u8"Число"),
			static_cast<int>(SpreadsheetComputedColumn::Kind::EditorNumber));

		QComboBox* storage_combo = new QComboBox;
		storage_combo->addItem(QString::fromUtf8(u8"Локально (этот ПК)"),
			static_cast<int>(SpreadsheetComputedColumn::Storage::Local));
		storage_combo->addItem(QString::fromUtf8(u8"В карте (ездит с картой)"),
			static_cast<int>(SpreadsheetComputedColumn::Storage::InMap));
		storage_combo->setItemData(0,
			QString::fromUtf8(u8"Значения хранятся в настройках HiveWE на этом ПК"), Qt::ToolTipRole);
		storage_combo->setItemData(1,
			QString::fromUtf8(u8"Тихий файл war3map.hivewe_fields.json в карте; игрой игнорируется"),
			Qt::ToolTipRole);

		form->addRow(QString::fromUtf8(u8"Заголовок"), title_edit);
		form->addRow(QString::fromUtf8(u8"Группа"), group_edit);
		form->addRow(QString::fromUtf8(u8"Тип"), type_combo);
		form->addRow(QString::fromUtf8(u8"Хранение"), storage_combo);

		QDialogButtonBox* field_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		form->addRow(field_box);
		connect(field_box, &QDialogButtonBox::accepted, &field_dialog, &QDialog::accept);
		connect(field_box, &QDialogButtonBox::rejected, &field_dialog, &QDialog::reject);

		if (field_dialog.exec() != QDialog::Accepted) return;

		const auto kind = static_cast<SpreadsheetComputedColumn::Kind>(type_combo->currentData().toInt());
		const auto storage = static_cast<SpreadsheetComputedColumn::Storage>(storage_combo->currentData().toInt());

		QString error;
		const int new_column = proxy->addEditorColumn(title_edit->text(), group_edit->text(),
		                                              kind, storage, &error);
		if (new_column < 0) {
			QMessageBox::warning(&dlg, QString::fromUtf8(u8"Своё поле"),
				error.isEmpty() ? QString::fromUtf8(u8"Не удалось добавить поле.") : error);
			return;
		}

		view->setColumnHidden(new_column, false);
		frozen_view->setColumnHidden(new_column, true);
		dlg.accept();
	});

	btn_row->addWidget(defaults_btn);
	btn_row->addWidget(show_all_btn);
	btn_row->addWidget(balance_btn);
	btn_row->addWidget(formula_btn);
	btn_row->addWidget(field_btn);
	btn_row->addStretch();
	main_layout->addLayout(btn_row);

	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	main_layout->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted) return;

	QStringList hidden_fields;
	for (int i = 0; i < tree->topLevelItemCount(); ++i) {
		QTreeWidgetItem* gitem = tree->topLevelItem(i);
		for (int j = 0; j < gitem->childCount(); ++j) {
			QTreeWidgetItem* child = gitem->child(j);
			int col = item_to_col[child];
			bool hidden = (child->checkState(0) == Qt::Unchecked);
			const bool prefer_frozen = (col == proxy->nameColumn()) || !frozen_view->isColumnHidden(col);
			if (prefer_frozen) {
				frozen_view->setColumnHidden(col, hidden && col != proxy->nameColumn());
				view->setColumnHidden(col, true);
			} else {
				view->setColumnHidden(col, hidden);
			}
			if (hidden) {
				hidden_fields.append(proxy->columnKey(col));
			}
		}
	}

	settings.setValue(settings_key, hidden_fields.join(','));
}

// ============================================================================
// Batch edit dialog
// ============================================================================

void SpreadsheetEditor::openBatchDialog(SpreadsheetView* view, SpreadsheetProxy* proxy, TableModel* table, int preferred_column) {
	slk::SLK* slk = table->slk;
	slk::SLK* meta_slk = table->meta_slk;

	std::vector<int> source_rows;
	for (const QModelIndex& idx : view->selectionModel()->selectedRows()) {
		const QModelIndex src = proxy->mapToSource(idx);
		if (src.isValid()) {
			source_rows.push_back(src.row());
		}
	}
	if (source_rows.empty()) return;

	const std::string& rep_id = slk->index_to_row.at(0);
	auto field_type = [&](const std::string& field) -> std::string_view {
		if (const auto meta_id = slk->field_to_meta_id(*meta_slk, field, rep_id)) {
			return meta_slk->data<std::string_view>("type", *meta_id);
		}
		return {};
	};
	auto is_numeric = [&](std::string_view type) { return type == "int" || type == "real" || type == "unreal"; };

	QDialog dialog(this);
	dialog.setWindowTitle("Batch edit");
	dialog.setModal(true);

	QFormLayout* form = new QFormLayout(&dialog);

	QComboBox* field_combo = new QComboBox;
	for (int c = 0; c < slk->columns(); c++) {
		if (view->isColumnHidden(c)) continue;
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) {
			label = QString::fromStdString(slk->index_to_column.at(static_cast<size_t>(c)));
		}
		field_combo->addItem(label, c);
	}
	if (field_combo->count() == 0) return;
	if (preferred_column >= 0) {
		int i = field_combo->findData(preferred_column);
		if (i >= 0) field_combo->setCurrentIndex(i);
	}

	QComboBox* op_combo = new QComboBox;
	op_combo->addItem("Set", "set");
	op_combo->addItem("Add (+)", "add");
	op_combo->addItem("Multiply (x)", "mul");
	op_combo->addItem("Percent (+%)", "pct");

	QLineEdit* value_edit = new QLineEdit;
	QLabel* count_label = new QLabel(QString("Applies to %1 object(s)").arg(source_rows.size()));

	form->addRow("Field", field_combo);
	form->addRow("Operation", op_combo);
	form->addRow("Value", value_edit);
	form->addRow(count_label);

	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto refresh_ops = [&]() {
		const std::string field = slk->index_to_column.at(static_cast<size_t>(field_combo->currentData().toInt()));
		const bool numeric = is_numeric(field_type(field));
		op_combo->setEnabled(numeric);
		if (!numeric) op_combo->setCurrentIndex(0);
	};
	connect(field_combo, &QComboBox::currentIndexChanged, &dialog, [&](int) { refresh_ops(); });
	refresh_ops();

	if (dialog.exec() != QDialog::Accepted) return;

	const int col = field_combo->currentData().toInt();
	const std::string field = slk->index_to_column.at(static_cast<size_t>(col));
	const QString op = op_combo->currentData().toString();
	const std::string_view type = field_type(field);
	const bool numeric = is_numeric(type);
	const bool is_int = type == "int";

	auto format_number = [is_int](double v) -> std::string {
		if (is_int) return std::to_string(static_cast<long long>(std::llround(v)));
		return std::format("{}", v);
	};

	if (!numeric || op == "set") {
		if (numeric && op == "set") {
			bool ok = false;
			const double v = value_edit->text().toDouble(&ok);
			if (!ok) {
				QMessageBox::warning(this, "Batch edit", "Please enter a valid number.");
				return;
			}
			const QString out = QString::fromStdString(format_number(v));
			for (const int r : source_rows) {
				table->setData(table->index(r, col), out, Qt::EditRole);
			}
		} else {
			const QString out = value_edit->text();
			for (const int r : source_rows) {
				table->setData(table->index(r, col), out, Qt::EditRole);
			}
		}
		return;
	}

	bool ok = false;
	const double operand = value_edit->text().toDouble(&ok);
	if (!ok) {
		QMessageBox::warning(this, "Batch edit", "Please enter a valid number.");
		return;
	}

	for (const int r : source_rows) {
		const std::string& id = slk->index_to_row.at(static_cast<size_t>(r));
		double current = 0.0;
		const std::string cur_str{ slk->data<std::string_view>(field, id) };
		try { current = std::stod(cur_str); } catch (...) { current = 0.0; }

		double result = current;
		if (op == "add") result = current + operand;
		else if (op == "mul") result = current * operand;
		else if (op == "pct") result = current * (1.0 + operand / 100.0);

		table->setData(table->index(r, col), QString::fromStdString(format_number(result)), Qt::EditRole);
	}
}
