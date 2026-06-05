#include "spreadsheet_editor.h"

#include <QTableView>
#include <QHeaderView>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QVBoxLayout>
#include <QWidget>

import std;
import Globals;

// ---------------------------------------------------------------------------
// SpreadsheetProxy
// ---------------------------------------------------------------------------

SpreadsheetProxy::SpreadsheetProxy(TableModel* table, QObject* parent)
	: QSortFilterProxyModel(parent), slk(table->slk), meta_slk(table->meta_slk) {
	setSourceModel(table);
	// Sort on the raw stored value so numeric columns sort numerically-ish and
	// strings alphabetically, matching what the user typed/sees.
	setSortRole(Qt::EditRole);
	setFilterCaseSensitivity(Qt::CaseInsensitive);
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
	addCategoryTab("Units", units_table,
		S{ "name", "race", "hp", "manan", "regenhp", "regenmana", "def", "dmgplus1", "dice1", "sides1",
		   "spd", "goldcost", "lumbercost", "fmade", "fused", "level", "primary", "sight" });
	addCategoryTab("Items", items_table,
		S{ "name", "goldcost", "lumbercost", "level", "class", "prio", "perishable", "pickrandom", "hp" });
	addCategoryTab("Abilities", abilities_table,
		S{ "name", "code", "race", "levels", "targs1" });
	addCategoryTab("Doodads", doodads_table,
		S{ "name", "category", "file" });
	addCategoryTab("Destructibles", destructibles_table,
		S{ "name", "category", "file" });
	addCategoryTab("Upgrades", upgrade_table,
		S{ "name1", "race", "class", "maxlevel", "goldbase", "lumberbase" });
	addCategoryTab("Buffs", buff_table,
		S{ "editorname", "bufftip", "race" });

	show();
}

void SpreadsheetEditor::addCategoryTab(const QString& name, TableModel* table, const std::vector<std::string>& curated) {
	SpreadsheetProxy* proxy = new SpreadsheetProxy(table, this);

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

	// Toolbar: "Columns…" toggles visibility of any field.
	QToolBar* bar = new QToolBar;
	bar->setMovable(false);

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
