#pragma once

#include <QMainWindow>
#include <QIdentityProxyModel>
#include <QTabWidget>
#include <QTableView>

#include <string>
#include <vector>
#include <set>

import TableModel;
import SLK;

// Transparent proxy over a TableModel (rows = objects, columns = SLK fields).
// Provides localized column headers (field display names) and object-id
// vertical headers.  Manual filtering replaces QSortFilterProxyModel to avoid
// SIGSEGV crashes caused by Qt's internal proxy-mapping engine.
class SpreadsheetProxy : public QIdentityProxyModel {
	Q_OBJECT

  public:
	// `source_model` is the underlying QAbstractItemModel (normally a TableModel).
	// `data_slk` / `meta_slk` provide the SLK handles needed for header lookups
	// and filter checks. `name_field` is the SLK column used for text search.
	SpreadsheetProxy(QAbstractItemModel* source_model,
	                 slk::SLK* data_slk, slk::SLK* meta_slk,
	                 std::string name_field,
	                 QObject* parent = nullptr);

	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	// Flat list – no hierarchy.
	QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex&) const override { return {}; }
	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;

	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;

	void setTextFilter(const QString& text);
	void setCustomOnly(bool custom_only);
	void setRaceFilter(const QString& race_key); // empty = all races

	// Returns the *source* model index for the given visible proxy row.
	QModelIndex mapToSource(QModelIndex proxyIndex) const;

	// Re-fills visible_rows_ from the source model, applying all active filters.
	void reapplyFilters();

  private:
	QString fieldDisplayName(int source_column) const;

	std::string name_field;
	int name_column = -1;
	QString text_filter;
	bool custom_only = false;
	std::string race_filter;

	// Ordered list of source rows that currently pass all filters.
	std::vector<int> visible_rows_;
	void rebuildVisibleRows();
};

class SpreadsheetEditor : public QMainWindow {
	Q_OBJECT

  public:
	explicit SpreadsheetEditor(QWidget* parent = nullptr);

  private:
	QTabWidget* tabs = nullptr;

	void addCategoryTab(
		const QString& name,
		TableModel* table,
		const std::string& name_field,
		const std::vector<std::string>& curated,
		bool race_filter = false
	);

	void openBatchDialog(QTableView* view, SpreadsheetProxy* proxy, TableModel* table, int preferred_column);
};
