#include "techtree_viewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsLineItem>
#include <QWheelEvent>

import std;
import Globals;
import MapGlobal;

namespace {

std::vector<std::string> split_rcx(std::string_view value) {
	std::vector<std::string> result;
	std::string current;
	for (char c : value) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'') {
			current.push_back(c);
		} else {
			if (current.size() == 4) result.push_back(current);
			current.clear();
		}
	}
	if (current.size() == 4) result.push_back(current);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

std::string resolve_name(const std::string& id) {
	if (!units_slk.row_headers.contains(id)) return {};
	std::string name = units_slk.data<std::string>("name", id);
	if (name.starts_with("TRIGSTR") && map && !map->trigger_strings.string(name).empty()) {
		return std::string(map->trigger_strings.string(name));
	}
	if (name.starts_with("TRIGSTR")) return id;
	return name;
}

QColor node_color(bool is_building, bool is_worker) {
	if (is_building) return QColor(255, 180, 100);
	if (is_worker)   return QColor(140, 220, 140);
	return QColor(120, 160, 255);
}

QColor edge_color(const std::string& relation) {
	if (relation == "trains")       return QColor(220, 180, 60);
	if (relation == "builds")       return QColor(80, 180, 80);
	if (relation == "upgrades to")   return QColor(100, 160, 220);
	if (relation == "researches")   return QColor(200, 120, 200);
	if (relation == "trained by")   return QColor(200, 200, 120);
	if (relation == "built by")      return QColor(100, 200, 100);
	if (relation == "upgraded from") return QColor(160, 200, 240);
	return QColor(160, 160, 160);
}

} // namespace

TechTreeViewer::TechTreeViewer(QWidget* parent)
	: QMainWindow(parent) {
	setWindowTitle("Tech Tree Viewer");
	resize(1000, 680);

	QWidget* central = new QWidget;
	setCentralWidget(central);
	QVBoxLayout* main_layout = new QVBoxLayout(central);

	QHBoxLayout* top_row = new QHBoxLayout;
	search_ = new QLineEdit;
	search_->setPlaceholderText("Enter unit rawcode (e.g. hfoo)");
	search_->setMaxLength(4);
	search_->setMaximumWidth(100);
	QLabel* depth_label = new QLabel("Depth:");
	depth_spin_ = new QSpinBox;
	depth_spin_->setRange(1, 10);
	depth_spin_->setValue(3);
	go_btn_ = new QPushButton("Go");
	top_row->addWidget(search_);
	top_row->addWidget(depth_label);
	top_row->addWidget(depth_spin_);
	top_row->addWidget(go_btn_);
	top_row->addStretch();
	main_layout->addLayout(top_row);

	info_label_ = new QLabel;
	main_layout->addWidget(info_label_);

	QSplitter* splitter = new QSplitter(Qt::Horizontal);

	tree_ = new QTreeWidget;
	tree_->setHeaderLabels({"Unit", "Relation"});
	tree_->setRootIsDecorated(true);
	tree_->setAlternatingRowColors(true);
	tree_->header()->setStretchLastSection(true);
	splitter->addWidget(tree_);

	graph_scene_ = new QGraphicsScene(this);
	graph_view_ = new QGraphicsView(graph_scene_);
	graph_view_->setRenderHint(QPainter::Antialiasing);
	graph_view_->setDragMode(QGraphicsView::ScrollHandDrag);
	graph_view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
	graph_view_->setResizeAnchor(QGraphicsView::AnchorUnderMouse);
	splitter->addWidget(graph_view_);

	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 2);
	main_layout->addWidget(splitter);

	QLabel* legend = new QLabel("Double-click tree node to re-root. Scroll to zoom graph, drag to pan.");
	legend->setWordWrap(true);
	main_layout->addWidget(legend);

	connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
		QString id = item->text(0).section(' ', 0, 0);
		if (id.length() == 4) {
			current_id_ = id;
			search_->setText(current_id_);
			rebuildTree();
		}
	});

	connect(go_btn_, &QPushButton::clicked, this, [this]() {
		current_id_ = search_->text().trimmed();
		rebuildTree();
	});
	connect(search_, &QLineEdit::returnPressed, go_btn_, &QPushButton::click);
	show();
}

void TechTreeViewer::setUnit(const std::string& rawcode) {
	current_id_ = QString::fromStdString(rawcode);
	search_->setText(current_id_);
	rebuildTree();
}

