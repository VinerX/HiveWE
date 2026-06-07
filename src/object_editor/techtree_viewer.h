#pragma once

#include <QMainWindow>
#include <QTreeWidget>
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
	void addNode(QTreeWidgetItem* parent, const std::string& id, int depth,
	            std::unordered_set<std::string>& visited,
	            std::unordered_map<std::string, QTreeWidgetItem*>& node_map);

	QLineEdit* search_ = nullptr;
	QSpinBox* depth_spin_ = nullptr;
	QPushButton* go_btn_ = nullptr;
	QLabel* info_label_ = nullptr;
	QTreeWidget* tree_ = nullptr;
	QString current_id_;
};
