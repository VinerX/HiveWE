#include "asset_manager.h"

#include <QApplication>
#include <QSizePolicy>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

import std;
import SLK;
import Map;
import MapGlobal;
import Globals;
import SafeMove;
import TableModel;
import ResourceManager;
import QIconResource;
import WindowHandler;
import "object_editor/object_editor.h";

namespace fs = std::filesystem;

// Column layout
enum Column { ColFile = 0, ColType = 1, ColSize = 2, ColUsages = 3, ColumnCount = 4 };

// Custom item data roles
static constexpr int IsUnusedRole = Qt::UserRole;     // bool, on file items
static constexpr int ObjectIdRole = Qt::UserRole + 1; // QString, on usage child items
static constexpr int CategoryRole = Qt::UserRole + 2; // int, on usage child items (-1 = not an object)
static constexpr int IsFileRole = Qt::UserRole + 3;   // bool, on file items
static constexpr int IsFolderRole = Qt::UserRole + 4; // bool, on folder items
static constexpr int SizeRole = Qt::UserRole + 5;     // qlonglong, on file/folder items (raw bytes)
static constexpr int PathRole = Qt::UserRole + 6;     // QString full relative path, on file items

void AssetTreeView::dragEnterEvent(QDragEnterEvent* event) {
	// Only our own internal row drags are interesting.
	if (event->source() == this) {
		event->acceptProposedAction();
	} else {
		QTreeView::dragEnterEvent(event);
	}
}

void AssetTreeView::dragMoveEvent(QDragMoveEvent* event) {
	if (event->source() == this) {
		event->setDropAction(Qt::MoveAction);
		event->accept();
	} else {
		QTreeView::dragMoveEvent(event);
	}
}

void AssetTreeView::dropEvent(QDropEvent* event) {
	if (event->source() != this) {
		QTreeView::dropEvent(event);
		return;
	}
	// We perform the move ourselves (safe_move + confirmation); never let the
	// base class shuffle model rows.
	const QModelIndex target = indexAt(event->position().toPoint());
	event->setDropAction(Qt::IgnoreAction);
	event->accept();
	emit files_dropped(target);
}

AssetType classify_asset_type(const std::string& path) {
	std::string ext = fs::path(path).extension().string();
	for (auto& c : ext) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (ext == ".mdx" || ext == ".mdl") {
		return AssetType::Model;
	}
	if (ext == ".blp" || ext == ".tga" || ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif") {
		return AssetType::Texture;
	}
	if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") {
		return AssetType::Sound;
	}
	return AssetType::Other;
}

QString type_label(const std::string& path) {
	std::string ext = fs::path(path).extension().string();
	if (!ext.empty() && ext.front() == '.') {
		ext.erase(ext.begin());
	}
	for (auto& c : ext) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return ext.empty() ? QString("—") : QString::fromStdString(ext);
}

QString human_size(long long bytes) {
	if (bytes < 0) {
		return {};
	}
	static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
	double value = static_cast<double>(bytes);
	int unit = 0;
	while (value >= 1024.0 && unit < 4) {
		value /= 1024.0;
		unit += 1;
	}
	if (unit == 0) {
		return QString("%1 B").arg(bytes);
	}
	return QString("%1 %2").arg(value, 0, 'f', 1).arg(units[unit]);
}

QIcon get_file_icon(const std::string& path) {
	static const QIcon model_icon = QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView);
	static const QIcon image_icon = QApplication::style()->standardIcon(QStyle::SP_DesktopIcon);
	static const QIcon sound_icon = QApplication::style()->standardIcon(QStyle::SP_MediaVolume);
	static const QIcon file_icon = QFileIconProvider().icon(QFileIconProvider::File);

	switch (classify_asset_type(path)) {
		case AssetType::Model:
			return model_icon;
		case AssetType::Texture:
			return image_icon;
		case AssetType::Sound:
			return sound_icon;
		default:
			return file_icon;
	}
}

QIcon get_object_icon(const TableModel* table, const std::string_view id, const std::string_view art_field) {
	const QVariant v = table->data(id, art_field, Qt::DecorationRole);
	if (v.isValid() && !v.isNull()) {
		return v.value<QIcon>();
	}
	return {};
}

QString get_object_name(const TableModel* table, const std::string_view id, const std::string_view name_field) {
	const QVariant v = table->data(id, name_field, Qt::DisplayRole);
	if (v.isValid() && !v.isNull()) {
		return v.toString();
	}
	return QString::fromStdString(std::string(id));
}

