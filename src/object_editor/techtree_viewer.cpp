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
#include <QEvent>
#include <QMenu>
#include <QAction>
#include <QMouseEvent>
#include <QToolTip>

#include "object_editor.h"

import std;
import Globals;
import MapGlobal;
import QIconResource;
import ResourceManager;
import Hierarchy;
import WindowHandler;

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

QColor node_color(bool is_building, bool is_worker, bool is_repeat) {
	if (is_repeat)           return QColor(210, 210, 210);
	if (is_building)         return QColor(255, 180, 100);
	if (is_worker)           return QColor(140, 220, 140);
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

void store_id(QGraphicsItem* item, const std::string& id) { item->setData(0, QString::fromStdString(id)); }
std::string get_id(QGraphicsItem* item) { return item->data(0).toString().toStdString(); }

} // namespace

TechTreeViewer::TechTreeViewer(QWidget* parent)
	: QMainWindow(parent) {
	setWindowTitle("Tech Tree Viewer");
	resize(1100, 700);

	QWidget* central = new QWidget;
	setCentralWidget(central);
	QVBoxLayout* main_layout = new QVBoxLayout(central);
	main_layout->setSpacing(2);
	main_layout->setContentsMargins(4, 4, 4, 4);

	// Top row
	QHBoxLayout* top_row = new QHBoxLayout;
	top_row->setSpacing(4);
	search_ = new QLineEdit;
	search_->setPlaceholderText("e.g. hfoo");
	search_->setMaxLength(4);
	search_->setMaximumWidth(80);
	depth_spin_ = new QSpinBox;
	depth_spin_->setRange(1, 10); depth_spin_->setValue(3);
	depth_spin_->setPrefix("Depth:"); depth_spin_->setMaximumWidth(70);
	go_btn_ = new QPushButton("Go");
	top_row->addWidget(search_);
	top_row->addWidget(depth_spin_);
	top_row->addWidget(go_btn_);

	chk_trains_ = new QCheckBox("Trains");       chk_trains_->setChecked(true);
	chk_builds_ = new QCheckBox("Builds");       chk_builds_->setChecked(true);
	chk_upgrades_ = new QCheckBox("UpgrTo");      chk_upgrades_->setChecked(true);
	chk_researches_ = new QCheckBox("Research");  chk_researches_->setChecked(true);
	chk_trained_by_ = new QCheckBox("TrndBy");    chk_trained_by_->setChecked(true);
	chk_built_by_ = new QCheckBox("BltBy");       chk_built_by_->setChecked(false);
	chk_upgraded_from_ = new QCheckBox("UpgrFr"); chk_upgraded_from_->setChecked(true);
	chk_repeats_ = new QCheckBox("Repeats");       chk_repeats_->setChecked(false);
	chk_repeats_->setToolTip("Show duplicate nodes (cycles) as grey repeat markers");

	auto conn = [this](QCheckBox* cb) { connect(cb, &QCheckBox::toggled, this, [this]() { rebuildTree(); }); };
	conn(chk_trains_); conn(chk_builds_); conn(chk_upgrades_); conn(chk_researches_);
	conn(chk_trained_by_); conn(chk_built_by_); conn(chk_upgraded_from_);
	conn(chk_repeats_);

	for (auto* cb : {chk_trains_, chk_builds_, chk_upgrades_, chk_researches_,
	                 chk_trained_by_, chk_built_by_, chk_upgraded_from_, chk_repeats_})
		top_row->addWidget(cb);
	top_row->addStretch();
	main_layout->addLayout(top_row);

	info_label_ = new QLabel;
	info_label_->setMaximumHeight(18);
	main_layout->addWidget(info_label_);

	// Splitter
	QSplitter* splitter = new QSplitter(Qt::Horizontal);
	tree_ = new QTreeWidget;
	tree_->setHeaderLabels({"Unit", "Relation"});
	tree_->setRootIsDecorated(true);
	tree_->setAlternatingRowColors(true);
	tree_->setIconSize(QSize(20, 20));
	tree_->header()->setStretchLastSection(true);
	tree_->setColumnWidth(0, 280);
	tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	splitter->addWidget(tree_);

	graph_scene_ = new QGraphicsScene(this);
	graph_view_ = new QGraphicsView(graph_scene_);
	graph_view_->setRenderHint(QPainter::Antialiasing);
	graph_view_->setDragMode(QGraphicsView::ScrollHandDrag);
	graph_view_->viewport()->installEventFilter(this);
	splitter->addWidget(graph_view_);

	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 5);
	main_layout->addWidget(splitter, 1);

	// Bottom zoom
	QHBoxLayout* zoom_row = new QHBoxLayout;
	QPushButton *zoom_in = new QPushButton("+"), *zoom_out = new QPushButton("-"), *zoom_fit = new QPushButton("Fit");
	zoom_in->setFixedWidth(26); zoom_out->setFixedWidth(26); zoom_fit->setFixedWidth(34);
	connect(zoom_in, &QPushButton::clicked,  this, [this]() { zoomGraph(1.25); });
	connect(zoom_out, &QPushButton::clicked, this, [this]() { zoomGraph(0.8); });
	connect(zoom_fit, &QPushButton::clicked, this, [this]() {
		graph_view_->fitInView(graph_scene_->sceneRect(), Qt::KeepAspectRatio);
	});
	QLabel* legend = new QLabel("Wheel=zoom  Drag=pan  Dbl-click tree=re-root  Dbl-click graph=re-root  Right-click=Object Editor");
	legend->setStyleSheet("color: gray; font-size: 10px;");
	zoom_row->addWidget(legend);
	zoom_row->addStretch();
	zoom_row->addWidget(zoom_out); zoom_row->addWidget(zoom_in); zoom_row->addWidget(zoom_fit);
	main_layout->addLayout(zoom_row);

	// tree dbl-click → re-root
	connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
		QString id = item->text(0).section(' ', 0, 0);
		if (id.length() == 4) { current_id_ = id; search_->setText(current_id_); rebuildTree(); }
	});

	// tree right-click → Open in Object Editor
	connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
		QTreeWidgetItem* item = tree_->itemAt(pos);
		if (!item) return;
		QString id = item->text(0).section(' ', 0, 0);
		if (id.length() != 4) return;
		QMenu menu;
		QAction* act = menu.addAction("Open in Object Editor");
		if (menu.exec(tree_->viewport()->mapToGlobal(pos)) == act) {
			bool created;
			auto* oe = window_handler.create_or_raise<class ObjectEditor>(nullptr, created);
			oe->show();
			oe->select_id(ObjectEditor::Category::unit, id.toStdString());
		}
	});

	connect(go_btn_, &QPushButton::clicked, this, [this]() { current_id_ = search_->text().trimmed(); rebuildTree(); });
	connect(search_, &QLineEdit::returnPressed, go_btn_, &QPushButton::click);
	connect(depth_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { rebuildTree(); });
	show();
}

