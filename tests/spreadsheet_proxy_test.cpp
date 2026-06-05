#include <doctest/doctest.h>
#include <QApplication>
#include <QAbstractTableModel>

import std;
import SLK;

// Minimal table model wrapping an SLK, for unit-testing SpreadsheetProxy
// without the heavy TriggerStrings / resource_manager dependencies.
class TestTableModel : public QAbstractTableModel {
  public:
	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;

	explicit TestTableModel(slk::SLK* data_slk, slk::SLK* meta, QObject* parent = nullptr)
		: QAbstractTableModel(parent), slk(data_slk), meta_slk(meta) {}

	int rowCount(const QModelIndex& = QModelIndex()) const override { return static_cast<int>(slk->rows()); }
	int columnCount(const QModelIndex& = QModelIndex()) const override { return static_cast<int>(slk->columns()); }

	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
		if (!index.isValid()) return {};
		if (role == Qt::DisplayRole || role == Qt::EditRole) {
			return QString::fromStdString(
				slk->data<std::string>(static_cast<size_t>(index.column()), static_cast<size_t>(index.row())));
		}
		return {};
	}
};

// -----------------------------------------------------------------------
// Fixture: mock SLK with 5 objects × 3 columns (name, hp, race)
// -----------------------------------------------------------------------

struct TestData {
	slk::SLK data_slk;
	slk::SLK meta_slk;

	TestData() {
		meta_slk.add_row("m_hp");
		meta_slk.set_shadow_data("field", "m_hp", "hp");
		meta_slk.set_shadow_data("displayname", "m_hp", "Health");
		meta_slk.set_shadow_data("type", "m_hp", "int");

		meta_slk.add_row("m_name");
		meta_slk.set_shadow_data("field", "m_name", "name");
		meta_slk.set_shadow_data("displayname", "m_name", "Unit Name");
		meta_slk.set_shadow_data("type", "m_name", "string");

		meta_slk.add_row("m_race");
		meta_slk.set_shadow_data("field", "m_race", "race");
		meta_slk.set_shadow_data("displayname", "m_race", "Race");
		meta_slk.set_shadow_data("type", "m_race", "string");

		meta_slk.build_meta_map();

		data_slk.add_column("name");
		data_slk.add_column("hp");
		data_slk.add_column("race");

		auto add = [&](const std::string& id, const std::string& n, const std::string& h, const std::string& r) {
			data_slk.add_row(id);
			data_slk.set_shadow_data("name", id, n);
			data_slk.set_shadow_data("hp", id, h);
			data_slk.set_shadow_data("race", id, r);
		};
		add("hpea", "Peasant",    "220", "human");
		add("hfoo", "Footman",    "420", "human");
		add("ogru", "Grunt",      "700", "orc");
		add("ohun", "Headhunter", "350", "orc");
		add("ewsp", "Wisp",       "120", "nightelf");
	}
};

static QApplication* ensure_qapp() {
	static int argc = 0;
	static QApplication app(argc, nullptr);
	return &app;
}

// We need TableModel module imported because the header does `import TableModel;`
// even though our test proxy only uses QAbstractItemModel.
import TableModel;
#define private public
#include "object_editor/spreadsheet_editor.h"
#undef private

// -----------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------

TEST_CASE("proxy – rowCount equals source") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");
	CHECK(proxy.rowCount() == 5);
	CHECK(proxy.columnCount() == 3);
}

TEST_CASE("proxy – mapToSource identity unfiltered") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");
	for (int i = 0; i < proxy.rowCount(); ++i)
		CHECK(proxy.mapToSource(proxy.index(i, 0)).row() == i);
}

TEST_CASE("proxy – text filter") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setTextFilter("Pea");
	CHECK(proxy.rowCount() == 1);

	proxy.setTextFilter("");
	CHECK(proxy.rowCount() == 5);

	proxy.setTextFilter("NOMATCH");
	CHECK(proxy.rowCount() == 0);
}

TEST_CASE("proxy – race filter") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setRaceFilter("orc");
	CHECK(proxy.rowCount() == 2);

	proxy.setRaceFilter("human");
	CHECK(proxy.rowCount() == 2);

	proxy.setRaceFilter("");
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy – custom only") {
	ensure_qapp();
	TestData d;
	d.data_slk.shadow_data["hfoo"]["oldid"] = "hfoo_base";
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setCustomOnly(true);
	CHECK(proxy.rowCount() == 1);

	proxy.setCustomOnly(false);
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy – combined filters") {
	ensure_qapp();
	TestData d;
	d.data_slk.shadow_data["hfoo"]["oldid"] = "hfoo_base";
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setRaceFilter("human");
	proxy.setCustomOnly(true);
	CHECK(proxy.rowCount() == 1);
}

TEST_CASE("proxy – fieldDisplayName") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString() == "Unit Name");
	CHECK(proxy.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString() == "Health");
	CHECK(proxy.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString() == "Race");
}

TEST_CASE("proxy – fieldDisplayName fallback") {
	ensure_qapp();
	TestData d;
	d.data_slk.add_column("customfield");
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString() == "customfield");
}

TEST_CASE("proxy – vertical headers") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.headerData(0, Qt::Vertical, Qt::DisplayRole).toString() == "hpea");
	CHECK(proxy.headerData(1, Qt::Vertical, Qt::DisplayRole).toString() == "hfoo");

	proxy.setRaceFilter("orc");
	CHECK(proxy.headerData(0, Qt::Vertical, Qt::DisplayRole).toString() == "ogru");
}

TEST_CASE("proxy – data passthrough") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.data(proxy.index(0, 1), Qt::DisplayRole).toString() == "220");
	CHECK(proxy.data(proxy.index(2, 0), Qt::DisplayRole).toString() == "Grunt");
}

TEST_CASE("proxy – flags check") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	auto f = proxy.flags(proxy.index(0, 0));
	CHECK((f & Qt::ItemIsEnabled) != Qt::NoItemFlags);
	CHECK((f & Qt::ItemIsSelectable) != Qt::NoItemFlags);
}

TEST_CASE("proxy – visible_rows_ unfiltered") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.visible_rows_.size() == 5);
	for (int i = 0; i < 5; ++i)
		CHECK(proxy.visible_rows_[static_cast<size_t>(i)] == i);
}

TEST_CASE("proxy – index/parent flat model") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.index(0, 1).isValid());
	CHECK(!proxy.parent(proxy.index(0, 0)).isValid());
	CHECK(!proxy.index(-1, 0).isValid());
	CHECK(!proxy.index(100, 0).isValid());
	CHECK(proxy.rowCount(proxy.index(0, 0)) == 0);
}

TEST_CASE("proxy – filter clear restores all") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setTextFilter("Footman");
	CHECK(proxy.visible_rows_.size() == 1);

	proxy.setTextFilter("");
	CHECK(proxy.visible_rows_.size() == 5);
}