struct ObjectInfo {
	QString display_name;
	QIcon icon;
	int category = -1; // matches ObjectEditor::Category, -1 = not a named object
};

ObjectInfo resolve_used_by_id(const std::string& id) {
	if (id == "loadingscreen") {
		return {"Loading Screen", QApplication::style()->standardIcon(QStyle::SP_DesktopIcon), -1};
	}
	if (id == "map script") {
		return {"Map Script", QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView), -1};
	}
	// MDX transitive reference (path contains a slash)
	if (id.contains('/')) {
		return {QString::fromStdString(id), QFileIconProvider().icon(QFileIconProvider::File), -1};
	}
	const auto display = [&](const QString& name) {
		return name + " (" + QString::fromStdString(id) + ")";
	};
	// Load the category icon used by DoodadTreeModel / DestructibleTreeModel
	const auto category_icon = [](const std::string& section, char cat) -> QIcon {
		for (const auto& [key, value] : world_edit_data.section(section)) {
			if (!key.empty() && key.front() == cat) {
				return resource_manager.load<QIconResource>(value[1]).value()->icon;
			}
		}
		return {};
	};
	if (units_slk.row_headers.contains(id)) {
		return {
			display(get_object_name(units_table, id, "name")),
			get_object_icon(units_table, id, "art"),
			static_cast<int>(ObjectEditor::Category::unit)
		};
	}
	if (items_slk.row_headers.contains(id)) {
		return {
			display(get_object_name(items_table, id, "name")),
			get_object_icon(items_table, id, "art"),
			static_cast<int>(ObjectEditor::Category::item)
		};
	}
	if (abilities_slk.row_headers.contains(id)) {
		return {
			display(get_object_name(abilities_table, id, "name")),
			get_object_icon(abilities_table, id, "art"),
			static_cast<int>(ObjectEditor::Category::ability)
		};
	}
	if (destructibles_slk.row_headers.contains(id)) {
		const std::string_view cat = destructibles_slk.data<std::string_view>("category", id);
		return {
			display(get_object_name(destructibles_table, id, "name")),
			cat.empty() ? QIcon {} : category_icon("DestructibleCategories", cat.front()),
			static_cast<int>(ObjectEditor::Category::destructible)
		};
	}
	if (doodads_slk.row_headers.contains(id)) {
		const std::string_view cat = doodads_slk.data<std::string_view>("category", id);
		return {
			display(get_object_name(doodads_table, id, "name")),
			cat.empty() ? QIcon {} : category_icon("DoodadCategories", cat.front()),
			static_cast<int>(ObjectEditor::Category::doodad)
		};
	}
	if (buff_slk.row_headers.contains(id)) {
		QString name = get_object_name(buff_table, id, "editorname");
		if (name.isEmpty() || name == QString::fromStdString(id)) {
			name = get_object_name(buff_table, id, "bufftip");
		}
		return {display(name), get_object_icon(buff_table, id, "buffart"), static_cast<int>(ObjectEditor::Category::buff)};
	}
	if (upgrade_slk.row_headers.contains(id)) {
		return {
			display(get_object_name(upgrade_table, id, "name1")),
			get_object_icon(upgrade_table, id, "art1"),
			static_cast<int>(ObjectEditor::Category::upgrade)
		};
	}
	// Fallback: likely a sound name
	return {QString::fromStdString(id), QApplication::style()->standardIcon(QStyle::SP_MediaVolume), -1};
}

bool AssetFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
	// Keep folders grouped above files regardless of sort direction.
	const bool left_folder = left.siblingAtColumn(0).data(IsFolderRole).toBool();
	const bool right_folder = right.siblingAtColumn(0).data(IsFolderRole).toBool();
	if (left_folder != right_folder) {
		return sortOrder() == Qt::AscendingOrder ? left_folder : right_folder;
	}

	if (left.column() == ColUsages) {
		return left.data(Qt::DisplayRole).toInt() < right.data(Qt::DisplayRole).toInt();
	}
	if (left.column() == ColSize) {
		return left.siblingAtColumn(0).data(SizeRole).toLongLong() < right.siblingAtColumn(0).data(SizeRole).toLongLong();
	}
	return QSortFilterProxyModel::lessThan(left, right);
}

