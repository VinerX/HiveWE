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
#include <QIcon>
#include <QScrollBar>

import std;
import Globals;
import MapGlobal;
import QIconResource;
import ResourceManager;
import Hierarchy;

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

QIcon get_unit_icon(const std::string& id) {
	if (!units_slk.row_headers.contains(id)) return {};
	std::string art = units_slk.data<std::string>("art", id);
	if (art.empty()) return {};
	std::string path = art;
	if (!hierarchy.file_exists(path)) path = art + ".dds";
	if (!hierarchy.file_exists(path)) {
		std::string alt = art + ".blp";
		if (hierarchy.file_exists(alt)) path = alt;
		else return {};
	}
	auto res = resource_manager.load<QIconResource>(path);
	if (res) return res.value()->icon;
	return {};
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

QString edge_label(const std::string& relation) {
	if (relation == "trains")       return "Trains";
	if (relation == "builds")       return "Builds";
	if (relation == "upgrades to")   return "Upgr. to";
	if (relation == "researches")   return "Research";
	if (relation == "trained by")   return "Trnd by";
	if (relation == "built by")      return "Blt by";
	if (relation == "upgraded from") return "Upgr. from";
	return QString::fromStdString(relation);
}

} // namespace

TechTreeViewer::TechTreeViewer(QWidget* parent)
	: QMainWindow(parent) {
	setWindowTitle("Tech Tree Viewer");
	resize(1100, 700);

	QWidget* central = new QWidget;
	setCentralWidget(central);
	QVBoxLayout* main_layout = new QVBoxLayout(central);
	main_layout->setSpacing(4);
	main_layout->setContentsMargins(6, 6, 6, 6);

	// --- Top row: search + depth + go ---
	QHBoxLayout* top_row = new QHBoxLayout;
	top_row->setSpacing(6);
	search_ = new QLineEdit;
	search_->setPlaceholderText("Unit rawcode (e.g. hfoo)");
	search_->setMaxLength(4);
	search_->setMaximumWidth(100);
	depth_spin_ = new QSpinBox;
	depth_spin_->setRange(1, 10);
	depth_spin_->setValue(3);
	depth_spin_->setPrefix("Depth: ");
	depth_spin_->setMaximumWidth(80);
	go_btn_ = new QPushButton("Go");
	top_row->addWidget(search_);
	top_row->addWidget(depth_spin_);
	top_row->addWidget(go_btn_);
	top_row->addStretch();

	// relation filter checkboxes
	chk_trains_ = new QCheckBox("Trains");
	chk_trains_->setChecked(true);
	chk_builds_ = new QCheckBox("Builds");
	chk_builds_->setChecked(true);
	chk_upgrades_ = new QCheckBox("Upgr.To");
	chk_upgrades_->setChecked(true);
	chk_researches_ = new QCheckBox("Research");
	chk_researches_->setChecked(true);
	chk_trained_by_ = new QCheckBox("TrndBy");
	chk_trained_by_->setChecked(true);
	chk_built_by_ = new QCheckBox("BltBy");
	chk_built_by_->setChecked(true);
	chk_upgraded_from_ = new QCheckBox("UpgrFr");
	chk_upgraded_from_->setChecked(true);
	chk_recursive_ = new QCheckBox("Recursive");
	chk_recursive_->setChecked(true);

	auto conn = [this](QCheckBox* cb) { connect(cb, &QCheckBox::toggled, this, [this]() { rebuildTree(); }); };
	conn(chk_trains_); conn(chk_builds_); conn(chk_upgrades_); conn(chk_researches_);
	conn(chk_trained_by_); conn(chk_built_by_); conn(chk_upgraded_from_);
	conn(chk_recursive_);

	for (auto* cb : {chk_trains_, chk_builds_, chk_upgrades_, chk_researches_,
	                 chk_trained_by_, chk_built_by_, chk_upgraded_from_, chk_recursive_}) {
		top_row->addWidget(cb);
	}

	main_layout->addLayout(top_row);

	// --- info label ---
	info_label_ = new QLabel;
	info_label_->setWordWrap(true);
	main_layout->addWidget(info_label_);

	// --- splitter: tree | graph ---
	QSplitter* splitter = new QSplitter(Qt::Horizontal);

	tree_ = new QTreeWidget;
	tree_->setHeaderLabels({"Unit", "Relation"});
	tree_->setRootIsDecorated(true);
	tree_->setAlternatingRowColors(true);
	tree_->setIconSize(QSize(20, 20));
	tree_->header()->setStretchLastSection(true);
	tree_->setColumnWidth(0, 280);
	splitter->addWidget(tree_);

	graph_scene_ = new QGraphicsScene(this);
	graph_view_ = new QGraphicsView(graph_scene_);
	graph_view_->setRenderHint(QPainter::Antialiasing);
	graph_view_->setDragMode(QGraphicsView::ScrollHandDrag);
	graph_view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
	graph_view_->setResizeAnchor(QGraphicsView::AnchorUnderMouse);
	splitter->addWidget(graph_view_);

	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 5);
	main_layout->addWidget(splitter);

	// --- bottom: zoom buttons ---
	QHBoxLayout* zoom_row = new QHBoxLayout;
	QPushButton* zoom_in = new QPushButton("+");
	QPushButton* zoom_out = new QPushButton("-");
	QPushButton* zoom_fit = new QPushButton("Fit");
	zoom_in->setFixedWidth(28);
	zoom_out->setFixedWidth(28);
	zoom_fit->setFixedWidth(36);
	connect(zoom_in, &QPushButton::clicked, this, [this]() { zoomGraph(1.25); });
	connect(zoom_out, &QPushButton::clicked, this, [this]() { zoomGraph(0.8); });
	connect(zoom_fit, &QPushButton::clicked, this, [this]() {
		graph_view_->fitInView(graph_scene_->sceneRect(), Qt::KeepAspectRatio);
	});

	QLabel* legend = new QLabel("Scroll = zoom  |  Drag = pan  |  Dbl-click tree node = re-root");
	legend->setStyleSheet("color: gray; font-size: 10px;");
	zoom_row->addWidget(legend);
	zoom_row->addStretch();
	zoom_row->addWidget(zoom_out);
	zoom_row->addWidget(zoom_in);
	zoom_row->addWidget(zoom_fit);
	main_layout->addLayout(zoom_row);

	// tree double-click → re-root
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

