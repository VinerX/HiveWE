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
#include <QGroupBox>
#include <QTreeWidget>
#include <QSplitter>
#include <QScrollBar>
#include <QPushButton>
#include <map>
#include <algorithm>

import std;
import Globals;

// ============================================================================
// SpreadsheetProxy
// ============================================================================

SpreadsheetProxy::SpreadsheetProxy(QAbstractItemModel* source_model,
                                   slk::SLK* data_slk, slk::SLK* meta_slk,
                                   std::string name_field,
                                   std::string icon_field,
                                   QObject* parent)
	: QIdentityProxyModel(parent)
	, slk(data_slk)
	, meta_slk(meta_slk)
	, name_field(std::move(name_field))
	, icon_field(std::move(icon_field))
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

	beginResetModel();
	rebuildVisibleRows();
	endResetModel();
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

		if (building_only && slk->column_headers.contains("isbldg")) {
			auto isbldg = slk->data<std::string_view>("isbldg", id);
			if (isbldg != "1") continue;
		}

		if (!editor_suffix.isEmpty()) {
			if (slk->column_headers.contains("editorSuffix")) {
				auto suffix = slk->data<std::string_view>("editorSuffix", id);
				if (!suffix.data() || suffix != editor_suffix.toStdString()) continue;
			} else if (slk->column_headers.contains("version")) {
				auto suffix = slk->data<std::string_view>("version", id);
				if (!suffix.data() || suffix != editor_suffix.toStdString()) continue;
			}
		}

		if (!field_filter_name.empty() && !field_filter_text.isEmpty() && slk->column_headers.contains(field_filter_name)) {
			auto val = slk->data<std::string_view>(field_filter_name, id);
			QString qval = QString::fromUtf8(val.data(), static_cast<int>(val.size()));
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

void SpreadsheetProxy::setBuildingFilter(bool buildings) {
	if (building_only == buildings) return;
	building_only = buildings;
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
	return sourceModel() ? sourceModel()->columnCount() : 0;
}

QModelIndex SpreadsheetProxy::mapToSource(QModelIndex proxyIndex) const {
	if (!proxyIndex.isValid()) return {};
	const int src_row = visible_rows_.at(static_cast<size_t>(proxyIndex.row()));
	return sourceModel()->index(src_row, proxyIndex.column());
}

QVariant SpreadsheetProxy::displayData(int source_row, int source_column) const {
	const QModelIndex src = sourceModel()->index(source_row, source_column);
	return sourceModel()->data(src, Qt::DisplayRole);
}

QVariant SpreadsheetProxy::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || !sourceModel()) return {};

	if (role == Qt::DecorationRole && icon_column >= 0) {
		const int src_row = visible_rows_.at(static_cast<size_t>(index.row()));
		return sourceModel()->data(sourceModel()->index(src_row, icon_column), Qt::DecorationRole);
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
					QString s = QString::fromUtf8(dn.data(), static_cast<int>(dn.size()));
					s.replace('&', "");
					return s;
				}
			}
		} catch (...) {}
	}

	return QString::fromStdString(field);
}

// ============================================================================
// SpreadsheetDelegate
// ============================================================================

void SpreadsheetDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
	auto opts = option;
	opts.decorationSize = {16, 16};
	painter->save();

	QColor text_color;
	QString field;

	if (const auto* proxy = qobject_cast<const SpreadsheetProxy*>(index.model())) {
		QModelIndex src = proxy->mapToSource(index);
		if (src.isValid() && proxy->slk &&
			static_cast<size_t>(src.column()) < proxy->slk->index_to_column.size()) {
			field = QString::fromStdString(proxy->slk->index_to_column.at(src.column()));
		}
	}

	static const std::map<std::string, QColor> text_colours = {
		{"goldcost",   QColor(255, 200, 60)},
		{"lumbercost", QColor(100, 220, 100)},
		{"goldbase",   QColor(255, 200, 60)},
		{"lumberbase", QColor(100, 220, 100)},
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

	opts.decorationAlignment = Qt::AlignLeft | Qt::AlignVCenter;
	opts.decorationPosition = QStyleOptionViewItem::Left;
	opts.decorationSize = {16, 16};

	QStyledItemDelegate::paint(painter, opts, index);
	painter->restore();
}