bool AssetFilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
	const QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);

	// Type / unused filters only apply to file items. Folders and usage rows
	// fall through to the recursive base behaviour (a folder is kept if any of
	// its descendant files pass).
	if (idx.data(IsFileRole).toBool()) {
		if (unused_only && !idx.data(IsUnusedRole).toBool()) {
			return false;
		}
		if (type_filter != AssetType::All) {
			QString p = idx.data(PathRole).toString();
			if (p.isEmpty()) {
				p = idx.data(Qt::DisplayRole).toString();
			}
			if (classify_asset_type(p.toStdString()) != type_filter) {
				return false;
			}
		}
	}
	return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
}

void AssetFilterModel::set_type_filter(AssetType type) {
	if (type_filter != type) {
		beginFilterChange();
		type_filter = type;
		endFilterChange(QSortFilterProxyModel::Direction::Rows);
	}
}

void AssetFilterModel::set_unused_only(bool value) {
	if (unused_only != value) {
		beginFilterChange();
		unused_only = value;
		endFilterChange(QSortFilterProxyModel::Direction::Rows);
	}
}

AssetManager::AssetManager(QWidget* parent) : QDialog(parent) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle("Asset Manager");
	resize(700, 800);

	auto* layout = new QVBoxLayout(this);

	// Filter bar
	auto* search_bar = new QHBoxLayout;
	search_edit = new QLineEdit(this);
	search_edit->setPlaceholderText("Search files...");
	search_bar->addWidget(search_edit);

	auto* refresh_button = new QPushButton(this);
	refresh_button->setIcon(QIcon("data/icons/asset_manager/refresh.png"));
	refresh_button->setIconSize(QSize(16, 16));
	refresh_button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
	search_bar->addWidget(refresh_button);

	auto* info_button = new QLabel(this);
	info_button->setPixmap(QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(QSize(16, 16)));
	info_button->setToolTip(
		"Usage count detection is not perfect and can show files as being unused even if they're actually used.\n"
		"It has difficulty detecting game overrides or files used in the game code if they're not using forward slashes.\n"
		"Be careful deleting them based only on the \"Usages\" number!"
	);
	search_bar->addWidget(info_button);

	layout->addLayout(search_bar);

	// Toolbar: type filter + toggles
	auto* tool_bar = new QHBoxLayout;
	tool_bar->addWidget(new QLabel("Type:", this));
	type_combo = new QComboBox(this);
	type_combo->addItem("All", QVariant::fromValue(static_cast<int>(AssetType::All)));
	type_combo->addItem("Models", QVariant::fromValue(static_cast<int>(AssetType::Model)));
	type_combo->addItem("Textures", QVariant::fromValue(static_cast<int>(AssetType::Texture)));
	type_combo->addItem("Sounds", QVariant::fromValue(static_cast<int>(AssetType::Sound)));
	type_combo->addItem("Other", QVariant::fromValue(static_cast<int>(AssetType::Other)));
	tool_bar->addWidget(type_combo);

	unused_checkbox = new QCheckBox("Unused only", this);
	tool_bar->addWidget(unused_checkbox);

	group_checkbox = new QCheckBox("Group by folder", this);
	group_checkbox->setChecked(group_by_folder);
	tool_bar->addWidget(group_checkbox);

	deps_checkbox = new QCheckBox("Show dependencies", this);
	deps_checkbox->setChecked(show_dependencies);
	deps_checkbox->setToolTip("Show what each file is used by as expandable children.\nTurn off for a lighter tree when moving lots of files.");
	tool_bar->addWidget(deps_checkbox);

	auto* expand_button = new QPushButton("Expand all", this);
	expand_button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
	tool_bar->addWidget(expand_button);

	auto* collapse_button = new QPushButton("Collapse all", this);
	collapse_button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
	tool_bar->addWidget(collapse_button);

	tool_bar->addStretch();
	layout->addLayout(tool_bar);

	model = new QStandardItemModel(this);
	model->setHorizontalHeaderLabels({"File", "Type", "Size", "Usages"});

	filter_model = new AssetFilterModel(this);
	filter_model->setSourceModel(model);
	filter_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
	filter_model->setRecursiveFilteringEnabled(true);
	filter_model->setFilterKeyColumn(0);

	tree_view = new AssetTreeView(this);
	tree_view->setModel(filter_model);
	tree_view->setUniformRowHeights(true);
	tree_view->setContextMenuPolicy(Qt::CustomContextMenu);
	tree_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	tree_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	tree_view->setSortingEnabled(true);
	tree_view->sortByColumn(ColUsages, Qt::AscendingOrder);
	tree_view->header()->setStretchLastSection(false);
	// Drag files onto a folder to move them there (with reference rewrite).
	tree_view->setDragEnabled(true);
	tree_view->setAcceptDrops(true);
	tree_view->setDropIndicatorShown(true);
	tree_view->setDragDropMode(QAbstractItemView::DragDrop);
	tree_view->setDefaultDropAction(Qt::MoveAction);

	layout->addWidget(tree_view);

	status_label = new QLabel(this);
	layout->addWidget(status_label);

	connect(search_edit, &QLineEdit::textChanged, filter_model, &QSortFilterProxyModel::setFilterFixedString);
	connect(refresh_button, &QPushButton::clicked, this, &AssetManager::refresh);
	connect(type_combo, &QComboBox::currentIndexChanged, this, [this](int) {
		filter_model->set_type_filter(static_cast<AssetType>(type_combo->currentData().toInt()));
	});
	connect(unused_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
		filter_model->set_unused_only(checked);
	});
	connect(group_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
		group_by_folder = checked;
		refresh();
	});
	connect(deps_checkbox, &QCheckBox::toggled, this, [this](bool checked) {
		show_dependencies = checked;
		refresh();
	});
	connect(expand_button, &QPushButton::clicked, tree_view, &QTreeView::expandAll);
	connect(collapse_button, &QPushButton::clicked, tree_view, &QTreeView::collapseAll);

	// When objects are deleted in the Object Editor, remove their references from the tree.
	// We use rowsAboutToBeRemoved so the SLK index_to_row mapping is still intact.
	const auto make_removal_handler = [this](const slk::SLK& slk) {
		return [this, &slk](const QModelIndex&, const int first, const int last) {
			for (int i = first; i <= last; i++) {
				remove_object_references(slk.index_to_row.at(i));
			}
		};
	};
	connect(units_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(units_slk));
	connect(items_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(items_slk));
	connect(abilities_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(abilities_slk));
	connect(doodads_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(doodads_slk));
	connect(destructibles_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(destructibles_slk));
	connect(buff_table, &QAbstractItemModel::rowsAboutToBeRemoved, this, make_removal_handler(buff_slk));
	connect(tree_view, &QTreeView::customContextMenuRequested, this, &AssetManager::show_context_menu);
	connect(tree_view, &QTreeView::doubleClicked, this, &AssetManager::open_in_editor);
	connect(tree_view, &AssetTreeView::files_dropped, this, &AssetManager::handle_drop);

	refresh();
	show();
}

