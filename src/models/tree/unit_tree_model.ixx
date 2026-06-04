module;

#include <QMargins>
#include <QObject>
#include <QModelIndex>

export module UnitTreeModel;

import std;
import BaseTreeModel;
import SLK;
import Globals;
import UnorderedMap;

export class UnitTreeModel : public BaseTreeModel {
	Q_OBJECT

	struct Category {
		std::string name;
		BaseTreeItem* item;
	};

	hive::unordered_map<std::string, Category> categories;
	std::vector<std::string> rowToCategory;

	std::array<std::string, 4> subCategories = {
		"Units",
		"Buildings",
		"Heroes",
		"Special",
	};

	BaseTreeItem* getFolderParent(const std::string& id) const override {
		if (categories.empty()) {
			return rootItem;
		}

		const std::string_view race = units_slk.data<std::string_view>("race", id);
		const bool isBuilding = units_slk.data<std::string_view>("isbldg", id) == "1";
		const bool isHero = isupper(id.front());
		const bool isSpecial = units_slk.data<std::string_view>("special", id) == "1";

		int subIndex = 0;
		if (isSpecial) {
			subIndex = 3;
		} else if (isBuilding) {
			subIndex = 1;
		} else if (isHero) {
			subIndex = 2;
		}

		if (const auto found = categories.find(race); found != categories.end()) {
			return found->second.item->children[subIndex];
		} else {
			std::println("Unit with id: {} has no race set. Set a race!", id);
			return categories.begin()->second.item->children[subIndex];
		}
	}

  public:
	QVariant data(const QModelIndex& index, int role) const override {
		if (!index.isValid()) {
			return {};
		}

		BaseTreeItem* item = static_cast<BaseTreeItem*>(index.internalPointer());

		switch (role) {
			case Qt::EditRole:
			case Qt::DisplayRole:
				if (item->baseCategory) {
					return QString::fromStdString(categories.at(rowToCategory[index.row()]).name);
				} else if (item->subCategory) {
					return QString::fromStdString(subCategories[index.row()] + " (" + std::to_string(item->children.size()) + ")");
				} else {
					if (units_slk.data<std::string_view>("campaign", item->id) == "1") {
						const std::string_view properNames = units_slk.data<std::string_view>("propernames", item->id);

						if (!properNames.empty()) {
							return QString::fromUtf8(properNames).split(',').first();
						}
					}

					return QAbstractProxyModel::data(index, role).toString() + " " + sourceModel()->data(sourceModel()->index(slk->row_headers.at(item->id), slk->column_headers.at("editorsuffix")), role).toString();
				}
			default:
				return BaseTreeModel::data(index, role);
		}
	}

	explicit UnitTreeModel(QObject* parent)
		: BaseTreeModel(parent) {
		slk = &units_slk;

		for (const auto& [key, value] : unit_editor_data.section("unitRace")) {
			if (key == "Sort" || key == "NumValues") {
				continue;
			}

			categories[value[0]].name = value[1];
			categories[value[0]].item = new BaseTreeItem(rootItem);
			categories[value[0]].item->baseCategory = true;
			rowToCategory.push_back(value[0]);
		}

		for (const auto& i : rootItem->children) {
			for (const auto& subCategory : subCategories) {
				BaseTreeItem* item = new BaseTreeItem(i);
				item->subCategory = true;
			}
		}

		for (size_t i = 0; i < units_slk.rows(); i++) {
			if (auto found = units_slk.index_to_row.find(i); found != units_slk.index_to_row.end()) {
				const std::string id = found->second;
				BaseTreeItem* item = new BaseTreeItem(UnitTreeModel::getFolderParent(id));
				item->id = id;
				items.emplace(id, item);
			}
		}

		categoryChangeFields = { "race", "isbldg", "special" };
	}
};

#include "unit_tree_model.moc"