QSize SpreadsheetDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
	QSize s = QStyledItemDelegate::sizeHint(option, index);
	s.setHeight(std::max(s.height(), 20));
	return s;
}

// ============================================================================
// SpreadsheetView — Ctrl+wheel zoom, Shift+wheel horizontal scroll
// ============================================================================

void SpreadsheetView::wheelEvent(QWheelEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	int delta = event->angleDelta().y();
#else
	int delta = event->delta();
#endif
	if (event->modifiers() & Qt::ControlModifier) {
		if (delta != 0) {
			int h = verticalHeader()->defaultSectionSize();
			h = std::clamp(h + (delta > 0 ? 2 : -2), 10, 80);
			verticalHeader()->setDefaultSectionSize(h);
			int icon_sz = std::clamp(h - 6, 8, 64);
			setIconSize({icon_sz, icon_sz});
		}
		event->accept();
		return;
	}
	if (event->modifiers() & Qt::ShiftModifier) {
		horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta);
		event->accept();
		return;
	}
	QTableView::wheelEvent(event);
}

// ============================================================================
// SpreadsheetEditor
// ============================================================================

static QString groupForField(const std::string& field) {
	// General: identity / classification / name
	if (field == "name" || field == "name1" || field == "editorname" ||
	    field == "bufftip" || field == "buff" || field == "code" || field == "comment" ||
	    field == "version" || field == "editorSuffix" || field == "useSpecific" ||
	    field == "missileType" || field == "isbldg" || field == "unitSound" ||
	    field == "unitType" || field == "unitClass")
		return "General";

	// Race / Class / Level
	if (field == "race" || field == "class" || field == "level" || field == "levels" ||
	    field == "primary" || field == "tilesets" || field == "campaign" ||
	    field == "special" || field == "inBeta")
		return "Race / Class";

	// Combat
	if (field.starts_with("def") || field.starts_with("dmg") || field.starts_with("dice") ||
	    field.starts_with("sides") || field.starts_with("range") || field.starts_with("targs") ||
	    field.starts_with("weap") || field.starts_with("cool") || field.starts_with("dur") ||
	    field.starts_with("heroDur") || field.starts_with("cost") || field.starts_with("area") ||
	    field.starts_with("cast") || field == "spd" || field == "sight" ||
	    field.starts_with("armor") || field.starts_with("backSw") ||
	    field.starts_with("atk") || field == "attacks" || field == "preventDef" ||
	    field == "nbrandom" || field == "half" || field == "naval")
		return "Combat";

	// HP / Mana / Regen
	if (field.starts_with("hp") || field.starts_with("mana") || field == "regenhp" ||
	    field == "regenmana" || field == "regenType" || field == "regen" ||
	    field == "starthp" || field == "startmana" || field == "food" ||
	    field == "bldtm" || field == "regenMana" || field == "regenHP")
		return "HP / Mana / Regen";

	// Cost
	if (field.starts_with("gold") || field.starts_with("lumber") ||
	    field.starts_with("fmad") || field.starts_with("fuse") ||
	    field.starts_with("stock") || field.starts_with("sell") ||
	    field == "pickrandom" || field == "perishable" || field == "prio" ||
	    field == "used" || field == "uses" || field == "useCost")
		return "Cost / Resources";

	// Art / Icons / Models
	if (field == "file" || field.starts_with("Art") || field.starts_with("Icon") ||
	    field.starts_with("model") || field.starts_with("Model") || field == "portrait" ||
	    field.starts_with("Button") || field.starts_with("anim") || field == "Extra" ||
	    field.starts_with("Scale") || field.starts_with("scale") ||
	    field == "shadow" || field == "deathType" || field == "death" ||
	    field.starts_with("specialart") || field.starts_with("missileart") ||
	    field.starts_with("projectile") || field.starts_with("targetart") ||
	    field.starts_with("effect") || field.starts_with("caster") ||
	    field.starts_with("lightning") || field.starts_with("spawn") ||
	    field == "occH" || field == "tilesetSpecific")
		return "Art / Model";

	// Sound
	if (field.starts_with("sound") || field.starts_with("Sound") ||
	    field == "moveSound" || field == "constructionSound" ||
	    field == "loopSound" || field == "randomSound")
		return "Sound";

	// Pathing / Movement
	if (field.starts_with("path") || field.starts_with("coll") ||
	    field == "turnRate" || field == "propwin" || field == "movetp" ||
	    field == "moveHeight" || field == "walk" || field == "targType" ||
	    field == "setu" || field == "setd")
		return "Pathing / Movement";

	// Techtree
	if (field.starts_with("research") || field.starts_with("upgrade") ||
	    field.starts_with("require") || field.starts_with("dep") ||
	    field.starts_with("techtree") || field.starts_with("parent") ||
	    field.starts_with("check") || field == "tier" || field == "hero" ||
	    field.starts_with("inherit") || field.starts_with("train") ||
	    field.starts_with("build") || field.starts_with("revive"))
		return "Techtree";

	// Text / Tooltips
	if (field.starts_with("tooltip") || field.starts_with("tip") ||
	    field.starts_with("ubertip") || field.starts_with("description") ||
	    field.starts_with("hotkey") || field.starts_with("name") ||
	    field.starts_with("awaken") || field.starts_with("reviveTip") ||
	    field.starts_with("editor"))
		return "Text / Tooltips";

	return "Other";
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

	safe_add("Units", units_table, "name", "file",
		S{ "name", "race", "hp", "manan", "regenhp", "regenmana", "def", "dmgplus1", "dice1", "sides1",
		   "spd", "goldcost", "lumbercost", "fmade", "fused", "level", "primary", "sight" }, true);
	safe_add("Items", items_table, "name", "file",
		S{ "name", "goldcost", "lumbercost", "level", "class", "prio", "perishable", "pickrandom", "hp" });
	safe_add("Abilities", abilities_table, "name", "Art",
		S{ "name", "code", "race", "levels", "targs1" });
	safe_add("Doodads", doodads_table, "name", "file",
		S{ "name", "category", "file" });
	safe_add("Destructibles", destructibles_table, "name", "file",
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
	                                                 name_field, icon_field, this);

	const int columns = table->slk->columns();

	// --- frozen view: shows only the name column ---
	SpreadsheetView* frozen_view = new SpreadsheetView;
	frozen_view->setModel(proxy);
	frozen_view->setSortingEnabled(false);
	frozen_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	frozen_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	frozen_view->setContextMenuPolicy(Qt::CustomContextMenu);
	frozen_view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	frozen_view->setAlternatingRowColors(true);
	frozen_view->setIconSize({16, 16});
	frozen_view->setItemDelegate(new SpreadsheetDelegate(frozen_view));
	frozen_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	frozen_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	frozen_view->verticalHeader()->setDefaultSectionSize(22);
	frozen_view->verticalHeader()->setVisible(false);
	frozen_view->horizontalHeader()->setMinimumSectionSize(40);
	frozen_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	frozen_view->horizontalHeader()->setStretchLastSection(false);
	frozen_view->horizontalHeader()->setMinimumHeight(36);
	QFont frozen_font = frozen_view->horizontalHeader()->font();
	frozen_font.setPointSize(std::max(7, frozen_font.pointSize() - 1));
	frozen_view->horizontalHeader()->setFont(frozen_font);
	// Show only column 0 (name)
	for (int c = 0; c < columns; c++) {
		frozen_view->setColumnHidden(c, c != proxy->nameColumn());
	}

	// --- main view ---
	SpreadsheetView* view = new SpreadsheetView;
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
	view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	view->setIconSize({16, 16});
	view->setItemDelegate(new SpreadsheetDelegate(view));

	view->horizontalHeader()->setMinimumHeight(36);
	QFont hdr_font = view->horizontalHeader()->font();
	hdr_font.setPointSize(std::max(7, hdr_font.pointSize() - 1));
	view->horizontalHeader()->setFont(hdr_font);

	// Hide name column in main view (shown frozen)
	view->setColumnHidden(proxy->nameColumn(), true);

	// Column visibility from QSettings
	QString settings_key = "Spreadsheet/columns/" + name;
	QSettings settings;
	const QStringList saved_hidden = settings.value(settings_key).toString().split(',',
		Qt::SkipEmptyParts);

	const std::set<std::string> visible(curated.begin(), curated.end());
	bool has_saved = !saved_hidden.isEmpty();

	for (int c = 0; c < columns; c++) {
		if (c == proxy->nameColumn()) continue; // handled by frozen view
		const std::string& field = table->slk->index_to_column.at(static_cast<size_t>(c));
		if (has_saved) {
			view->setColumnHidden(c, saved_hidden.contains(QString::fromStdString(field)));
		} else {
			view->setColumnHidden(c, !visible.contains(field));
		}
	}

	bool any_visible = false;
	for (int c = 0; c < columns; c++) {
		if (c != proxy->nameColumn() && !view->isColumnHidden(c)) {
			any_visible = true; break;
		}
	}
	if (!any_visible) {
		for (int c = 0; c < columns; c++) {
			if (c != proxy->nameColumn()) view->setColumnHidden(c, false);
		}
	}

	// Auto-size icon column
	if (proxy->iconColumn() >= 0 && proxy->iconColumn() != proxy->nameColumn() &&
	    !view->isColumnHidden(proxy->iconColumn())) {
		view->setColumnWidth(proxy->iconColumn(), 40);
	}

	// Sync vertical scrolling between frozen and main view
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

	// Sort via header clicks
	view->setSortingEnabled(false);
	frozen_view->setSortingEnabled(false);
	auto sort_fn = [proxy](int logicalIndex) {
		static int last_col = -1;
		static Qt::SortOrder last_order = Qt::AscendingOrder;
		if (logicalIndex == last_col) {
			last_order = (last_order == Qt::AscendingOrder)
			                 ? Qt::DescendingOrder : Qt::AscendingOrder;
		} else {
			last_col = logicalIndex;
			last_order = Qt::AscendingOrder;
		}
		proxy->sort(logicalIndex, last_order);
	};
	connect(view->horizontalHeader(), &QHeaderView::sectionClicked, proxy, sort_fn);
	connect(frozen_view->horizontalHeader(), &QHeaderView::sectionClicked, proxy, [proxy](int) {
		proxy->sort(proxy->nameColumn(),
			(proxy->sort_column == proxy->nameColumn() && proxy->sort_order == Qt::AscendingOrder)
				? Qt::DescendingOrder : Qt::AscendingOrder);
	});

	// Right-click → Batch edit
	connect(view, &QTableView::customContextMenuRequested, this,
	        [this, view, proxy, table](const QPoint& pos) {
		QMenu menu;
		QAction* batch = menu.addAction("Batch edit...");
		batch->setEnabled(!view->selectionModel()->selectedRows().isEmpty());
		const QModelIndex at = view->indexAt(pos);
		const int preferred_column = at.isValid() ? at.column() : -1;
		connect(batch, &QAction::triggered, this,
		        [this, view, proxy, table, preferred_column]() {
			openBatchDialog(view, proxy, table, preferred_column);
		});
		menu.exec(view->viewport()->mapToGlobal(pos));
	});
	connect(frozen_view, &QTableView::customContextMenuRequested, this,
	        [this, frozen_view, proxy, table](const QPoint& pos) {
		QMenu menu;
		QAction* batch = menu.addAction("Batch edit...");
		batch->setEnabled(!frozen_view->selectionModel()->selectedRows().isEmpty());
		const QModelIndex at = frozen_view->indexAt(pos);
		const int preferred_column = at.isValid() ? at.column() : -1;
		connect(batch, &QAction::triggered, this,
		        [this, frozen_view, proxy, table, preferred_column]() {
			openBatchDialog(frozen_view, proxy, table, preferred_column);
		});
		menu.exec(frozen_view->viewport()->mapToGlobal(pos));
	});

	// Toolbar
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
		QToolButton* building = new QToolButton;
		building->setText("Building");
		building->setCheckable(true);
		building->setToolTip("Show only buildings (isbldg = 1)");
		connect(building, &QToolButton::toggled, proxy, &SpreadsheetProxy::setBuildingFilter);
		bar->addSeparator();
		bar->addWidget(building);
	}

	// Editor suffix as text input
	{
		QLineEdit* suffix_edit = new QLineEdit;
		suffix_edit->setPlaceholderText("editor suffix");
		suffix_edit->setClearButtonEnabled(true);
		suffix_edit->setMaximumWidth(120);
		suffix_edit->setToolTip("Filter by editorSuffix / version field");
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

	QToolButton* columns_button = new QToolButton;
	columns_button->setText("Columns...");
	connect(columns_button, &QToolButton::clicked, this,
	        [this, view, proxy, table, curated]() {
		openColumnDialog(view, proxy, table, curated);
	});
	bar->addWidget(columns_button);

	// Build container: toolbar + splitter with frozen + main
	QSplitter* splitter = new QSplitter(Qt::Horizontal);
	splitter->setChildrenCollapsible(false);
	splitter->addWidget(frozen_view);
	splitter->addWidget(view);
	splitter->setSizes({170, 900});

	QWidget* container = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(bar);
	layout->addWidget(splitter);

	tabs->addTab(container, name);
}