QStandardItem* AssetManager::sibling_column(QStandardItem* item, int column) const {
	if (!item) {
		return nullptr;
	}
	if (QStandardItem* parent = item->parent()) {
		return parent->child(item->row(), column);
	}
	return model->item(item->row(), column);
}

QList<QStandardItem*> AssetManager::make_file_row(const std::string& full_path, const QString& display, const std::unordered_set<std::string>& used_by) const {
	const bool is_unused = used_by.empty();

	long long size = -1;
	{
		std::error_code ec;
		const auto bytes = fs::file_size(map->filesystem_path / full_path, ec);
		if (!ec) {
			size = static_cast<long long>(bytes);
		}
	}

	auto* file_item = new QStandardItem(display);
	file_item->setEditable(false);
	file_item->setData(is_unused, IsUnusedRole);
	file_item->setData(true, IsFileRole);
	file_item->setData(static_cast<qlonglong>(size), SizeRole);
	file_item->setData(QString::fromStdString(full_path), PathRole);
	file_item->setIcon(get_file_icon(full_path));

	auto* type_item = new QStandardItem(type_label(full_path));
	type_item->setEditable(false);

	auto* size_item = new QStandardItem(human_size(size));
	size_item->setEditable(false);
	size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

	auto* count_item = new QStandardItem(QString::number(used_by.size()));
	count_item->setEditable(false);
	count_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

	if (is_unused) {
		constexpr QColor orange(200, 120, 0);
		file_item->setForeground(orange);
		type_item->setForeground(orange);
		size_item->setForeground(orange);
		count_item->setForeground(orange);
	}

	if (show_dependencies) {
		for (const auto& id : used_by) {
			const auto& info = resolve_used_by_id(id);

			auto* child_item = new QStandardItem(info.display_name);
			child_item->setEditable(false);
			if (!info.icon.isNull()) {
				child_item->setIcon(info.icon);
			}
			child_item->setData(QString::fromStdString(id), ObjectIdRole);
			child_item->setData(info.category, CategoryRole);

			file_item->appendRow(child_item);
		}
	}

	return {file_item, type_item, size_item, count_item};
}

