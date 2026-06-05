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
// SpreadsheetProxy
// ---------------------------------------------------------------------------

SpreadsheetProxy::SpreadsheetProxy(TableModel* table, std::string name_field, QObject* parent)
	: QSortFilterProxyModel(parent), slk(table->slk), meta_slk(table->meta_slk), name_field(std::move(name_field)) {
	setSourceModel(table);
	// Sort on the raw stored value so numeric columns sort numerically-ish and
	// strings alphabetically, matching what the user typed/sees.
	setSortRole(Qt::EditRole);
	setFilterCaseSensitivity(Qt::CaseInsensitive);

	if (const auto found = slk->column_headers.find(this->name_field); found != slk->column_headers.end()) {
		name_column = static_cast<int>(found->second);
	}
}

void SpreadsheetProxy::setTextFilter(const QString& text) {
	beginFilterChange();
	text_filter = text;
	endFilterChange(Direction::Rows);
}

void SpreadsheetProxy::setCustomOnly(bool only) {
	beginFilterChange();
	custom_only = only;
	endFilterChange(Direction::Rows);
}

void SpreadsheetProxy::setRaceFilter(const QString& race_key) {
	beginFilterChange();
	race_filter = race_key.toStdString();
	endFilterChange(Direction::Rows);
}

bool SpreadsheetProxy::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
	if (source_row < 0 || static_cast<size_t>(source_row) >= slk->index_to_row.size()) {
		return false;
	}
	const std::string& id = slk->index_to_row.at(source_row);

	// Custom-only: keep just objects with a custom shadow override (oldid).
	if (custom_only) {
		const auto it = slk->shadow_data.find(id);
		if (it == slk->shadow_data.end() || !it->second.contains("oldid")) {
			return false;
		}
	}

	// Race filter (units): compare the raw race key.
	if (!race_filter.empty() && slk->column_headers.contains("race")) {
		if (slk->data<std::string_view>("race", id) != race_filter) {
			return false;
		}
	}

	// Name text search on the display value of the name column.
	if (!text_filter.isEmpty() && name_column >= 0) {
		const QString name = sourceModel()->data(sourceModel()->index(source_row, name_column), Qt::DisplayRole).toString();
		if (!name.contains(text_filter, Qt::CaseInsensitive)) {
			return false;
		}
	}

	return true;
}

QString SpreadsheetProxy::fieldDisplayName(int source_column) const {
	if (source_column < 0 || static_cast<size_t>(source_column) >= slk->index_to_column.size()) {
		return {};
	}

	const std::string& field = slk->index_to_column.at(source_column);

	// Resolve the human-readable field name through the meta SLK, the same path
	// the Object Editor's SingleModel uses. Display name is independent of the
	// concrete object, so any existing row works as the lookup anchor.
	if (slk->rows() > 0) {
		const std::string& rep_id = slk->index_to_row.at(0);
		if (const auto meta_id = slk->field_to_meta_id(*meta_slk, field, rep_id)) {
			const std::string_view dn = meta_slk->data<std::string_view>("displayname", *meta_id);
			if (!dn.empty()) {
				QString s = QString::fromUtf8(dn);
				s.replace('&', "");
				return s;
			}
		}
	}

	// Fallback: the raw field key (still meaningful, e.g. "hp", "goldcost").
	return QString::fromStdString(field);
}

QVariant SpreadsheetProxy::headerData(int section, Qt::Orientation orientation, int role) const {
	if (role == Qt::DisplayRole) {
		if (orientation == Qt::Horizontal) {
			// Columns are never reordered/filtered by this proxy, so the proxy
			// column index equals the source column index.
			return fieldDisplayName(section);
		} else {
			if (section >= 0 && section < rowCount()) {
				const QModelIndex src = mapToSource(index(section, 0));
				if (src.isValid() && static_cast<size_t>(src.row()) < slk->index_to_row.size()) {
					return QString::fromStdString(slk->index_to_row.at(src.row()));
				}
			}
		}
	}
	return QSortFilterProxyModel::headerData(section, orientation, role);
}

// ---------------------------------------------------------------------------
// SpreadsheetEditor
// ---------------------------------------------------------------------------

