#pragma once

#include <QMainWindow>
#include <QSortFilterProxyModel>
#include <QTabWidget>

#include <string>
#include <vector>

import TableModel;
import SLK;

// Proxy over a TableModel (rows = objects, columns = SLK fields).
// Adds proper, localized horizontal headers (field display names) and
// object-id vertical headers. Row filtering is layered on in a later step.
class SpreadsheetProxy : public QSortFilterProxyModel {
	Q_OBJECT

  public:
	explicit SpreadsheetProxy(TableModel* table, QObject* parent = nullptr);

	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;

  private:
	QString fieldDisplayName(int source_column) const;
};

// Separate, read-/edit-capable global table view of all object types, sitting
// next to (and deliberately independent of) the regular Object Editor.
class SpreadsheetEditor : public QMainWindow {
	Q_OBJECT

  public:
	explicit SpreadsheetEditor(QWidget* parent = nullptr);

  private:
	QTabWidget* tabs = nullptr;

	// Builds one category tab: a QTableView over `table`, defaulting to the
	// `curated` set of visible field columns plus a "Columns…" toggle.
	void addCategoryTab(const QString& name, TableModel* table, const std::vector<std::string>& curated);
};