QStandardItem* AssetManager::ensure_folder(const std::string& folder_path, std::unordered_map<std::string, QStandardItem*>& cache) const {
	if (folder_path.empty()) {
		return nullptr; // root
	}
	if (const auto it = cache.find(folder_path); it != cache.end()) {
		return it->second;
	}

	const auto slash = folder_path.find_last_of('/');
	const std::string parent_path = slash == std::string::npos ? "" : folder_path.substr(0, slash);
	const std::string name = slash == std::string::npos ? folder_path : folder_path.substr(slash + 1);

	auto* folder_item = new QStandardItem(QApplication::style()->standardIcon(QStyle::SP_DirIcon), QString::fromStdString(name));
	folder_item->setEditable(false);
	folder_item->setData(true, IsFolderRole);

	auto* type_item = new QStandardItem("folder");
	type_item->setEditable(false);
	auto* size_item = new QStandardItem;
	size_item->setEditable(false);
	size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
	auto* count_item = new QStandardItem;
	count_item->setEditable(false);
	count_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

	const QList<QStandardItem*> row = {folder_item, type_item, size_item, count_item};
	if (QStandardItem* parent = ensure_folder(parent_path, cache)) {
		parent->appendRow(row);
	} else {
		model->appendRow(row);
	}

	cache.emplace(folder_path, folder_item);
	return folder_item;
}

std::pair<long long, int> AssetManager::aggregate_folder(QStandardItem* folder) const {
	long long total = 0;
	int files = 0;
	for (int r = 0; r < folder->rowCount(); r++) {
		QStandardItem* child = folder->child(r, ColFile);
		if (!child) {
			continue;
		}
		if (child->data(IsFolderRole).toBool()) {
			const auto [bytes, count] = aggregate_folder(child);
			total += bytes;
			files += count;
		} else if (child->data(IsFileRole).toBool()) {
			const long long s = child->data(SizeRole).toLongLong();
			if (s > 0) {
				total += s;
			}
			files += 1;
		}
	}

	folder->setData(static_cast<qlonglong>(total), SizeRole);
	if (QStandardItem* size_item = sibling_column(folder, ColSize)) {
		size_item->setText(human_size(total));
	}
	if (QStandardItem* count_item = sibling_column(folder, ColUsages)) {
		count_item->setText(QString("%1 files").arg(files));
	}
	return {total, files};
}

void AssetManager::refresh() const {
	model->clear();
	model->setHorizontalHeaderLabels({"File", "Type", "Size", "Usages"});
	tree_view->header()->setSectionResizeMode(ColFile, QHeaderView::Stretch);
	tree_view->header()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
	tree_view->header()->setSectionResizeMode(ColSize, QHeaderView::ResizeToContents);
	tree_view->header()->setSectionResizeMode(ColUsages, QHeaderView::ResizeToContents);

	auto results = map->get_file_usage();

	// Sort: unused files first, then alphabetically within each group
	std::ranges::sort(results, [](const FileUsage& a, const FileUsage& b) {
		const bool a_unused = a.used_by.empty();
		const bool b_unused = b.used_by.empty();
		if (a_unused != b_unused) {
			return a_unused > b_unused;
		}
		return a.path < b.path;
	});

	if (group_by_folder) {
		std::unordered_map<std::string, QStandardItem*> folders;
		for (const auto& [path, used_by] : results) {
			const auto slash = path.find_last_of('/');
			const std::string folder_path = slash == std::string::npos ? "" : path.substr(0, slash);
			const std::string file_name = slash == std::string::npos ? path : path.substr(slash + 1);

			const QList<QStandardItem*> row = make_file_row(path, QString::fromStdString(file_name), used_by);
			if (QStandardItem* folder = ensure_folder(folder_path, folders)) {
				folder->appendRow(row);
			} else {
				model->appendRow(row);
			}
		}
		// Fill folder aggregates (top-level folders only; recurses internally).
		for (int r = 0; r < model->rowCount(); r++) {
			QStandardItem* item = model->item(r, ColFile);
			if (item && item->data(IsFolderRole).toBool()) {
				aggregate_folder(item);
			}
		}
		tree_view->expandToDepth(0);
	} else {
		for (const auto& [path, used_by] : results) {
			model->appendRow(make_file_row(path, QString::fromStdString(path), used_by));
		}
	}

	update_status();
}