// ============================================================================
// Column visibility dialog (tree + search + restore defaults)
// ============================================================================

void SpreadsheetEditor::openColumnDialog(SpreadsheetView* view, SpreadsheetProxy* proxy,
                                          TableModel* table, const std::vector<std::string>& curated) {
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

	const int columns = table->slk->columns();
	const std::set<std::string> default_set(curated.begin(), curated.end());

	// Build groups
	std::map<QString, std::vector<std::pair<int, QString>>> groups;
	for (int c = 0; c < columns; c++) {
		if (c == proxy->nameColumn()) continue;
		const std::string& field = table->slk->index_to_column.at(static_cast<size_t>(c));
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) label = QString::fromStdString(field);
		QString gname = groupForField(field);
		groups[gname].push_back({c, label});
	}

	// Desired order of top-level groups
	const std::vector<QString> ordered_groups = {
		"General", "Race / Class", "Combat", "HP / Mana / Regen",
		"Cost / Resources", "Art / Model", "Sound",
		"Pathing / Movement", "Techtree", "Text / Tooltips", "Other",
	};

	// Create tree items
	QMap<QString, QTreeWidgetItem*> group_items;
	std::map<QTreeWidgetItem*, int> item_to_col;
	std::map<QTreeWidgetItem*, bool> item_to_checked;

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
			bool is_visible = !view->isColumnHidden(col);
			child->setCheckState(0, is_visible ? Qt::Checked : Qt::Unchecked);
			item_to_col[child] = col;
			item_to_checked[child] = is_visible;
		}
	}

	tree->expandAll();

	// Search filter
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

	// Buttons
	QHBoxLayout* btn_row = new QHBoxLayout;
	QPushButton* defaults_btn = new QPushButton("Restore defaults");
	connect(defaults_btn, &QPushButton::clicked, tree, [tree, proxy, columns, default_set, &item_to_col]() {
		for (int i = 0; i < tree->topLevelItemCount(); ++i) {
			QTreeWidgetItem* gitem = tree->topLevelItem(i);
			for (int j = 0; j < gitem->childCount(); ++j) {
				QTreeWidgetItem* child = gitem->child(j);
				int col = item_to_col[child];
				const std::string& field = proxy->slk->index_to_column.at(static_cast<size_t>(col));
				bool visible = default_set.contains(field);
				child->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
			}
		}
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

	btn_row->addWidget(defaults_btn);
	btn_row->addWidget(show_all_btn);
	btn_row->addStretch();
	main_layout->addLayout(btn_row);

	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	main_layout->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted) return;

	// Apply visibility
	QStringList hidden_fields;
	for (int i = 0; i < tree->topLevelItemCount(); ++i) {
		QTreeWidgetItem* gitem = tree->topLevelItem(i);
		for (int j = 0; j < gitem->childCount(); ++j) {
			QTreeWidgetItem* child = gitem->child(j);
			int col = item_to_col[child];
			bool hidden = (child->checkState(0) == Qt::Unchecked);
			view->setColumnHidden(col, hidden);
			if (hidden) {
				const std::string& field = table->slk->index_to_column.at(static_cast<size_t>(col));
				hidden_fields.append(QString::fromStdString(field));
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
