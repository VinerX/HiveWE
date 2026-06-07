#include "techtree_viewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QStringList>

import std;
import Globals;
import Utilities;

namespace {

std::vector<std::string> split_list(const std::string& val) {
	std::vector<std::string> result;
	if (val.empty()) return result;
	std::string_view sv(val);
	if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"') {
		sv = sv.substr(1, sv.size() - 2);
	}
	if (sv.find(',') == std::string_view::npos) {
		result.push_back(std::string(sv));
		return result;
	}
	std::size_t start = 0;
	while (start < sv.size()) {
		std::size_t end = sv.find(',', start);
		if (end == std::string_view::npos) end = sv.size();
		auto token = sv.substr(start, end - start);
		while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
		while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
		if (!token.empty()) result.push_back(std::string(token));
		start = end + 1;
	}
	return result;
}

std::string unit_name(const std::string& id) {
	if (!units_slk.row_headers.contains(id)) return {};
	auto name = units_slk.data<std::string>("name", id);
	if (name.starts_with("TRIGSTR")) {
		return id;
	}
	return name;
}

std::string ability_name(const std::string& id) {
	if (abilities_slk.data<std::string>("code", id).empty()) return id;
	auto name = abilities_slk.data<std::string>("name", id);
	if (name.starts_with("TRIGSTR")) return id;
	return name;
}

std::string upgrade_name(const std::string& id) {
	if (!upgrade_slk.row_headers.contains(id)) return id;
	auto name = upgrade_slk.data<std::string>("name", id);
	if (name.starts_with("TRIGSTR")) return id;
	return name;
}

} // namespace

TechTreeViewer::TechTreeViewer(QWidget* parent)
	: QMainWindow(parent) {
	setWindowTitle("Tech Tree Viewer");
	resize(560, 640);

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

	tree_ = new QTreeWidget;
	tree_->setHeaderLabels({"Unit", "Relation"});
	tree_->setRootIsDecorated(true);
	tree_->setAlternatingRowColors(true);
	tree_->header()->setStretchLastSection(true);
	main_layout->addWidget(tree_);

	// Double-click to navigate to that unit
	connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
		QString text = item->text(0);
		QString id = text.section(' ', 0, 0);
		if (id.length() == 4) {
			current_id_ = id;
			search_->setText(current_id_);
			rebuildTree();
		}
	});

	QLabel* legend = new QLabel("Double-click a node to re-root. "
		"Shows: trains, builds, upgrades to, researches, "
		"trained by, built by, upgraded from.");
	legend->setWordWrap(true);
	main_layout->addWidget(legend);

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

void TechTreeViewer::rebuildTree() {
	tree_->clear();
	if (current_id_.isEmpty()) return;

	const std::string root_id = current_id_.toStdString();
	if (!units_slk.row_headers.contains(root_id)) {
		info_label_->setText("Unit not found: " + current_id_);
		return;
	}

	const std::string root_name = unit_name(root_id);
	const bool is_bldg = units_slk.data<std::string>("isbldg", root_id) == "1";
	const std::string type_str = is_bldg ? " [building]" : " [unit]";
	info_label_->setText(QString("Tracing: %1 (%2)%3")
		.arg(current_id_, QString::fromStdString(root_name), QString::fromStdString(type_str)));

	// Precompute reverse lookups
	std::unordered_map<std::string, std::vector<std::string>> trained_by;
	std::unordered_map<std::string, std::vector<std::string>> built_by;
	std::unordered_map<std::string, std::vector<std::string>> upgraded_from;

	for (const auto& [row_id, idx] : units_slk.row_headers) {
		for (const auto& t : split_list(units_slk.data<std::string>("trains", row_id)))
			trained_by[t].push_back(row_id);
		for (const auto& b : split_list(units_slk.data<std::string>("builds", row_id)))
			built_by[b].push_back(row_id);
		for (const char* f : {"upgrade", "upgrades", "revive"}) {
			for (const auto& u : split_list(units_slk.data<std::string>(f, row_id)))
				upgraded_from[u].push_back(row_id);
		}
	}

	// Lambda to get children of a unit
	auto get_children = [&](const std::string& id) -> std::vector<std::pair<std::string, std::string>> {
		std::vector<std::pair<std::string, std::string>> children; // {rawcode, relation}

		// Forward: what this unit builds/trains/upgrades-to/researches
		for (const auto& t : split_list(units_slk.data<std::string>("trains", id)))
			if (units_slk.row_headers.contains(t))
				children.push_back({t, "trains"});
		for (const auto& b : split_list(units_slk.data<std::string>("builds", id)))
			if (units_slk.row_headers.contains(b))
				children.push_back({b, "builds"});
		for (const char* f : {"upgrade", "upgrades", "revive"}) {
			for (const auto& u : split_list(units_slk.data<std::string>(f, id)))
				if (units_slk.row_headers.contains(u))
					children.push_back({u, "upgrades to"});
		}
		for (const auto& r : split_list(units_slk.data<std::string>("researches", id)))
			children.push_back({r, "researches"});

		// Reverse: who trains/builds/upgrades-to this unit
		if (auto it = trained_by.find(id); it != trained_by.end())
			for (const auto& t : it->second)
				children.push_back({t, "trained by"});
		if (auto it = built_by.find(id); it != built_by.end())
			for (const auto& b : it->second)
				children.push_back({b, "built by"});
		if (auto it = upgraded_from.find(id); it != upgraded_from.end())
			for (const auto& u : it->second)
				children.push_back({u, "upgraded from"});

		// Sort by relation then name
		std::sort(children.begin(), children.end(),
			[](const auto& a, const auto& b) {
				if (a.second != b.second) return a.second < b.second;
				return unit_name(a.first) < unit_name(b.first);
			});
		children.erase(std::unique(children.begin(), children.end()), children.end());
		return children;
	};

	// Build tree recursively with depth limit and cycle detection
	std::unordered_set<std::string> visited;
	const int max_depth = depth_spin_->value();

	std::function<void(QTreeWidgetItem*, const std::string&, int)> build;
	build = [&](QTreeWidgetItem* parent_item, const std::string& id, int depth) {
		if (depth >= max_depth) return;
		if (!visited.insert(id).second) return;

		for (const auto& [child_id, relation] : get_children(id)) {
			const std::string name = unit_name(child_id);
			const bool is_b = units_slk.data<std::string>("isbldg", child_id) == "1";
			const QString type_tag = is_b ? "[B] " : "[U] ";

			QString label = QString("%1 (%2)").arg(QString::fromStdString(child_id),
				QString::fromStdString(name));
			QString detail = QString::fromStdString(relation);

			QTreeWidgetItem* item = new QTreeWidgetItem(parent_item,
				{label, detail});
			QFont f = item->font(0);
			f.setBold(true);
			item->setFont(0, f);
			item->setToolTip(0, type_tag + label);
			item->setToolTip(1, detail);

			// Placeholder child to enable expand arrow (lazy load would go here)
			if (depth + 1 < max_depth) {
				build(item, child_id, depth + 1);
			}
		}
	};

	QString root_label = QString("%1 (%2)").arg(current_id_, QString::fromStdString(root_name));
	QTreeWidgetItem* root_item = new QTreeWidgetItem(tree_, {root_label, "ROOT"});
	QFont rf = root_item->font(0);
	rf.setBold(true);
	rf.setPointSize(rf.pointSize() + 1);
	root_item->setFont(0, rf);

	tree_->addTopLevelItem(root_item);
	build(root_item, root_id, 0);
	tree_->expandAll();
}