std::vector<std::pair<std::string, std::string>> TechTreeViewer::getChildren(const std::string& id) {
	std::vector<std::pair<std::string, std::string>> children;

	for (const auto& t : split_rcx(units_slk.data<std::string>("trains", id)))
		if (units_slk.row_headers.contains(t))
			children.push_back({t, "trains"});
	for (const auto& b : split_rcx(units_slk.data<std::string>("builds", id)))
		if (units_slk.row_headers.contains(b))
			children.push_back({b, "builds"});
	for (const char* f : {"upgrade", "upgrades", "revive"}) {
		for (const auto& u : split_rcx(units_slk.data<std::string>(f, id)))
			if (units_slk.row_headers.contains(u))
				children.push_back({u, "upgrades to"});
	}
	for (const auto& r : split_rcx(units_slk.data<std::string>("researches", id)))
		children.push_back({r, "researches"});

	// Reverse lookups
	static std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> reverse_cache;
	static bool reverse_built = false;
	if (!reverse_built) {
		for (const auto& [row_id, idx] : units_slk.row_headers) {
			for (const auto& t : split_rcx(units_slk.data<std::string>("trains", row_id)))
				reverse_cache[t].push_back({row_id, "trained by"});
			for (const auto& b : split_rcx(units_slk.data<std::string>("builds", row_id)))
				reverse_cache[b].push_back({row_id, "built by"});
			for (const char* f : {"upgrade", "upgrades", "revive"}) {
				for (const auto& u : split_rcx(units_slk.data<std::string>(f, row_id)))
					reverse_cache[u].push_back({row_id, "upgraded from"});
			}
		}
		reverse_built = true;
	}
	if (auto it = reverse_cache.find(id); it != reverse_cache.end())
		children.insert(children.end(), it->second.begin(), it->second.end());

	std::sort(children.begin(), children.end(),
		[](const auto& a, const auto& b) {
			if (a.second != b.second) return a.second < b.second;
			return resolve_name(a.first) < resolve_name(b.first);
		});
	children.erase(std::unique(children.begin(), children.end()), children.end());
	return children;
}

void TechTreeViewer::rebuildTree() {
	tree_->clear();
	graph_scene_->clear();

	if (current_id_.isEmpty()) return;

	const std::string root_id = current_id_.toStdString();
	if (!units_slk.row_headers.contains(root_id)) {
		info_label_->setText("Unit not found: " + current_id_);
		return;
	}

	const std::string root_name = resolve_name(root_id);
	const bool is_bldg = units_slk.data<std::string>("isbldg", root_id) == "1";
	const std::string type_str = is_bldg ? " [building]" : " [unit]";
	info_label_->setText(QString("Tracing: %1 (%2)%3")
		.arg(current_id_, QString::fromStdString(root_name), QString::fromStdString(type_str)));

	const int max_depth = depth_spin_->value();
	std::unordered_set<std::string> visited;

	// ---- tree ----
	std::function<void(QTreeWidgetItem*, const std::string&, int)> build_tree;
	build_tree = [&](QTreeWidgetItem* parent_item, const std::string& id, int depth) {
		if (depth >= max_depth) return;
		if (!visited.insert(id).second) return;

		for (const auto& [child_id, relation] : getChildren(id)) {
			const std::string name = resolve_name(child_id);
			QString label = QString("%1 (%2)").arg(QString::fromStdString(child_id),
				QString::fromStdString(name));
			QTreeWidgetItem* item = new QTreeWidgetItem(parent_item, {label, QString::fromStdString(relation)});
			QFont f = item->font(0);
			f.setBold(true);
			item->setFont(0, f);
			if (depth + 1 < max_depth)
				build_tree(item, child_id, depth + 1);
		}
	};

	QString root_label = QString("%1 (%2)").arg(current_id_, QString::fromStdString(root_name));
	QTreeWidgetItem* root_item = new QTreeWidgetItem(tree_, {root_label, "ROOT"});
	QFont rf = root_item->font(0);
	rf.setBold(true);
	rf.setPointSize(rf.pointSize() + 1);
	root_item->setFont(0, rf);
	tree_->addTopLevelItem(root_item);
	build_tree(root_item, root_id, 0);
	tree_->expandAll();

	// ---- graph ----
	rebuildGraph();
}

