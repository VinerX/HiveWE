#pragma once

#include <QMainWindow>
#include <QIdentityProxyModel>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QTabWidget>
#include <QTableView>
#include <QVariant>

#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>

import TableModel;
import SLK;

struct SpreadsheetComputedColumn {
	enum class Kind {
		FormulaNumber,
		CombinedText,
		EditorText,    // editable, user-stored free text — never written to the map's object data
		EditorNumber,  // editable, user-stored number — never written to the map's object data
	};

	// Where the per-row values of an editor column live.
	enum class Storage {
		Local,  // QSettings on this machine (does not travel with the map)
		InMap,  // war3mapSkin.w3u; stripped on "Export for game" (travels with the map)
	};

	QString key;
	QString title;
	QString group;
	QString formula;
	Kind kind = Kind::FormulaNumber;
	bool builtin = false;
	Storage storage = Storage::Local;
	std::map<std::string, QString> values;  // row id -> value (editor columns only)

	bool isEditor() const { return kind == Kind::EditorText || kind == Kind::EditorNumber; }
};

class SpreadsheetProxy : public QIdentityProxyModel {
	Q_OBJECT

  public:
	SpreadsheetProxy(QAbstractItemModel* source_model,
	                 slk::SLK* data_slk, slk::SLK* meta_slk,
	                 std::string name_field,
	                 std::string icon_field = {},
	                 QString category_name = {},
	                 QObject* parent = nullptr);

	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
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
	void setUnitTypeFilter(bool show_buildings, bool show_units);
	void setEditorSuffixFilter(const QString& suffix);
	void setFieldFilter(const QString& field_name, const QString& text);

	QModelIndex mapToSource(QModelIndex proxyIndex) const;

	void reapplyFilters();
	void sort(int column, Qt::SortOrder order);

	int iconColumn() const { return icon_column; }
	int nameColumn() const { return name_column; }
	int sourceColumnCount() const;
	bool isComputedColumn(int column) const;   // any virtual column (formula or editor)
	bool isEditorColumn(int column) const;     // editable, user-stored virtual column
	bool isEditableColumn(int column) const;
	QString columnKey(int column) const;
	QString groupName(int column) const;
	int findColumnByKey(const QString& key) const;
	bool validateFormula(const QString& formula, QString* error_message = nullptr) const;
	int addCustomFormulaColumn(const QString& title, const QString& formula,
	                           const QString& group = "Computed",
	                           QString* error_message = nullptr);
	int addEditorColumn(const QString& title, const QString& group,
	                    SpreadsheetComputedColumn::Kind kind,
	                    SpreadsheetComputedColumn::Storage storage,
	                    QString* error_message = nullptr);
	bool removeCustomFormulaColumn(const QString& key);

	std::vector<int> visibleRowsForTest() const { return visible_rows_; }
	std::vector<SpreadsheetComputedColumn> computedColumnsForTest() const { return computed_columns_; }

  public:
	QString fieldDisplayName(int source_column) const;

	std::string name_field;
	std::string icon_field;
	QString category_name;
	int name_column = -1;
	int icon_column = -1;
	QString text_filter;
	bool custom_only = false;
	std::string race_filter;
	bool show_buildings = true;
	bool show_units = true;
	QString editor_suffix;
	std::string field_filter_name;
	QString field_filter_text;
	int sort_column = -1;
	Qt::SortOrder sort_order = Qt::AscendingOrder;
	std::vector<SpreadsheetComputedColumn> computed_columns_;

	std::vector<int> visible_rows_;
	void rebuildVisibleRows();
	void rebuildSortOrder();
	void populateBuiltInComputedColumns();
	void loadCustomComputedColumns();   // local (QSettings) formula + editor columns
	void saveCustomComputedColumns() const;
	void loadInMapColumns();            // in-map editor columns (war3map.hivewe_fields.json)
	void saveInMapColumns() const;
	void persistEditorColumn(const SpreadsheetComputedColumn& column) const;
	QVariant computedData(int source_row, const SpreadsheetComputedColumn& column, int role) const;
	bool evaluateFormula(const QString& formula, int source_row, double& value,
	                     QString* error_message = nullptr) const;
	QString computedTextValue(const SpreadsheetComputedColumn& column, int source_row) const;
	std::string rowIdForSourceRow(int source_row) const;
	double numericFieldValue(const QString& field_name, int source_row) const;

	QVariant displayData(int source_row, int source_column) const;
};

class SpreadsheetDelegate : public QStyledItemDelegate {
	Q_OBJECT
  public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override;
	QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
	QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
	                      const QModelIndex& index) const override;
};

// Header that word-wraps long column labels, shrinking font if needed
class WordWrapHeader : public QHeaderView {
	Q_OBJECT
  public:
	explicit WordWrapHeader(Qt::Orientation orientation, QWidget* parent = nullptr)
		: QHeaderView(orientation, parent) {}

  protected:
	void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override;
	QSize sectionSizeFromContents(int logicalIndex) const override;
};

class SpreadsheetView : public QTableView {
	Q_OBJECT
  public:
	using QTableView::QTableView;

	SpreadsheetView* peer = nullptr;

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
	void openColumnDialog(SpreadsheetView* view, SpreadsheetView* frozen_view, SpreadsheetProxy* proxy, TableModel* table,
	                      const std::vector<std::string>& curated,
	                      const std::function<void()>& full_reset);
};
