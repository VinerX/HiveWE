#include "spreadsheet_editor.h"

#include <QTableView>
#include <QHeaderView>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QVBoxLayout>
#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>

import std;
import Globals;

// ---------------------------------------------------------------------------
// SpreadsheetProxy (QIdentityProxyModel — no internal mapping, no SIGSEGV risk)
// ---------------------------------------------------------------------------

SpreadsheetProxy::SpreadsheetProxy(QAbstractItemModel* source_model,
                                   slk::SLK* data_slk, slk::SLK* meta_slk,
                                   std::string name_field, QObject* parent)
	: QIdentityProxyModel(parent), slk(data_slk), meta_slk(meta_slk), name_field(std::move(name_field)) {
	setSourceModel(source_model);

	if (const auto found = slk->column_headers.find(this->name_field); found != slk->column_headers.end()) {
		name_column = static_cast<int>(found->second);
	}

	// Build initial visible_rows_ and notify any attached views.
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
			if (it == slk->shadow_data.end() || !it->second.contains("oldid")) {
				continue;
			}
		}

		if (!race_filter.empty() && slk->column_headers.contains("race")) {
			auto race_val = slk->data<std::string_view>("race", id);
			if (race_val != race_filter) {
				continue;
			}
		}

		if (!text_filter.isEmpty() && name_column >= 0) {
			const QModelIndex src_idx = sourceModel()->index(r, name_column);
			const QString name = sourceModel()->data(src_idx, Qt::DisplayRole).toString();
			if (!name.contains(text_filter, Qt::CaseInsensitive)) {
				continue;
			}
		}

		visible_rows_.push_back(r);
	}
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

// --- Model geometry ---

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

// --- Data / Headers ---

QVariant SpreadsheetProxy::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || !sourceModel()) return {};
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
		} catch (...) {
			// slk::field_to_meta_id can throw std::bad_optional_access for
			// fields not present in the meta SLK (slk.ixx line 248).
		}
	}

	return QString::fromStdString(field);
}

// ---------------------------------------------------------------------------
// SpreadsheetEditor
// ---------------------------------------------------------------------------

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

	auto safe_add = [this](const QString& name, TableModel* table, const std::string& nf,
	                        const std::vector<std::string>& curated, bool rf = false) {
		try {
			addCategoryTab(name, table, nf, curated, rf);
		} catch (const std::exception& e) {
			QMessageBox::warning(this, "Spreadsheet",
				QString("Tab '%1' failed: %2").arg(name, e.what()));
		} catch (...) {
			QMessageBox::warning(this, "Spreadsheet",
				QString("Tab '%1' failed: unknown error").arg(name));
		}
	};

	safe_add("Units", units_table, "name",
		S{ "name", "race", "hp", "manan", "regenhp", "regenmana", "def", "dmgplus1", "dice1", "sides1",
		   "spd", "goldcost", "lumbercost", "fmade", "fused", "level", "primary", "sight" }, true);
	safe_add("Items", items_table, "name",
		S{ "name", "goldcost", "lumbercost", "level", "class", "prio", "perishable", "pickrandom", "hp" });
	safe_add("Abilities", abilities_table, "name",
		S{ "name", "code", "race", "levels", "targs1" });
	safe_add("Doodads", doodads_table, "name",
		S{ "name", "category", "file" });
	safe_add("Destructibles", destructibles_table, "name",
		S{ "name", "category", "file" });
	safe_add("Upgrades", upgrade_table, "name1",
		S{ "name1", "race", "class", "maxlevel", "goldbase", "lumberbase" });
	safe_add("Buffs", buff_table, "editorname",
		S{ "editorname", "bufftip", "race" });

	show();
}