void TechTreeViewer::rebuildGraph() {
	const std::string root_id = current_id_.toStdString();
	const int max_depth = depth_spin_->value();

	std::unordered_set<std::string> visited;
	std::vector<NodeInfo> nodes;
	std::vector<std::tuple<std::string, std::string, std::string>> edges; // from, to, relation
	std::unordered_map<std::string, int> depth_map;

	std::function<void(const std::string&, int)> collect;
	collect = [&](const std::string& id, int depth) {
		if (depth >= max_depth) return;
		if (!visited.insert(id).second) return;

		NodeInfo ni;
		ni.id = id;
		ni.name = resolve_name(id);
		ni.is_building = units_slk.data<std::string>("isbldg", id) == "1";
		ni.is_worker = !split_rcx(units_slk.data<std::string>("builds", id)).empty();
		ni.children = getChildren(id);
		nodes.push_back(ni);
		depth_map[id] = depth;

		for (const auto& [child_id, rel] : ni.children) {
			edges.push_back({id, child_id, rel});
			collect(child_id, depth + 1);
		}
	};

	collect(root_id, 0);

	// Layout: group nodes by depth, spread vertically
	std::unordered_map<int, std::vector<NodeInfo*>> depth_groups;
	for (auto& n : nodes) {
		depth_groups[depth_map[n.id]].push_back(&n);
	}

	const float col_width = 220;
	const float row_height = 70;
	const float box_w = 180;
	const float box_h = 48;

	for (int d = 0; d < max_depth && depth_groups.count(d); ++d) {
		auto& group = depth_groups[d];
		const float total_height = static_cast<float>(group.size()) * row_height;
		const float start_y = -total_height / 2.0f;

		for (size_t i = 0; i < group.size(); ++i) {
			auto* n = group[i];
			const float x = static_cast<float>(d) * col_width;
			const float y = start_y + static_cast<float>(i) * row_height;

			QColor color = node_color(n->is_building, n->is_worker);
			QGraphicsRectItem* rect = graph_scene_->addRect(QRectF(x, y, box_w, box_h),
				QPen(color.darker(120), 2), QBrush(color.lighter(130)));
			rect->setFlag(QGraphicsItem::ItemIsSelectable);

			QGraphicsTextItem* label = graph_scene_->addText(
				QString("%1\n%2").arg(QString::fromStdString(n->id),
					QString::fromStdString(n->name)));
			label->setPos(x + 6, y + 4);
			label->setDefaultTextColor(Qt::black);
			QFont f = label->font();
			f.setPointSize(8);
			if (n->name.size() > 20) f.setPointSize(7);
			label->setFont(f);
		}
	}

	for (const auto& [from, to, rel] : edges) {
		if (!depth_map.contains(from) || !depth_map.contains(to)) continue;
		const float from_x = static_cast<float>(depth_map[from]) * col_width + box_w;
		const float to_x = static_cast<float>(depth_map[to]) * col_width;
		const auto& from_group = depth_groups[depth_map[from]];
		const auto& to_group = depth_groups[depth_map[to]];
		const float total_h_from = static_cast<float>(from_group.size()) * row_height;
		const float total_h_to = static_cast<float>(to_group.size()) * row_height;

		float from_y = 0, to_y = 0;
		for (size_t i = 0; i < from_group.size(); ++i)
			if (from_group[i]->id == from) {
				from_y = -total_h_from / 2.0f + static_cast<float>(i) * row_height + box_h / 2.0f;
				break;
			}
		for (size_t i = 0; i < to_group.size(); ++i)
			if (to_group[i]->id == to) {
				to_y = -total_h_to / 2.0f + static_cast<float>(i) * row_height + box_h / 2.0f;
				break;
			}

		QColor ec = edge_color(rel);
		QPen pen(ec, 1.5);
		if (rel == "trains" || rel == "builds") pen.setWidth(2);
		QGraphicsLineItem* line = graph_scene_->addLine(QLineF(from_x, from_y, to_x, to_y), pen);

		// Arrowhead
		const float arrow_len = 8;
		const float angle = std::atan2(to_y - from_y, to_x - from_x);
		const float a1 = angle + 0.4f;
		const float a2 = angle - 0.4f;
		graph_scene_->addLine(QLineF(to_x, to_y,
			to_x - arrow_len * std::cos(a1), to_y - arrow_len * std::sin(a1)), pen);
		graph_scene_->addLine(QLineF(to_x, to_y,
			to_x - arrow_len * std::cos(a2), to_y - arrow_len * std::sin(a2)), pen);
	}

	graph_scene_->setSceneRect(graph_scene_->itemsBoundingRect().adjusted(-20, -20, 20, 20));
	graph_view_->fitInView(graph_scene_->sceneRect(), Qt::KeepAspectRatio);

	// Zoom with mouse wheel
	graph_view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
	graph_view_->setRenderHint(QPainter::Antialiasing);
}
