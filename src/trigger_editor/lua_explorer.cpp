#include "lua_explorer.h"

#include <QFileIconProvider>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QMap>
#include <filesystem>

namespace fs = std::filesystem;

LuaFileItem::LuaFileItem(Type type, const QString& name, const std::string& file_path, LuaFileItem* parent)
	: type(type), name(name), file_path(file_path), parent(parent) {
	if (parent) {
		parent->appendChild(this);
	}
}

LuaFileItem::~LuaFileItem() {
	qDeleteAll(children);
}

void LuaFileItem::appendChild(LuaFileItem* child) {
	children.append(child);
}

void LuaFileItem::removeChild(LuaFileItem* child) {
	children.removeOne(child);
}

LuaFileItem* LuaFileItem::child(int row) const {
	if (row < 0 || row >= children.size()) {
		return nullptr;
	}
	return children.at(row);
}

int LuaFileItem::childCount() const {
	return children.size();
}

int LuaFileItem::row() const {
	if (parent) {
		return parent->children.indexOf(const_cast<LuaFileItem*>(this));
	}
	return 0;
}

LuaFileItem* LuaFileItem::parentItem() const {
	return parent;
}

LuaFileModel::LuaFileModel(QObject* parent) : QAbstractItemModel(parent) {
	rootItem = new LuaFileItem(LuaFileItem::directory, "root");
}

LuaFileModel::~LuaFileModel() {
	delete rootItem;
}

QVariant LuaFileModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid()) {
		return QVariant();
	}

	LuaFileItem* item = static_cast<LuaFileItem*>(index.internalPointer());

	if (role == Qt::DisplayRole) {
		if (item->type == LuaFileItem::section && item->order > 0) {
			return QString("%1. %2").arg(item->order).arg(item->name);
		}
		return item->name;
	}

	if (role == Qt::DecorationRole) {
		switch (item->type) {
			case LuaFileItem::directory:
				return QFileIconProvider().icon(QFileIconProvider::Folder);
			case LuaFileItem::category:
				return QFileIconProvider().icon(QFileIconProvider::Folder);
			case LuaFileItem::lua_file:
			case LuaFileItem::section:
				return QFileIconProvider().icon(QFileIconProvider::File);
			case LuaFileItem::assembled_output:
				return QFileIconProvider().icon(QFileIconProvider::File);
		}
	}

	if (role == Qt::ForegroundRole && item->read_only) {
		return QColor(128, 128, 128);
	}

	if (role == Qt::FontRole && item->type == LuaFileItem::assembled_output) {
		QFont font;
		font.setBold(true);
		return font;
	}

	return QVariant();
}

Qt::ItemFlags LuaFileModel::flags(const QModelIndex& index) const {
	if (!index.isValid()) {
		return Qt::NoItemFlags;
	}
	return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant LuaFileModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
		return "Name";
	}
	return QVariant();
}

QModelIndex LuaFileModel::index(int row, int column, const QModelIndex& parent) const {
	if (!hasIndex(row, column, parent)) {
		return QModelIndex();
	}

	LuaFileItem* parentItem;
	if (!parent.isValid()) {
		parentItem = rootItem;
	} else {
		parentItem = static_cast<LuaFileItem*>(parent.internalPointer());
	}

	LuaFileItem* childItem = parentItem->child(row);
	if (childItem) {
		return createIndex(row, column, childItem);
	}
	return QModelIndex();
}

QModelIndex LuaFileModel::parent(const QModelIndex& index) const {
	if (!index.isValid()) {
		return QModelIndex();
	}

	LuaFileItem* childItem = static_cast<LuaFileItem*>(index.internalPointer());
	LuaFileItem* parentItem = childItem->parentItem();

	if (parentItem == rootItem || !parentItem) {
		return QModelIndex();
	}

	return createIndex(parentItem->row(), 0, parentItem);
}

int LuaFileModel::rowCount(const QModelIndex& parent) const {
	if (parent.column() > 0) {
		return 0;
	}

	LuaFileItem* parentItem;
	if (!parent.isValid()) {
		parentItem = rootItem;
	} else {
		parentItem = static_cast<LuaFileItem*>(parent.internalPointer());
	}

	return parentItem->childCount();
}

int LuaFileModel::columnCount(const QModelIndex& /*parent*/) const {
	return 1;
}

LuaFileItem* LuaFileModel::itemFromIndex(const QModelIndex& index) const {
	if (!index.isValid()) {
		return nullptr;
	}
	return static_cast<LuaFileItem*>(index.internalPointer());
}

void LuaFileModel::scanDirectory(LuaFileItem* parent, const std::string& dir_path, int depth) {
	if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
		return;
	}

	std::string dir_name = fs::path(dir_path).filename().string();

	if (depth == 0 && (dir_name == "war3mapImported" || dir_name == "Buildings" || dir_name == "Units" || dir_name == "Doodads")) {
		return;
	}

	std::vector<fs::path> lua_files;
	std::vector<fs::path> subdirs;

	for (const auto& entry : fs::directory_iterator(dir_path)) {
		if (entry.is_directory()) {
			subdirs.push_back(entry.path());
		} else if (entry.is_regular_file() && entry.path().extension() == ".lua") {
			lua_files.push_back(entry.path());
		}
	}

	for (const auto& file : lua_files) {
		QString name = QString::fromStdString(file.filename().string());
		bool is_war3map = (name == "war3map.lua");
		auto* item = new LuaFileItem(LuaFileItem::lua_file, name, file.string(), parent);
		if (is_war3map) {
			assembled_output_path = file.string();
		}
	}

	for (const auto& dir : subdirs) {
		QString name = QString::fromStdString(dir.filename().string());
		auto* item = new LuaFileItem(LuaFileItem::directory, name, dir.string(), parent);
		scanDirectory(item, dir.string(), depth + 1);
	}
}