void TechTreeViewer::zoomGraph(double factor) { graph_view_->scale(factor, factor); }
void TechTreeViewer::setUnit(const std::string& rawcode) {
	current_id_ = QString::fromStdString(rawcode);
	search_->setText(current_id_);
	rebuildTree();
}

std::vector<std::pair<std::string, std::string>> TechTreeViewer::getChildren(const std::string& id) {
	std::vector<std::pair<std::string, std::string>> children;
	std::set<std::string> enabled;
	if (chk_trains_->isChecked())       enabled.insert("trains");
	if (chk_builds_->isChecked())        enabled.insert("builds");
	if (chk_upgrades_->isChecked())      enabled.insert("upgrades to");
	if (chk_researches_->isChecked())    enabled.insert("researches");
	if (chk_trained_by_->isChecked())    enabled.insert("trained by");
	if (chk_built_by_->isChecked())      enabled.insert("built by");
	if (chk_upgraded_from_->isChecked()) enabled.insert("upgraded from");

	auto add = [&](const std::string& rel, const std::string& cid) {
		if (enabled.contains(rel) && units_slk.row_headers.contains(cid))
			children.push_back({cid, rel});
	};

	for (const auto& t : split_rcx(units_slk.data<std::string>("trains", id)))   add("trains", t);
	for (const auto& b : split_rcx(units_slk.data<std::string>("builds", id)))   add("builds", b);
	for (const char* f : {"upgrade", "upgrades", "revive"})
		for (const auto& u : split_rcx(units_slk.data<std::string>(f, id)))      add("upgrades to", u);
	for (const auto& r : split_rcx(units_slk.data<std::string>("researches", id))) add("researches", r);

	static std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> rev;
	static bool rev_ok = false;
	if (!rev_ok) {
		for (const auto& [rid, idx] : units_slk.row_headers) {
			for (const auto& t : split_rcx(units_slk.data<std::string>("trains", rid))) rev[t].push_back({rid, "trained by"});
			for (const auto& b : split_rcx(units_slk.data<std::string>("builds", rid))) rev[b].push_back({rid, "built by"});
			for (const char* f : {"upgrade","upgrades","revive"})
				for (const auto& u : split_rcx(units_slk.data<std::string>(f, rid))) rev[u].push_back({rid, "upgraded from"});
		}
		rev_ok = true;
	}
	if (auto it = rev.find(id); it != rev.end())
		for (const auto& [rid, rel] : it->second)
			if (enabled.contains(rel)) children.push_back({rid, rel});

	std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
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
	if (!units_slk.row_headers.contains(root_id)) { info_label_->setText("Not found: " + current_id_); return; }

	const std::string root_name = resolve_name(root_id);
	const bool is_bldg = units_slk.data<std::string>("isbldg", root_id) == "1";
	info_label_->setText(QString("Root: %1  %2  [%3]")
		.arg(current_id_, QString::fromStdString(root_name), QString::fromStdString(is_bldg ? "bldg" : "unit")));

	const int max_depth = depth_spin_->value();
	const bool show_repeats = chk_repeats_->isChecked();
	std::unordered_set<std::string> visited;

	std::function<void(QTreeWidgetItem*, const std::string&, int)> bt;
	bt = [&](QTreeWidgetItem* p, const std::string& id, int depth) {
		if (depth >= max_depth) return;
		bool rep = !visited.insert(id).second;
		if (rep) {
			if (show_repeats) {
				auto* r = new QTreeWidgetItem(p, {QString("(%1 rpt)").arg(QString::fromStdString(id)), "-"});
				r->setForeground(0, Qt::gray);
			}
			return;
		}
		for (const auto& [cid, rel] : getChildren(id)) {
			QString label = QString::fromStdString(cid + "  " + resolve_name(cid));
			auto* item = new QTreeWidgetItem(p, {label, edge_label(rel)});
			QIcon ic = get_unit_icon(cid); if (!ic.isNull()) item->setIcon(0, ic);
			QFont f = item->font(0); f.setBold(true); item->setFont(0, f);
			bt(item, cid, depth + 1);
		}
	};

	QString rl = QString::fromStdString(root_id + "  " + root_name);
	auto* ri = new QTreeWidgetItem(tree_, {rl, "ROOT"});
	QIcon rci = get_unit_icon(root_id); if (!rci.isNull()) ri->setIcon(0, rci);
	QFont rf = ri->font(0); rf.setBold(true); rf.setPointSize(rf.pointSize() + 1); ri->setFont(0, rf);
	tree_->addTopLevelItem(ri);
	bt(ri, root_id, 0);
	tree_->expandAll();

	rebuildGraph();
}

