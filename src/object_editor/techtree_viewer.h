#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QTreeWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

class TechTreeViewer : public QMainWindow {
	Q_OBJECT

public:
	explicit TechTreeViewer(QWidget* parent = nullptr);

	void setUnit(const std::string& rawcode);

private:
	void rebuildTree();
	void rebuildGraph();

	struct NodeInfo {
		std::string id;
		std::string name;
		bool is_building = false;
		bool is_worker = false;
		std::vector<std::pair<std::string, std::string>> children; // {id, relation}
	};

	std::vector<std::pair<std::string, std::string>> getChildren(const std::string& id);

	QLineEdit* search_ = nullptr;
	QSpinBox* depth_spin_ = nullptr;
	QPushButton* go_btn_ = nullptr;
	QLabel* info_label_ = nullptr;
	QTreeWidget* tree_ = nullptr;
	QGraphicsView* graph_view_ = nullptr;
	QGraphicsScene* graph_scene_ = nullptr;
	QString current_id_;
};