void AssetManager::update_status() const {
	size_t total = 0;
	size_t unused = 0;

	std::function<void(QStandardItem*)> walk = [&](QStandardItem* item) {
		for (int r = 0; r < item->rowCount(); r++) {
			QStandardItem* child = item->child(r, ColFile);
			if (!child) {
				continue;
			}
			if (child->data(IsFileRole).toBool()) {
				total += 1;
				if (child->data(IsUnusedRole).toBool()) {
					unused += 1;
				}
			} else if (child->data(IsFolderRole).toBool()) {
				walk(child);
			}
		}
	};

	for (int r = 0; r < model->rowCount(); r++) {
		QStandardItem* item = model->item(r, ColFile);
		if (!item) {
			continue;
		}
		if (item->data(IsFileRole).toBool()) {
			total += 1;
			if (item->data(IsUnusedRole).toBool()) {
				unused += 1;
			}
		} else if (item->data(IsFolderRole).toBool()) {
			walk(item);
		}
	}

	status_label->setText(QString("%1 unused · %2 total").arg(unused).arg(total));
}

void AssetManager::remove_object_references(const std::string& id) {
	const QString qid = QString::fromStdString(id);
	constexpr QColor orange(200, 120, 0);

	std::function<void(QStandardItem*)> visit_file = [&](QStandardItem* file_item) {
		bool changed = false;
		for (int child_row = file_item->rowCount() - 1; child_row >= 0; child_row--) {
			const QStandardItem* const child = file_item->child(child_row);
			if (child && child->data(ObjectIdRole).toString() == qid) {
				file_item->removeRow(child_row);
				changed = true;
			}
		}
		if (!changed) {
			return;
		}
		const int new_count = file_item->rowCount();
		const bool is_now_unused = (new_count == 0);
		if (QStandardItem* count_item = sibling_column(file_item, ColUsages)) {
			count_item->setText(QString::number(new_count));
			count_item->setData(is_now_unused ? QVariant(QBrush(orange)) : QVariant(), Qt::ForegroundRole);
		}
		file_item->setData(is_now_unused, IsUnusedRole);
		file_item->setData(is_now_unused ? QVariant(QBrush(orange)) : QVariant(), Qt::ForegroundRole);
	};

	std::function<void(QStandardItem*)> walk = [&](QStandardItem* item) {
		for (int r = 0; r < item->rowCount(); r++) {
			QStandardItem* child = item->child(r, ColFile);
			if (!child) {
				continue;
			}
			if (child->data(IsFolderRole).toBool()) {
				walk(child);
			} else if (child->data(IsFileRole).toBool()) {
				visit_file(child);
			}
		}
	};

	for (int row = 0; row < model->rowCount(); row++) {
		QStandardItem* item = model->item(row, ColFile);
		if (!item) {
			continue;
		}
		if (item->data(IsFolderRole).toBool()) {
			walk(item);
		} else if (item->data(IsFileRole).toBool()) {
			visit_file(item);
		}
	}

	update_status();
}

void AssetManager::open_in_editor(const QModelIndex& proxy_index) const {
	if (!proxy_index.isValid()) {
		return;
	}
	const QModelIndex source_index = filter_model->mapToSource(proxy_index).siblingAtColumn(ColFile);
	QStandardItem* item = model->itemFromIndex(source_index);
	if (!item) {
		return;
	}
	// Only usage (child) items open an object; files/folders do nothing.
	if (item->data(IsFileRole).toBool() || item->data(IsFolderRole).toBool()) {
		return;
	}
	const int category = item->data(CategoryRole).toInt();
	if (category < 0) {
		return;
	}
	const QString id = item->data(ObjectIdRole).toString();
	bool created = false;
	auto* editor = window_handler.create_or_raise<ObjectEditor>(nullptr, created);
	editor->select_id(static_cast<ObjectEditor::Category>(category), id.toStdString());
}