void LuaFileModel::scanScriptDirs(LuaFileItem* parent, const std::string& map_path) {
	for (const auto& entry : fs::directory_iterator(map_path)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".lua") continue;
		QString name = QString::fromStdString(entry.path().filename().string());
		auto* item = new LuaFileItem(LuaFileItem::lua_file, name, entry.path().string(), parent);
		if (name == "war3map.lua") {
			assembled_output_path = entry.path().string();
		}
	}

	std::string lua_dir = map_path + "/_lua";
	if (fs::exists(lua_dir) && fs::is_directory(lua_dir)) {
		auto* lua_folder = new LuaFileItem(LuaFileItem::directory, "_lua", lua_dir, parent);
		scanDirectory(lua_folder, lua_dir, 1);
	}
}

void LuaFileModel::loadFromFilesystem(const std::string& map_path, bool script_dirs_only) {
	beginResetModel();
	manifest_mode = false;
	this->script_dirs_only = script_dirs_only;
	delete rootItem;
	rootItem = new LuaFileItem(LuaFileItem::directory, QString::fromStdString(map_path));

	if (script_dirs_only) {
		scanScriptDirs(rootItem, map_path);
	} else {
		scanDirectory(rootItem, map_path, 0);
	}

	endResetModel();
}

std::string LuaFileModel::findManifest(const std::string& map_path) {
	std::string candidates[] = {
		map_path + "/manifest.json",
		map_path + "/_lua/manifest.json",
	};

	for (const auto& path : candidates) {
		if (fs::exists(path)) {
			return path;
		}
	}
	return "";
}

void LuaFileModel::loadFromManifest(const std::string& map_path, const std::string& manifest_path) {
	QFile file(QString::fromStdString(manifest_path));
	if (!file.open(QIODevice::ReadOnly)) {
		loadFromFilesystem(map_path, script_dirs_only);
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	file.close();

	if (!doc.isObject()) {
		loadFromFilesystem(map_path, script_dirs_only);
		return;
	}

	beginResetModel();
	manifest_mode = true;
	delete rootItem;
	rootItem = new LuaFileItem(LuaFileItem::directory, QString::fromStdString(map_path));

	QJsonObject root_obj = doc.object();
	QString output_file = root_obj["output"].toString("war3map.lua");
	assembled_output_path = map_path + "/" + output_file.toStdString();

	QJsonArray sections = root_obj["sections"].toArray();
	if (sections.isEmpty()) {
		endResetModel();
		return;
	}

	QMap<QString, LuaFileItem*> category_items;
	int order = 1;

	for (const auto& section : sections) {
		QJsonObject sec = section.toObject();
		QString name = sec["name"].toString();
		QString file_rel = sec["file"].toString();
		QString category = sec["category"].toString();

		if (name.isEmpty() || file_rel.isEmpty()) {
			continue;
		}

		if (!category.isEmpty()) {
			if (!category_items.contains(category)) {
				auto* cat = new LuaFileItem(LuaFileItem::category, category, "", rootItem);
				category_items[category] = cat;
			}

			std::string absolute_path = map_path + "/" + file_rel.toStdString();
			auto* sec_item = new LuaFileItem(LuaFileItem::section, name, absolute_path, category_items[category]);
			sec_item->order = order;
		} else {
			std::string absolute_path = map_path + "/" + file_rel.toStdString();
			auto* sec_item = new LuaFileItem(LuaFileItem::section, name, absolute_path, rootItem);
			sec_item->order = order;
		}

		order++;
	}

	auto* assembled = new LuaFileItem(LuaFileItem::assembled_output, QString::fromStdString(output_file.toStdString()), assembled_output_path, rootItem);
	assembled->read_only = true;

	endResetModel();
}

LuaExplorer::LuaExplorer(QWidget* parent) : QTreeView(parent) {
	setHeaderHidden(true);
	setUniformRowHeights(true);
	setAnimated(true);
	setExpandsOnDoubleClick(false);

	QFileIconProvider icons;
	file_icon = icons.icon(QFileIconProvider::File);
	folder_icon = icons.icon(QFileIconProvider::Folder);
	assembled_icon = icons.icon(QFileIconProvider::File);
}

void LuaExplorer::loadForMap(const std::string& map_path, bool script_dirs_only) {
	auto* model = new LuaFileModel(this);

	std::string manifest = LuaFileModel::findManifest(map_path);
	if (!manifest.empty()) {
		model->loadFromManifest(map_path, manifest);
	} else {
		model->loadFromFilesystem(map_path, script_dirs_only);
	}

	setModel(model);
	expandToDepth(2);

	connect(this, &QTreeView::doubleClicked, [this, model](const QModelIndex& index) {
		LuaFileItem* item = model->itemFromIndex(index);
		if (!item) {
			return;
		}
		if (item->type == LuaFileItem::lua_file || item->type == LuaFileItem::section || item->type == LuaFileItem::assembled_output) {
			emit fileDoubleClicked(QString::fromStdString(item->file_path), item->read_only);
		}
	});
}