void SpreadsheetEditor::addCategoryTab(
	const QString& name,
	TableModel* table,
	const std::string& name_field,
	const std::vector<std::string>& curated,
	bool race_filter
) {
	if (!table || !table->slk) {
		return;
	}

	SpreadsheetProxy* proxy = new SpreadsheetProxy(table, table->slk, table->meta_slk, name_field, this);

	QTableView* view = new QTableView;
	view->setModel(proxy);
	view->setSortingEnabled(false);
	view->setAlternatingRowColors(true);
	view->setSelectionBehavior(QAbstractItemView::SelectRows);
	view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	view->setContextMenuPolicy(Qt::CustomContextMenu);
	view->setWordWrap(false);
	view->horizontalHeader()->setSectionsMovable(true);
	view->horizontalHeader()->setStretchLastSection(false);
	view->verticalHeader()->setDefaultSectionSize(22);
	view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

	const int columns = table->slk->columns();
	const std::set<std::string> visible(curated.begin(), curated.end());
	for (int c = 0; c < columns; c++) {
		const std::string& field = table->slk->index_to_column.at(static_cast<size_t>(c));
		view->setColumnHidden(c, !visible.contains(field));
	}

	bool any_visible = false;
	for (int c = 0; c < columns; c++) {
		if (!view->isColumnHidden(c)) { any_visible = true; break; }
	}
	if (!any_visible) {
		for (int c = 0; c < columns; c++) {
			view->setColumnHidden(c, false);
		}
	}

	// Right-click → Batch edit
	connect(view, &QTableView::customContextMenuRequested, this, [this, view, proxy, table](const QPoint& pos) {
		QMenu menu;
		QAction* batch = menu.addAction("Batch edit…");
		batch->setEnabled(!view->selectionModel()->selectedRows().isEmpty());
		const QModelIndex at = view->indexAt(pos);
		const int preferred_column = at.isValid() ? at.column() : -1;
		connect(batch, &QAction::triggered, this, [this, view, proxy, table, preferred_column]() {
			openBatchDialog(view, proxy, table, preferred_column);
		});
		menu.exec(view->viewport()->mapToGlobal(pos));
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
			if (key == "NumValues" || key == "Sort" || key.ends_with("_Alt")) {
				continue;
			}
			QString label = QString::fromStdString(value[1]);
			label.replace('&', "");
			race_combo->addItem(label, QString::fromStdString(value[0]));
		}
		connect(race_combo, &QComboBox::currentIndexChanged, proxy, [proxy, race_combo](int) {
			proxy->setRaceFilter(race_combo->currentData().toString());
		});
		bar->addSeparator();
		bar->addWidget(new QLabel(" Race: "));
		bar->addWidget(race_combo);
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
	columns_button->setText("Columns…");
	columns_button->setPopupMode(QToolButton::InstantPopup);
	QMenu* col_menu = new QMenu(columns_button);

	std::vector<std::pair<QString, int>> entries;
	entries.reserve(columns);
	for (int c = 0; c < columns; c++) {
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) {
			label = QString::fromStdString(table->slk->index_to_column.at(static_cast<size_t>(c)));
		}
		entries.emplace_back(label, c);
	}
	std::ranges::sort(entries, [](const auto& a, const auto& b) { return a.first.localeAwareCompare(b.first) < 0; });

	for (const auto& [label, c] : entries) {
		QAction* act = col_menu->addAction(label);
		act->setCheckable(true);
		act->setChecked(!view->isColumnHidden(c));
		const int col = c;
		connect(act, &QAction::toggled, view, [view, col](bool on) { view->setColumnHidden(col, !on); });
	}
	columns_button->setMenu(col_menu);
	bar->addWidget(columns_button);

	QWidget* container = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(bar);
	layout->addWidget(view);

	tabs->addTab(container, name);
}

void SpreadsheetEditor::openBatchDialog(QTableView* view, SpreadsheetProxy* proxy, TableModel* table, int preferred_column) {
	slk::SLK* slk = table->slk;
	slk::SLK* meta_slk = table->meta_slk;

	std::vector<int> source_rows;
	for (const QModelIndex& idx : view->selectionModel()->selectedRows()) {
		const QModelIndex src = proxy->mapToSource(idx);
		if (src.isValid()) {
			source_rows.push_back(src.row());
		}
	}
	if (source_rows.empty()) {
		return;
	}

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
	op_combo->addItem("Multiply (×)", "mul");
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