void AssetManager::show_context_menu(const QPoint& pos) {
	const QModelIndex proxy_index = tree_view->indexAt(pos);
	if (!proxy_index.isValid()) {
		return;
	}

	const QModelIndex source_index = filter_model->mapToSource(proxy_index).siblingAtColumn(ColFile);
	QStandardItem* item = model->itemFromIndex(source_index);
	if (!item) {
		return;
	}

	QMenu menu;

	if (item->data(IsFolderRole).toBool()) {
		// Move every file in the folder, keeping the relative layout below it.
		const std::vector<std::string> paths = descendant_file_paths(item);
		if (!paths.empty()) {
			QAction* move_action =
				menu.addAction(QApplication::style()->standardIcon(QStyle::SP_DirIcon), QString("Move %1 files in folder…").arg(paths.size()));
			connect(move_action, &QAction::triggered, [this, paths]() {
				move_files(paths);
			});
		}
	} else if (item->data(IsFileRole).toBool()) {
		// Move / rename — operates on the whole selection if the clicked file is
		// part of it, otherwise just on the clicked file.
		std::vector<std::string> paths = selected_file_paths();
		const std::string clicked = item->data(PathRole).toString().toStdString();
		if (std::find(paths.begin(), paths.end(), clicked) == paths.end()) {
			paths = {clicked};
		}
		const QString move_label =
			paths.size() > 1 ? QString("Move %1 files…").arg(paths.size()) : QString("Move / rename file…");
		QAction* move_action = menu.addAction(QApplication::style()->standardIcon(QStyle::SP_DirIcon), move_label);
		connect(move_action, &QAction::triggered, [this, paths]() {
			move_files(paths);
		});

		if (item->data(IsUnusedRole).toBool()) {
			QAction* delete_action = menu.addAction(QApplication::style()->standardIcon(QStyle::SP_TrashIcon), "Delete file");
			connect(delete_action, &QAction::triggered, [this, item]() {
				const QString path_str = item->data(PathRole).toString();
				const int answer =
					QMessageBox::question(this, "Delete file", QString("Delete '%1'?").arg(path_str), QMessageBox::Yes | QMessageBox::No);
				if (answer != QMessageBox::Yes) {
					return;
				}
				const fs::path full_path = map->filesystem_path / path_str.toStdString();
				std::error_code ec;
				fs::remove(full_path, ec);
				if (ec) {
					QMessageBox::warning(
						this,
						"Delete failed",
						QString("Could not delete '%1':\n%2").arg(path_str, QString::fromStdString(ec.message()))
					);
					return;
				}
				if (QStandardItem* parent = item->parent()) {
					parent->removeRow(item->row());
				} else {
					model->removeRow(item->row());
				}
				update_status();
			});
		}
	} else {
		const int category = item->data(CategoryRole).toInt();
		if (category >= 0) {
			QAction* open_action = menu.addAction("Open in Object Editor");
			connect(open_action, &QAction::triggered, [this, proxy_index]() {
				open_in_editor(proxy_index);
			});
		}
	}

	if (!menu.isEmpty()) {
		menu.exec(tree_view->viewport()->mapToGlobal(pos));
	}
}

std::vector<std::string> AssetManager::selected_file_paths() const {
	std::vector<std::string> paths;
	const auto selection = tree_view->selectionModel()->selectedRows(ColFile);
	for (const QModelIndex& proxy_index : selection) {
		const QModelIndex source_index = filter_model->mapToSource(proxy_index);
		QStandardItem* item = model->itemFromIndex(source_index);
		if (item && item->data(IsFileRole).toBool()) {
			paths.push_back(item->data(PathRole).toString().toStdString());
		}
	}
	return paths;
}

std::vector<std::string> AssetManager::descendant_file_paths(QStandardItem* item) const {
	std::vector<std::string> paths;
	if (!item) {
		return paths;
	}
	if (item->data(IsFileRole).toBool()) {
		paths.push_back(item->data(PathRole).toString().toStdString());
		return paths;
	}
	for (int r = 0; r < item->rowCount(); r++) {
		QStandardItem* child = item->child(r, ColFile);
		if (!child) {
			continue;
		}
		auto sub = descendant_file_paths(child);
		paths.insert(paths.end(), sub.begin(), sub.end());
	}
	return paths;
}

void AssetManager::move_files(const std::vector<std::string>& paths) {
	if (paths.empty() || !map) {
		return;
	}

	// Build the from→to list.
	std::vector<std::pair<std::string, std::string>> moves;
	if (paths.size() == 1) {
		bool accepted = false;
		const QString current = QString::fromStdString(paths.front());
		const QString dest = QInputDialog::getText(
			this, "Move / rename file", "New path (relative to the map folder):", QLineEdit::Normal, current, &accepted
		);
		if (!accepted || dest.isEmpty() || dest == current) {
			return;
		}
		moves.emplace_back(paths.front(), dest.toStdString());
	} else {
		bool accepted = false;
		QString folder = QInputDialog::getText(
			this,
			"Move files",
			QString("Move %1 files into folder (relative to the map folder):").arg(paths.size()),
			QLineEdit::Normal,
			"",
			&accepted
		);
		if (!accepted) {
			return;
		}
		std::string folder_str = folder.toStdString();
		while (!folder_str.empty() && (folder_str.back() == '/' || folder_str.back() == '\\')) {
			folder_str.pop_back();
		}
		for (const auto& from : paths) {
			const std::string file_name = fs::path(from).filename().string();
			const std::string to = folder_str.empty() ? file_name : (folder_str + "/" + file_name);
			moves.emplace_back(from, to);
		}
	}

	apply_moves(moves);
}

