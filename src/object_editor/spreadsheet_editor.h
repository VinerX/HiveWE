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
	// `name_field` is the SLK column used for text search (e.g. "name",
	// "name1", "editorname").
	SpreadsheetProxy(TableModel* table, std::string name_field, QObject* parent = nullptr);

	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;

  public slots:
	void setTextFilter(const QString& text);
	void setCustomOnly(bool custom_only);
	void setRaceFilter(const QString& race_key); // empty = all races

  protected:
	bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
	QString fieldDisplayName(int source_column) const;

	std::string name_field;
	int name_column = -1;
	QString text_filter;
	bool custom_only = false;
	std::string race_filter; // empty = no race filtering
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
	// `curated` set of visible field columns plus a "Columns…" toggle, a name
	// search box, a custom-only toggle and (when race_filter) a race combo.
	void addCategoryTab(
		const QString& name,
		TableModel* table,
		const std::string& name_field,
		const std::vector<std::string>& curated,
		bool race_filter = false
	);
};