void TechTreeViewer::rebuildGraph() {
	const std::string root_id = current_id_.toStdString();
	const int max_depth = depth_spin_->value();
	const bool show_repeats = chk_repeats_->isChecked();

	std::unordered_set<std::string> visited;
	std::vector<NodeInfo> nodes;
	std::vector<std::tuple<std::string, std::string, std::string>> edges;
	std::unordered_map<std::string, int> depth_map;

	std::function<void(const std::string&, int)> collect;
	collect = [&](const std::string& id, int depth) {
		if (depth >= max_depth) return;
		bool rep = !visited.insert(id).second;
		if (rep) {
			if (show_repeats) {
				NodeInfo ni; ni.id = id; ni.name = resolve_name(id); ni.is_repeat = true;
				nodes.push_back(ni); depth_map[id] = depth;
			}
			return;
		}
		NodeInfo ni;
		ni.id = id; ni.name = resolve_name(id);
		ni.is_building = units_slk.data<std::string>("isbldg", id) == "1";
		ni.is_worker = !split_rcx(units_slk.data<std::string>("builds", id)).empty();
		ni.requires_field = units_slk.data<std::string>("requires", id);
		ni.children = getChildren(id);
		nodes.push_back(ni); depth_map[id] = depth;
		for (const auto& [cid, rel] : ni.children) {
			edges.push_back({id, cid, rel});
			collect(cid, depth + 1);
		}
	};
	collect(root_id, 0);

	std::unordered_map<int, std::vector<NodeInfo*>> dg;
	for (auto& n : nodes) dg[depth_map[n.id]].push_back(&n);

	const float cw = 250, rh = 100, bw = 210, bh = 70;

	for (int d = 0; d < max_depth && dg.count(d); ++d) {
		auto& grp = dg[d];
		float th = static_cast<float>(grp.size()) * rh;
		float sy = -th / 2.0f;
		for (size_t i = 0; i < grp.size(); ++i) {
			auto* n = grp[i];
			float x = static_cast<float>(d) * cw, y = sy + static_cast<float>(i) * rh;

			QColor col = node_color(n->is_building, n->is_worker, n->is_repeat);
			QPen border(col.darker(130), n->is_repeat ? 1 : 2, n->is_repeat ? Qt::DashLine : Qt::SolidLine);
			auto* rect = graph_scene_->addRect(QRectF(x, y, bw, bh), border, QBrush(col.lighter(130)));
			rect->setFlag(QGraphicsItem::ItemIsSelectable);
			store_id(rect, n->id);

			QString label = QString::fromStdString((n->is_repeat ? "(rpt) " : "") + n->id + "\n" + n->name);
			auto* txt = graph_scene_->addText(label); txt->setPos(x + 6, y + 2); txt->setDefaultTextColor(Qt::black);
			QFont f = txt->font(); f.setPointSize(8); if (n->name.size() > 18) f.setPointSize(7); txt->setFont(f);
			store_id(txt, n->id);

			QIcon ic = get_unit_icon(n->id);
			if (!ic.isNull()) { auto* pm = graph_scene_->addPixmap(ic.pixmap(36, 36)); pm->setPos(x + bw - 42, y + 12); store_id(pm, n->id); }

			// Requirements: names + icons
			if (!n->requires_field.empty() && !n->is_repeat) {
				auto reqs = split_rcx(n->requires_field);
				std::sort(reqs.begin(), reqs.end()); reqs.erase(std::unique(reqs.begin(), reqs.end()), reqs.end());
				float rx = x + 6, ry = y + 48;
				for (size_t ri = 0; ri < std::min(reqs.size(), size_t(3)); ++ri) {
					const std::string& rid = reqs[ri];
					QIcon ric = get_unit_icon(rid);
					if (!ric.isNull()) { auto* rp = graph_scene_->addPixmap(ric.pixmap(12, 12)); rp->setPos(rx, ry); store_id(rp, rid); rx += 16; }
					QString rn = QString::fromStdString(rid + " " + resolve_name(rid));
					auto* rt = graph_scene_->addText(rn); rt->setPos(rx, ry - 1); rx += rt->boundingRect().width() + 10;
					rt->setDefaultTextColor(QColor(180, 80, 80));
					QFont rf = rt->font(); rf.setPointSize(7); rt->setFont(rf);
				}
				if (reqs.size() > 3) {
					auto* more = graph_scene_->addText("..."); more->setPos(rx, ry - 1);
					more->setDefaultTextColor(QColor(180, 80, 80));
				}
			}
		}
	}

	for (const auto& [from, to, rel] : edges) {
		if (!depth_map.contains(from) || !depth_map.contains(to)) continue;
		auto& fg = dg[depth_map[from]], &tg = dg[depth_map[to]];
		float tfh = static_cast<float>(fg.size()) * rh, tth = static_cast<float>(tg.size()) * rh;
		float fx = 0, fy = 0, tx = 0, ty = 0;
		for (size_t i = 0; i < fg.size(); ++i) if (fg[i]->id == from) { fx = depth_map[from] * cw + bw; fy = -tfh/2 + i*rh + bh/2; break; }
		for (size_t i = 0; i < tg.size(); ++i) if (tg[i]->id == to)   { tx = depth_map[to] * cw;         ty = -tth/2 + i*rh + bh/2; break; }

		QPen pen(edge_color(rel), 1.5); if (rel == "trains" || rel == "builds") pen.setWidth(2);
		graph_scene_->addLine(QLineF(fx, fy, tx, ty), pen);
		float al = 8, ang = std::atan2(ty - fy, tx - fx);
		graph_scene_->addLine(QLineF(tx, ty, tx - al*std::cos(ang+0.4f), ty - al*std::sin(ang+0.4f)), pen);
		graph_scene_->addLine(QLineF(tx, ty, tx - al*std::cos(ang-0.4f), ty - al*std::sin(ang-0.4f)), pen);
	}

	graph_scene_->setSceneRect(graph_scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30));
	QRectF sr = graph_scene_->sceneRect();
	graph_view_->resetTransform();
	graph_view_->scale(1.5, 1.5);
	graph_view_->centerOn(sr.left() + 80, sr.center().y());
}

