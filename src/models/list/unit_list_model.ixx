module;

#include <QSortFilterProxyModel>
#include <QSize>

export module UnitListModel;

import BaseListModel;
import Globals;
import TableModel;

namespace {
	// Editor suffixes on localized (e.g. Russian) maps are stored as "TRIGSTR_xxx"
	// references rather than literal text, so reading the raw SLK value never matches
	// what the user sees. Resolve through units_table (EditRole resolves the TRIGSTR and
	// decodes cp1251/UTF-8) — the same path the Object Editor uses — for both display and
	// filtering. See [[localized-strings-trigstr]].
	QString resolved_editor_suffix(const size_t row) {
		const std::string_view raw = units_slk.data<std::string_view>("editorsuffix", row);
		if (raw.empty()) {
			return {};
		}
		if (raw.starts_with("TRIGSTR") && units_slk.column_headers.contains("editorsuffix")) {
			const int col = static_cast<int>(units_slk.column_headers.at("editorsuffix"));
			return units_table->data(units_table->index(static_cast<int>(row), col), Qt::EditRole).toString();
		}
		return QString::fromUtf8(raw);
	}
}

export class UnitListModel: public BaseListModel {
	Q_OBJECT

  public:
	explicit UnitListModel(QObject* parent = nullptr) : BaseListModel(units_slk, parent) {}

	[[nodiscard]]
	QModelIndex mapToSource(const QModelIndex& proxyIndex) const override {
		if (!proxyIndex.isValid()) {
			return {};
		}

		return sourceModel()->index(proxyIndex.row(), units_slk.column_headers.at("name"));
	}

	[[nodiscard]]
	QVariant data(const QModelIndex& index, int role) const override {
		if (!index.isValid()) {
			return {};
		}

		switch (role) {
			case Qt::DisplayRole: {
				const QString name = mapToSource(index).data(role).toString();
				const QString suffix = resolved_editor_suffix(index.row());
				return suffix.isEmpty() ? name : name + " " + suffix;
			}
			case Qt::UserRole:
				return QString::fromStdString("units/" + units_slk.data("race", index.row()) + "/" + units_slk.index_to_row.at(index.row()));
			case Qt::DecorationRole:
				return sourceModel()->index(index.row(), units_slk.column_headers.at("art")).data(role);
			default:
				return BaseListModel::data(index, role);
		}
	}
};

export class UnitListFilter: public QSortFilterProxyModel {
	Q_OBJECT

	[[nodiscard]]
	bool filterAcceptsRow(const int sourceRow, const QModelIndex& sourceParent) const override {
		if (!filterRegularExpression().pattern().isEmpty()) {
			if (QString::fromStdString(units_slk.index_to_row.at(sourceRow)).contains(filterRegularExpression())) {
				return true;
			}

			const QModelIndex source_index = sourceModel()->index(sourceRow, 0);
			return source_index.data().toString().contains(filterRegularExpression());
		}

		if (filterRace) {
			if (units_slk.data<std::string_view>("race", sourceRow) != filterRace->toStdString()) {
				return false;
			}
		}

		if (filterSuffix) {
			if (!resolved_editor_suffix(sourceRow).contains(*filterSuffix, Qt::CaseInsensitive)) {
				return false;
			}
		}

		return true;
	}

	[[nodiscard]]
	bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
		QString leftIndex = "0";
		{
			const bool isHostile = units_slk.data<std::string_view>("hostilepal", left.row()) == "1";
			const bool isBuilding = units_slk.data<std::string_view>("isbldg", left.row()) == "1";
			const bool isHero = isupper(units_slk.index_to_row.at(left.row()).front());
			const bool isSpecial = units_slk.data<std::string_view>("special", left.row()) == "1";

			if (isSpecial) {
				leftIndex = "3";
			} else if (isBuilding) {
				leftIndex = "1";
			} else if (isHero) {
				leftIndex = "2";
			}
			leftIndex += QString::fromUtf8(units_slk.data<std::string_view>("name", left.row()));
		}

		QString rightIndex = "0";
		{
			const bool isHostile = units_slk.data<std::string_view>("hostilepal", right.row()) == "1";
			const bool isBuilding = units_slk.data<std::string_view>("isbldg", right.row()) == "1";
			const bool isHero = isupper(units_slk.index_to_row.at(right.row()).front());
			const bool isSpecial = units_slk.data<std::string_view>("special", right.row()) == "1";

			if (isSpecial) {
				rightIndex = "3";
			} else if (isBuilding) {
				rightIndex = "1";
			} else if (isHero) {
				rightIndex = "2";
			}
			rightIndex += QString::fromUtf8(units_slk.data<std::string_view>("name", right.row()));
		}

		return leftIndex < rightIndex;
	}

	std::optional<QString> filterRace;
	std::optional<QString> filterSuffix;

  public:
	using QSortFilterProxyModel::QSortFilterProxyModel;

  public slots:

	void setFilterRace(const QString& race) {
  		beginFilterChange();
		filterRace = race;
  		endFilterChange(Direction::Rows);
	}

	// Case-insensitive "contains" match on the editor suffix; an empty string disables it.
	void setFilterSuffix(const QString& suffix) {
		beginFilterChange();
		filterSuffix = suffix.isEmpty() ? std::nullopt : std::optional<QString>(suffix);
		endFilterChange(Direction::Rows);
	}
};

#include "unit_list_model.moc"