SpreadsheetEditor::SpreadsheetEditor(QWidget* parent) : QMainWindow(parent) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle("Spreadsheet");
	resize(1280, 800);

	tabs = new QTabWidget;
	tabs->setDocumentMode(true);
	setCentralWidget(tabs);

	using S = std::vector<std::string>;

	// Curated default columns per category (lowercased SLK field keys; missing
	// keys are silently skipped). Everything else is reachable via "Columns…".
	addCategoryTab("Units", units_table, "name",
		S{ "name", "race", "hp", "manan", "regenhp", "regenmana", "def", "dmgplus1", "dice1", "sides1",
		   "spd", "goldcost", "lumbercost", "fmade", "fused", "level", "primary", "sight" }, true);
	addCategoryTab("Items", items_table, "name",
		S{ "name", "goldcost", "lumbercost", "level", "class", "prio", "perishable", "pickrandom", "hp" });
	addCategoryTab("Abilities", abilities_table, "name",
		S{ "name", "code", "race", "levels", "targs1" });
	addCategoryTab("Doodads", doodads_table, "name",
		S{ "name", "category", "file" });
	addCategoryTab("Destructibles", destructibles_table, "name",
		S{ "name", "category", "file" });
	addCategoryTab("Upgrades", upgrade_table, "name1",
		S{ "name1", "race", "class", "maxlevel", "goldbase", "lumberbase" });
	addCategoryTab("Buffs", buff_table, "editorname",
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
	SpreadsheetProxy* proxy = new SpreadsheetProxy(table, name_field, this);

	QTableView* view = new QTableView;
	view->setModel(proxy);
	view->setSortingEnabled(true);
	view->setAlternatingRowColors(true);
	view->setSelectionBehavior(QAbstractItemView::SelectRows);
	view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	view->setContextMenuPolicy(Qt::CustomContextMenu);
	view->setWordWrap(false);
	view->horizontalHeader()->setSectionsMovable(true);
	view->horizontalHeader()->setStretchLastSection(false);
	view->verticalHeader()->setDefaultSectionSize(22);
	view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

	// Columns are not reordered by the proxy, so view/source column indices match.
	const int columns = table->slk->columns();
	const std::set<std::string> visible(curated.begin(), curated.end());
	for (int c = 0; c < columns; c++) {
		const std::string& field = table->slk->index_to_column.at(c);
		view->setColumnHidden(c, !visible.contains(field));
	}

	// Fallback so a tab is never fully blank if none of the curated keys exist.
	bool any_visible = false;
	for (int c = 0; c < columns; c++) {
		if (!view->isColumnHidden(c)) {
			any_visible = true;
			break;
		}
	}
	if (!any_visible) {
		for (int c = 0; c < columns; c++) {
			view->setColumnHidden(c, false);
		}
	}

	view->resizeColumnsToContents();

	// Right-click → Batch edit the selected rows.
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

	// Toolbar: search, filters and the "Columns…" visibility toggle.
	QToolBar* bar = new QToolBar;
	bar->setMovable(false);

	QLineEdit* search = new QLineEdit;
	search->setPlaceholderText("Search " + name);
	search->setClearButtonEnabled(true);
	search->setMaximumWidth(260);
	connect(search, &QLineEdit::textChanged, proxy, &SpreadsheetProxy::setTextFilter);
	bar->addWidget(search);

	// Race filter (units only), sourced from the same unitRace section the tree uses.
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

	QMenu* menu = new QMenu(columns_button);

	std::vector<std::pair<QString, int>> entries;
	entries.reserve(columns);
	for (int c = 0; c < columns; c++) {
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) {
			label = QString::fromStdString(table->slk->index_to_column.at(c));
		}
		entries.emplace_back(label, c);
	}
	std::ranges::sort(entries, [](const auto& a, const auto& b) { return a.first.localeAwareCompare(b.first) < 0; });

	for (const auto& [label, c] : entries) {
		QAction* act = menu->addAction(label);
		act->setCheckable(true);
		act->setChecked(!view->isColumnHidden(c));
		const int col = c;
		connect(act, &QAction::toggled, view, [view, col](bool on) { view->setColumnHidden(col, !on); });
	}
	columns_button->setMenu(menu);
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

	// Collect selected rows as source rows.
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

	// Target field: the currently visible columns.
	QComboBox* field_combo = new QComboBox;
	for (int c = 0; c < slk->columns(); c++) {
		if (view->isColumnHidden(c)) {
			continue;
		}
		QString label = proxy->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
		if (label.isEmpty()) {
			label = QString::fromStdString(slk->index_to_column.at(c));
		}
		field_combo->addItem(label, c);
	}
	if (field_combo->count() == 0) {
		return;
	}
	if (preferred_column >= 0) {
		const int i = field_combo->findData(preferred_column);
		if (i >= 0) {
			field_combo->setCurrentIndex(i);
		}
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

	// Arithmetic operations only make sense for numeric fields.
	auto refresh_ops = [&]() {
		const std::string field = slk->index_to_column.at(field_combo->currentData().toInt());
		const bool numeric = is_numeric(field_type(field));
		op_combo->setEnabled(numeric);
		if (!numeric) {
			op_combo->setCurrentIndex(0); // Set
		}
	};
	connect(field_combo, &QComboBox::currentIndexChanged, &dialog, [&](int) { refresh_ops(); });
	refresh_ops();

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	const int col = field_combo->currentData().toInt();
	const std::string field = slk->index_to_column.at(col);
	const QString op = op_combo->currentData().toString();
	const std::string_view type = field_type(field);
	const bool numeric = is_numeric(type);
	const bool is_int = type == "int";

	auto format_number = [is_int](double v) -> std::string {
		if (is_int) {
			return std::to_string(static_cast<long long>(std::llround(v)));
		}
		return std::format("{}", v);
	};

	// Non-numeric Set: write the raw string to every selected row.
	if (!numeric || op == "set") {
		if (numeric && op == "set") {
			// Validate the number once, then store the canonical form.
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

	// Arithmetic operations.
	bool ok = false;
	const double operand = value_edit->text().toDouble(&ok);
	if (!ok) {
		QMessageBox::warning(this, "Batch edit", "Please enter a valid number.");
		return;
	}

	for (const int r : source_rows) {
		const std::string& id = slk->index_to_row.at(r);
		double current = 0.0;
		const std::string cur_str{ slk->data<std::string_view>(field, id) };
		try {
			current = std::stod(cur_str);
		} catch (...) {
			current = 0.0;
		}

		double result = current;
		if (op == "add") {
			result = current + operand;
		} else if (op == "mul") {
			result = current * operand;
		} else if (op == "pct") {
			result = current * (1.0 + operand / 100.0);
		}

		table->setData(table->index(r, col), QString::fromStdString(format_number(result)), Qt::EditRole);
	}
}