void AssetManager::move_files_to_folder(const std::vector<std::string>& paths, const std::string& target_folder) {
	if (paths.empty() || !map) {
		return;
	}

	std::string folder_str = target_folder;
	while (!folder_str.empty() && (folder_str.back() == '/' || folder_str.back() == '\\')) {
		folder_str.pop_back();
	}

	std::vector<std::pair<std::string, std::string>> moves;
	for (const auto& from : paths) {
		const std::string file_name = fs::path(from).filename().string();
		const std::string to = folder_str.empty() ? file_name : (folder_str + "/" + file_name);
		if (to != from) {
			moves.emplace_back(from, to);
		}
	}
	if (moves.empty()) {
		return; // everything already in the target folder
	}
	apply_moves(moves);
}

void AssetManager::apply_moves(const std::vector<std::pair<std::string, std::string>>& moves) {
	if (moves.empty() || !map) {
		return;
	}

	// Dry-run preview so the user sees how many references will be rewritten.
	std::size_t total_refs = 0;
	bool any_error = false;
	QString details;
	for (const auto& [from, to] : moves) {
		const MoveReport rep = safe_move_file(from, to, true, map->info, map->sounds);
		if (!rep.ok) {
			any_error = true;
			details += QString("✗ %1 → %2: %3\n")
						   .arg(QString::fromStdString(from), QString::fromStdString(to), QString::fromStdString(rep.error));
		} else {
			total_refs += rep.reference_count();
			details += QString("%1 → %2  (%3 references)\n")
						   .arg(QString::fromStdString(from), QString::fromStdString(to))
						   .arg(rep.reference_count());
		}
	}

	// Only prompt when there is something to think about: an error, or
	// dependencies that would be rewritten. A clean move with no references
	// goes through silently.
	if (any_error || total_refs > 0) {
		const QString summary = QString("Move %1 file(s), rewriting %2 reference(s):\n\n%3\nProceed?")
									.arg(moves.size())
									.arg(total_refs)
									.arg(details);
		const auto answer = QMessageBox::question(
			this, "Dependencies found", summary, QMessageBox::Yes | QMessageBox::No, any_error ? QMessageBox::No : QMessageBox::Yes
		);
		if (answer != QMessageBox::Yes) {
			return;
		}
	}

	// Apply.
	QStringList failures;
	for (const auto& [from, to] : moves) {
		const MoveReport rep = safe_move_file(from, to, false, map->info, map->sounds);
		if (!rep.ok) {
			failures << QString("%1 → %2: %3")
							.arg(QString::fromStdString(from), QString::fromStdString(to), QString::fromStdString(rep.error));
		}
	}

	if (!failures.isEmpty()) {
		QMessageBox::warning(this, "Some moves failed", failures.join("\n"));
	}

	refresh();
}

std::string AssetManager::folder_path_for(QStandardItem* item) const {
	std::vector<std::string> parts;
	for (QStandardItem* cur = item; cur && cur->data(IsFolderRole).toBool(); cur = cur->parent()) {
		parts.push_back(cur->text().toStdString());
	}
	std::string path;
	for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
		if (!path.empty()) {
			path += '/';
		}
		path += *it;
	}
	return path;
}

void AssetManager::handle_drop(const QModelIndex& target_proxy_index) {
	std::vector<std::string> paths = selected_file_paths();
	if (paths.empty()) {
		return;
	}

	// Resolve the drop target into a destination folder (relative to the map root).
	std::string target_folder;
	if (target_proxy_index.isValid()) {
		const QModelIndex source_index = filter_model->mapToSource(target_proxy_index).siblingAtColumn(ColFile);
		QStandardItem* item = model->itemFromIndex(source_index);
		if (item) {
			if (item->data(IsFolderRole).toBool()) {
				target_folder = folder_path_for(item);
			} else if (item->data(IsFileRole).toBool()) {
				const std::string p = item->data(PathRole).toString().toStdString();
				const auto slash = p.find_last_of('/');
				target_folder = slash == std::string::npos ? "" : p.substr(0, slash);
			}
		}
	}
	// Invalid index (or a usage row) drops into the map root.

	move_files_to_folder(paths, target_folder);
}
