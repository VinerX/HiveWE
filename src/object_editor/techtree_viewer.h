#pragma once

#include <QMainWindow>
#include <QCloseEvent>
#include <QSplitter>
#include <QTreeWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <set>

class TechTreeViewer : public QMainWindow {
	Q_OBJECT

public:
	explicit TechTreeViewer(QWidget* parent = nullptr);

	void setUnit(const std::string& rawcode);

protected:
	void closeEvent(QCloseEvent* event) override { hide(); event->ignore(); }
	bool eventFilter(QObject* obj, QEvent* event) override;

private:
	void rebuildTree();
	void rebuildGraph();
	void zoomGraph(double factor);

	struct NodeInfo {
		std::string id;
		std::string name;
		bool is_building = false;
		bool is_worker = false;
		std::vector<std::pair<std::string, std::string>> children;
	};

	std::vector<std::pair<std::string, std::string>> getChildren(const std::string& id);

	QLineEdit* search_ = nullptr;
	QSpinBox* depth_spin_ = nullptr;
	QPushButton* go_btn_ = nullptr;
	QLabel* info_label_ = nullptr;
	QTreeWidget* tree_ = nullptr;
	QGraphicsView* graph_view_ = nullptr;
	QGraphicsScene* graph_scene_ = nullptr;

	QCheckBox* chk_trains_ = nullptr;
	QCheckBox* chk_builds_ = nullptr;
	QCheckBox* chk_upgrades_ = nullptr;
	QCheckBox* chk_researches_ = nullptr;
	QCheckBox* chk_trained_by_ = nullptr;
	QCheckBox* chk_built_by_ = nullptr;
	QCheckBox* chk_upgraded_from_ = nullptr;
	QCheckBox* chk_recursive_ = nullptr;

	QString current_id_;
};