void TechTreeViewer::zoomGraph(double factor) {
	graph_view_->scale(factor, factor);
}

void TechTreeViewer::setUnit(const std::string& rawcode) {
	current_id_ = QString::fromStdString(rawcode);
	search_->setText(current_id_);
	rebuildTree();
}

std::vector<std::pair<std::string, std::string>> TechTreeViewer::getChildren(const std::string& id) {
	std::vector<std::pair<std::string, std::string>> children;

	std::set<std::string> enabled;
	if (chk_trains_->isChecked())         enabled.insert("trains");
	if (chk_builds_->isChecked())          enabled.insert("builds");
	if (chk_upgrades_->isChecked())        enabled.insert("upgrades to");
	if (chk_researches_->isChecked())      enabled.insert("researches");
	if (chk_trained_by_->isChecked())      enabled.insert("trained by");
	if (chk_built_by_->isChecked())        enabled.insert("built by");
	if (chk_upgraded_from_->isChecked())   enabled.insert("upgraded from");

	auto add = [&](const std::string& rel, const std::string& child_id) {
		if (enabled.contains(rel) && units_slk.row_headers.contains(child_id))
			children.push_back({child_id, rel});
	};

	for (const auto& t : split_rcx(units_slk.data<std::string>("trains", id)))   add("trains", t);
	for (const auto& b : split_rcx(units_slk.data<std::string>("builds", id)))   add("builds", b);
	for (const char* f : {"upgrade", "upgrades", "revive"})
		for (const auto& u : split_rcx(units_slk.data<std::string>(f, id)))      add("upgrades to", u);
	for (const auto& r : split_rcx(units_slk.data<std::string>("researches", id))) add("researches", r);

	// Reverse lookups (cached once)
	static std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> reverse_cache;
	static bool reverse_built = false;
	if (!reverse_built) {
		for (const auto& [row_id, idx] : units_slk.row_headers) {
			for (const auto& t : split_rcx(units_slk.data<std::string>("trains", row_id)))
				reverse_cache[t].push_back({row_id, "trained by"});
			for (const auto& b : split_rcx(units_slk.data<std::string>("builds", row_id)))
				reverse_cache[b].push_back({row_id, "built by"});
			for (const char* f : {"upgrade", "upgrades", "revive"})
				for (const auto& u : split_rcx(units_slk.data<std::string>(f, row_id)))
					reverse_cache[u].push_back({row_id, "upgraded from"});
		}
		reverse_built = true;
	}
	if (auto it = reverse_cache.find(id); it != reverse_cache.end())
		for (const auto& [rid, rel] : it->second)
			if (enabled.contains(rel))
				children.push_back({rid, rel});

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
	const std::string type_str = is_bldg ? "building" : "unit";
	info_label_->setText(QString("Tracing: %1  %2  [%3]")
		.arg(current_id_, QString::fromStdString(root_name), QString::fromStdString(type_str)));

	const int max_depth = depth_spin_->value();
	const bool recursive = chk_recursive_->isChecked();
	std::unordered_set<std::string> visited;

	// ---- tree ----
	std::function<void(QTreeWidgetItem*, const std::string&, int)> build_tree;
	build_tree = [&](QTreeWidgetItem* parent_item, const std::string& id, int depth) {
		if (depth >= max_depth) return;
		if (!visited.insert(id).second) return;

		for (const auto& [child_id, relation] : getChildren(id)) {
			const std::string name = resolve_name(child_id);
			QString label = QString::fromStdString(child_id + "  " + name);
			QTreeWidgetItem* item = new QTreeWidgetItem(parent_item,
				{label, edge_label(relation)});
			QIcon icon = get_unit_icon(child_id);
			if (!icon.isNull()) item->setIcon(0, icon);
			QFont f = item->font(0);
			f.setBold(true);
			item->setFont(0, f);

			if (recursive && depth + 1 < max_depth)
				build_tree(item, child_id, depth + 1);
		}
	};

	QString root_label = QString::fromStdString(root_id + "  " + root_name);
	QTreeWidgetItem* root_item = new QTreeWidgetItem(tree_, {root_label, "ROOT"});
	QIcon root_icon = get_unit_icon(root_id);
	if (!root_icon.isNull()) root_item->setIcon(0, root_icon);
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
	const bool recursive = chk_recursive_->isChecked();

	std::unordered_set<std::string> visited;
	std::vector<NodeInfo> nodes;
	std::vector<std::tuple<std::string, std::string, std::string>> edges;
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

		if (recursive) {
			for (const auto& [child_id, rel] : ni.children) {
				edges.push_back({id, child_id, rel});
				collect(child_id, depth + 1);
			}
		} else {
			for (const auto& [child_id, rel] : ni.children)
				edges.push_back({id, child_id, rel});
		}
	};

	collect(root_id, 0);

	// Layout: group nodes by depth, spread vertically
	std::unordered_map<int, std::vector<NodeInfo*>> depth_groups;
	for (auto& n : nodes)
		depth_groups[depth_map[n.id]].push_back(&n);

	const float col_width = 250;
	const float row_height = 90;
	const float box_w = 210;
	const float box_h = 62;

	for (int d = 0; d < max_depth && depth_groups.count(d); ++d) {
		auto& group = depth_groups[d];
		const float total_h = static_cast<float>(group.size()) * row_height;
		const float start_y = -total_h / 2.0f;

		for (size_t i = 0; i < group.size(); ++i) {
			auto* n = group[i];
			const float x = static_cast<float>(d) * col_width;
			const float y = start_y + static_cast<float>(i) * row_height;

			QColor color = node_color(n->is_building, n->is_worker);
			QGraphicsRectItem* rect = graph_scene_->addRect(QRectF(x, y, box_w, box_h),
				QPen(color.darker(130), 2), QBrush(color.lighter(130)));
			rect->setFlag(QGraphicsItem::ItemIsSelectable);

			QString label = QString::fromStdString(n->id + "\n" + n->name);
			QGraphicsTextItem* txt = graph_scene_->addText(label);
			txt->setPos(x + 6, y + 4);
			txt->setDefaultTextColor(Qt::black);
			QFont f = txt->font();
			f.setPointSize(8);
			if (n->name.size() > 20) f.setPointSize(7);
			txt->setFont(f);

			QIcon icon = get_unit_icon(n->id);
			if (!icon.isNull()) {
				QPixmap pm = icon.pixmap(24, 24);
				graph_scene_->addPixmap(pm)->setPos(x + box_w - 30, y + 10);
			}
		}
	}

	for (const auto& [from, to, rel] : edges) {
		if (!depth_map.contains(from) || !depth_map.contains(to)) continue;

		const auto& from_group = depth_groups[depth_map[from]];
		const auto& to_group = depth_groups[depth_map[to]];
		const float total_h_from = static_cast<float>(from_group.size()) * row_height;
		const float total_h_to = static_cast<float>(to_group.size()) * row_height;

		float from_x = 0, from_y = 0, to_x = 0, to_y = 0;
		for (size_t i = 0; i < from_group.size(); ++i)
			if (from_group[i]->id == from) {
				from_x = static_cast<float>(depth_map[from]) * col_width + box_w;
				from_y = -total_h_from / 2.0f + static_cast<float>(i) * row_height + box_h / 2.0f;
				break;
			}
		for (size_t i = 0; i < to_group.size(); ++i)
			if (to_group[i]->id == to) {
				to_x = static_cast<float>(depth_map[to]) * col_width;
				to_y = -total_h_to / 2.0f + static_cast<float>(i) * row_height + box_h / 2.0f;
				break;
			}

		QColor ec = edge_color(rel);
		QPen pen(ec, 1.5);
		if (rel == "trains" || rel == "builds") pen.setWidth(2);
		graph_scene_->addLine(QLineF(from_x, from_y, to_x, to_y), pen);

		// Arrowhead
		const float alen = 8;
		const float angle = std::atan2(to_y - from_y, to_x - from_x);
		const float a1 = angle + 0.4f;
		const float a2 = angle - 0.4f;
		graph_scene_->addLine(QLineF(to_x, to_y,
			to_x - alen * std::cos(a1), to_y - alen * std::sin(a1)), pen);
		graph_scene_->addLine(QLineF(to_x, to_y,
			to_x - alen * std::cos(a2), to_y - alen * std::sin(a2)), pen);
	}

	graph_scene_->setSceneRect(graph_scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30));
	graph_view_->fitInView(graph_scene_->sceneRect(), Qt::KeepAspectRatio);
}