bool TechTreeViewer::eventFilter(QObject* obj, QEvent* event) {
	if (obj == graph_view_->viewport()) {
		if (event->type() == QEvent::Wheel) {
			auto* we = static_cast<QWheelEvent*>(event);
			double f = (we->angleDelta().y() > 0) ? 1.15 : 0.87;
			graph_view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
			graph_view_->scale(f, f);
			return true;
		}
		if (event->type() == QEvent::MouseButtonDblClick) {
			auto* me = static_cast<QMouseEvent*>(event);
			QGraphicsItem* item = graph_view_->itemAt(me->pos());
			if (item) {
				while (item->parentItem()) item = item->parentItem();
				std::string id = get_id(item);
				if (id.size() == 4 && units_slk.row_headers.contains(id)) {
					current_id_ = QString::fromStdString(id);
					search_->setText(current_id_);
					rebuildTree();
					return true;
				}
			}
		}
		if (event->type() == QEvent::MouseButtonRelease) {
			auto* me = static_cast<QMouseEvent*>(event);
			if (me->button() != Qt::RightButton) return false;
			QGraphicsItem* item = graph_view_->itemAt(me->pos());
			if (!item) return false;
			while (item->parentItem()) item = item->parentItem();
			std::string id = get_id(item);
			if (id.size() != 4 || !units_slk.row_headers.contains(id)) return false;
			QMenu menu;
			QAction* act = menu.addAction("Open in Object Editor");
			if (menu.exec(me->globalPosition().toPoint()) == act) {
				bool created;
				auto* oe = window_handler.create_or_raise<class ObjectEditor>(nullptr, created);
				oe->show();
				oe->select_id(ObjectEditor::Category::unit, id);
			}
			return true;
		}
	}
	return QMainWindow::eventFilter(obj, event);
}
