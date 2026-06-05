#pragma once

#include <QMainWindow>
#include <QIdentityProxyModel>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>

#include <string>
#include <vector>
#include <set>

import TableModel;
import SLK;

class SpreadsheetProxy : public QIdentityProxyModel {
	Q_OBJECT

  public:
	SpreadsheetProxy(QAbstractItemModel* source_model,
	                 slk::SLK* data_slk, slk::SLK* meta_slk,
	                 std::string name_field,
	                 std::string icon_field = {},
	                 QObject* parent = nullptr);

	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex&) const override { return {}; }
	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;

	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;

	void setTextFilter(const QString& text);
	void setCustomOnly(bool custom_only);
	void setRaceFilter(const QString& race_key);
	void setBuildingFilter(bool buildings_only);
	void setEditorSuffixFilter(const QString& suffix);
	void setFieldFilter(const QString& field_name, const QString& text);

	QModelIndex mapToSource(QModelIndex proxyIndex) const;

	void reapplyFilters();
	void sort(int column, Qt::SortOrder order);

	int iconColumn() const { return icon_column; }
	int nameColumn() const { return name_column; }

	std::vector<int> visibleRowsForTest() const { return visible_rows_; }

  public:
	QString fieldDisplayName(int source_column) const;

	std::string name_field;
	std::string icon_field;
	int name_column = -1;
	int icon_column = -1;
	QString text_filter;
	bool custom_only = false;
	std::string race_filter;
	bool building_only = false;
	QString editor_suffix;
	std::string field_filter_name;
	QString field_filter_text;
	int sort_column = -1;
	Qt::SortOrder sort_order = Qt::AscendingOrder;

	std::vector<int> visible_rows_;
	void rebuildVisibleRows();
	void rebuildSortOrder();

	QVariant displayData(int source_row, int source_column) const;
};

class SpreadsheetDelegate : public QStyledItemDelegate {
	Q_OBJECT
  public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override;
	QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

class SpreadsheetView : public QTableView {
	Q_OBJECT
  public:
	using QTableView::QTableView;

  protected:
	void wheelEvent(QWheelEvent* event) override;
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
		const std::string& icon_field,
		const std::vector<std::string>& curated,
		bool race_filter = false
	);

	void openBatchDialog(SpreadsheetView* view, SpreadsheetProxy* proxy, TableModel* table, int preferred_column);
	void openColumnDialog(SpreadsheetView* view, SpreadsheetProxy* proxy, TableModel* table,
	                      const std::vector<std::string>& curated);
};
