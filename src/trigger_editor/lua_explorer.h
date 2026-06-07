#pragma once

#include <QTreeView>
#include <QAbstractItemModel>
#include <QIcon>
#include <QMenu>
#include <QAction>
#include <QVector>
#include <string>

class LuaFileItem {
public:
	enum Type { directory, lua_file, category, section, assembled_output };

	LuaFileItem(Type type, const QString& name, const std::string& file_path = "", LuaFileItem* parent = nullptr);
	~LuaFileItem();

	void appendChild(LuaFileItem* child);
	void removeChild(LuaFileItem* child);
	LuaFileItem* child(int row) const;
	int childCount() const;
	int row() const;
	LuaFileItem* parentItem() const;

	Type type;
	QString name;
	std::string file_path;
	int order = 0;
	bool read_only = false;

private:
	LuaFileItem* parent = nullptr;
	QVector<LuaFileItem*> children;
};

class LuaFileModel : public QAbstractItemModel {
	Q_OBJECT

public:
	explicit LuaFileModel(QObject* parent = nullptr);
	~LuaFileModel();

	QVariant data(const QModelIndex& index, int role) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex& index) const override;
	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;

	void loadFromFilesystem(const std::string& map_path, bool script_dirs_only = true);
	void loadFromManifest(const std::string& map_path, const std::string& manifest_path);
	bool isManifestMode() const { return manifest_mode; }
	bool isScriptDirsOnly() const { return script_dirs_only; }

	static std::string findManifest(const std::string& map_path);
	LuaFileItem* itemFromIndex(const QModelIndex& index) const;

	std::string assembled_output_path;

private:
	LuaFileItem* rootItem;
	bool manifest_mode = false;
	bool script_dirs_only = true;

	void scanDirectory(LuaFileItem* parent, const std::string& dir_path, int depth);
	void scanScriptDirs(LuaFileItem* parent, const std::string& map_path);
};

class LuaExplorer : public QTreeView {
	Q_OBJECT

public:
	explicit LuaExplorer(QWidget* parent = nullptr);

	void loadForMap(const std::string& map_path, bool script_dirs_only = true);

	QIcon file_icon;
	QIcon folder_icon;
	QIcon assembled_icon;

signals:
	void fileDoubleClicked(const QString& file_path, bool read_only);
	void fileRefreshRequested();
};
