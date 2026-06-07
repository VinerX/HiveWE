#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Coarse asset classification used by the type filter.
enum class AssetType { All, Model, Texture, Sound, Other };

class AssetFilterModel : public QSortFilterProxyModel {
	Q_OBJECT
  public:
	using QSortFilterProxyModel::QSortFilterProxyModel;

	void set_type_filter(AssetType type);
	void set_unused_only(bool unused_only);

protected:
	bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
	bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
	AssetType type_filter = AssetType::All;
	bool unused_only = false;
};

// Tree view that turns a drag-drop of file rows onto a folder/file/empty area
// into a single signal carrying the drop target — the actual move (with the
// dependency-rewrite confirmation) is handled by AssetManager.
class AssetTreeView : public QTreeView {
	Q_OBJECT
  public:
	using QTreeView::QTreeView;

  signals:
	// target_index is the (proxy) index dropped onto; invalid = dropped on empty space (map root).
	void files_dropped(const QModelIndex& target_index);

  protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;
};

class AssetManager : public QDialog {
	Q_OBJECT
  public:
	explicit AssetManager(QWidget* parent = nullptr);

  private:
	void refresh() const;
	void update_status() const;
	void show_context_menu(const QPoint& pos);
	void open_in_editor(const QModelIndex& proxy_index) const;
	void remove_object_references(const std::string& id);

	// Builds the column items (File/Type/Size/Usages) for one file, appending
	// its "used by" rows as children of the first item (unless deps are hidden).
	QList<QStandardItem*> make_file_row(const std::string& full_path, const QString& display, const std::unordered_set<std::string>& used_by) const;
	// Returns (or creates) the folder item for a "a/b/c" path, nesting as needed.
	QStandardItem* ensure_folder(const std::string& folder_path, std::unordered_map<std::string, QStandardItem*>& cache) const;
	// Fills a folder's Size/Usages columns with recursive aggregates; returns {bytes, file_count}.
	std::pair<long long, int> aggregate_folder(QStandardItem* folder) const;
	// The sibling item in another column for the given column-0 item.
	QStandardItem* sibling_column(QStandardItem* item, int column) const;
	// Reconstructs the full relative folder path of a folder item by climbing parents.
	std::string folder_path_for(QStandardItem* item) const;

	// Collects the relative paths of every selected file item.
	std::vector<std::string> selected_file_paths() const;
	// Collects the relative paths of every file under the given item (recursive).
	std::vector<std::string> descendant_file_paths(QStandardItem* item) const;
	// Safely moves/renames the given files, rewriting all detectable references.
	void move_files(const std::vector<std::string>& paths);
	// Moves the given files into target_folder (keeping their filenames).
	void move_files_to_folder(const std::vector<std::string>& paths, const std::string& target_folder);
	// Shared apply path: dry-run preview, confirm if there are dependencies, then move.
	void apply_moves(const std::vector<std::pair<std::string, std::string>>& moves);
	// Resolves a drop onto the tree into a safe move of the current selection.
	void handle_drop(const QModelIndex& target_proxy_index);

	QLineEdit* search_edit;
	QComboBox* type_combo;
	QCheckBox* unused_checkbox;
	QCheckBox* group_checkbox;
	QCheckBox* deps_checkbox;
	AssetTreeView* tree_view;
	QLabel* status_label;
	QStandardItemModel* model;
	AssetFilterModel* filter_model;

	bool group_by_folder = false;
	bool show_dependencies = true;
};
