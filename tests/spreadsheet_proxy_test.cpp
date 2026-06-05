#include <doctest/doctest.h>
#include <QApplication>
#include <QAbstractTableModel>
#include <QIcon>

import std;
import SLK;

class TestTableModel : public QAbstractTableModel {
  public:
	slk::SLK* slk = nullptr;
	slk::SLK* meta_slk = nullptr;
	// Mirrors the real TableModel's war3map.wts table: "TRIGSTR_xxx" -> resolved text.
	std::map<std::string, QString> trigger_strings;

	explicit TestTableModel(slk::SLK* data_slk, slk::SLK* meta, QObject* parent = nullptr)
		: QAbstractTableModel(parent), slk(data_slk), meta_slk(meta) {}

	int rowCount(const QModelIndex& = QModelIndex()) const override { return static_cast<int>(slk->rows()); }
	int columnCount(const QModelIndex& = QModelIndex()) const override { return static_cast<int>(slk->columns()); }

	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
		if (!index.isValid()) return {};
		if (role == Qt::DisplayRole || role == Qt::EditRole) {
			const std::string raw =
				slk->data<std::string>(static_cast<size_t>(index.column()), static_cast<size_t>(index.row()));
			// Resolve TRIGSTR references like the real TableModel does (localized maps
			// store string fields as war3map.wts references rather than inline text).
			if (raw.starts_with("TRIGSTR")) {
				if (const auto it = trigger_strings.find(raw); it != trigger_strings.end()) {
					return it->second;
				}
			}
			return QString::fromStdString(raw);
		}
		if (role == Qt::CheckStateRole) {
			const std::string& field = slk->index_to_column.at(index.column());
			if (field != "isbldg") return {};
			const std::string val = slk->data<std::string>(static_cast<size_t>(index.column()), static_cast<size_t>(index.row()));
			return val == "1" ? Qt::Checked : Qt::Unchecked;
		}
		if (role == Qt::DecorationRole) {
			const std::string& field = slk->index_to_column.at(index.column());
			if (field == "file") return QIcon();
			return {};
		}
		return {};
	}

	Qt::ItemFlags flags(const QModelIndex& index) const override {
		if (!index.isValid()) return Qt::NoItemFlags;
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	}
};

// -----------------------------------------------------------------------
// Fixture: mock SLK with 5 objects x 5 columns (name, hp, race, isbldg, file)
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

		meta_slk.add_row("m_isbldg");
		meta_slk.set_shadow_data("field", "m_isbldg", "isbldg");
		meta_slk.set_shadow_data("displayname", "m_isbldg", "Is Building");
		meta_slk.set_shadow_data("type", "m_isbldg", "bool");

		meta_slk.add_row("m_file");
		meta_slk.set_shadow_data("field", "m_file", "file");
		meta_slk.set_shadow_data("displayname", "m_file", "Icon");
		meta_slk.set_shadow_data("type", "m_file", "icon");

		meta_slk.build_meta_map();

		data_slk.add_column("name");
		data_slk.add_column("hp");
		data_slk.add_column("race");
		data_slk.add_column("isbldg");
		data_slk.add_column("file");

		auto add = [&](const std::string& id, const std::string& n, const std::string& h,
		                const std::string& r, const std::string& b, const std::string& f) {
			data_slk.add_row(id);
			data_slk.set_shadow_data("name", id, n);
			data_slk.set_shadow_data("hp", id, h);
			data_slk.set_shadow_data("race", id, r);
			data_slk.set_shadow_data("isbldg", id, b);
			data_slk.set_shadow_data("file", id, f);
		};
		add("hpea", "Peasant",    "220", "human",    "0", "Human/Peasant/Peasant.blp");
		add("hfoo", "Footman",    "420", "human",    "0", "Human/Footman/Footman.blp");
		add("ogru", "Grunt",      "700", "orc",      "0", "Orc/Grunt/Grunt.blp");
		add("ohun", "Headhunter", "350", "orc",      "0", "Orc/HeadHunter/HeadHunter.blp");
		add("ewsp", "Wisp",       "120", "nightelf", "0", "NightElf/Wisp/Wisp.blp");
	}
};

static QApplication* ensure_qapp() {
	static int argc = 0;
	static QApplication app(argc, nullptr);
	return &app;
}

import TableModel;
#define private public
#include "object_editor/spreadsheet_editor.h"
#undef private

// -----------------------------------------------------------------------
// Row count, mapping, headers
// -----------------------------------------------------------------------

TEST_CASE("proxy – rowCount equals source") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");
	CHECK(proxy.rowCount() == 5);
	CHECK(proxy.columnCount() == 5);
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

	CHECK(proxy.headerData(5, Qt::Horizontal, Qt::DisplayRole).toString() == "customfield");
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

// -----------------------------------------------------------------------
// New feature tests
// -----------------------------------------------------------------------

TEST_CASE("proxy – building filter") {
	ensure_qapp();
	TestData d;
	d.data_slk.set_shadow_data("isbldg", "hpea", "1");
	d.data_slk.set_shadow_data("isbldg", "ogru", "1");
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	CHECK(proxy.rowCount() == 5);

	// buildings only
	proxy.setUnitTypeFilter(true, false);
	CHECK(proxy.rowCount() == 2);

	// units only (non-buildings)
	proxy.setUnitTypeFilter(false, true);
	CHECK(proxy.rowCount() == 3);

	// all
	proxy.setUnitTypeFilter(true, true);
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy – editor suffix filter") {
	ensure_qapp();
	TestData d;
	d.data_slk.set_shadow_data("editorsuffix", "hpea", "Campaign");
	d.data_slk.set_shadow_data("editorsuffix", "hfoo", "Campaign");
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setEditorSuffixFilter("Campaign");
	CHECK(proxy.rowCount() == 2);

	proxy.setEditorSuffixFilter("Custom");
	CHECK(proxy.rowCount() == 0);

	proxy.setEditorSuffixFilter("");
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy – icon column") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name", "file");

	CHECK(proxy.iconColumn() == 4);
	CHECK(proxy.nameColumn() == 0);
}

TEST_CASE("proxy – icon column not found") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name", "nonexistent");

	CHECK(proxy.iconColumn() == -1);
}

TEST_CASE("proxy – DecorationRole returns icon from icon column") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name", "file");

	QVariant deco = proxy.data(proxy.index(0, 0), Qt::DecorationRole);
	// TestTableModel returns QIcon() for "file" columns, valid but empty
	CHECK(deco.isValid());
}

TEST_CASE("proxy – manual sort ascending") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.sort(0, Qt::AscendingOrder);
	// Sorted by name: Footman < Grunt < Headhunter < Peasant < Wisp
	CHECK(proxy.data(proxy.index(0, 0)).toString() == "Footman");
	CHECK(proxy.data(proxy.index(4, 0)).toString() == "Wisp");
}

TEST_CASE("proxy – manual sort descending") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.sort(0, Qt::DescendingOrder);
	CHECK(proxy.data(proxy.index(0, 0)).toString() == "Wisp");
	CHECK(proxy.data(proxy.index(4, 0)).toString() == "Footman");
}

TEST_CASE("proxy – manual sort numeric") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.sort(1, Qt::AscendingOrder); // hp column
	// By numeric hp: 120, 220, 350, 420, 700
	CHECK(proxy.data(proxy.index(0, 1)).toString() == "120");
	CHECK(proxy.data(proxy.index(4, 1)).toString() == "700");
	CHECK(proxy.data(proxy.index(0, 0)).toString() == "Wisp");
	CHECK(proxy.data(proxy.index(4, 0)).toString() == "Grunt");
}

TEST_CASE("proxy – sort preserves filters") {
	ensure_qapp();
	TestData d;
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setRaceFilter("orc");
	// Only ogru (hp=700) and ohun (hp=350)
	proxy.sort(1, Qt::AscendingOrder); // hp
	CHECK(proxy.rowCount() == 2);
	CHECK(proxy.data(proxy.index(0, 0)).toString() == "Headhunter"); // hp=350
	CHECK(proxy.data(proxy.index(1, 0)).toString() == "Grunt");      // hp=700

	proxy.setRaceFilter("");
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy – triple filter: race + building + text") {
	ensure_qapp();
	TestData d;
	d.data_slk.set_shadow_data("isbldg", "hpea", "1");
	d.data_slk.set_shadow_data("isbldg", "hfoo", "1");
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setRaceFilter("human");
	proxy.setUnitTypeFilter(true, false); // buildings only
	proxy.setTextFilter("Foot");
	CHECK(proxy.rowCount() == 1);
}

TEST_CASE("delegate – sizeHint meets minimum") {
	ensure_qapp();
	SpreadsheetDelegate delegate;
	QStyleOptionViewItem opt;
	QSize sz = delegate.sizeHint(opt, QModelIndex());
CHECK(sz.height() >= 20);
}

TEST_CASE("proxy вЂ“ editor suffix filter decodes cp1251 russian text") {
	ensure_qapp();
	TestData d;
	d.data_slk.set_shadow_data("editorsuffix", "hpea", std::string("\xCA\xE0\xEC\xEF\xE0\xED\xE8\xFF", 8));
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setEditorSuffixFilter(QString::fromUtf8(u8"камп"));
	CHECK(proxy.rowCount() == 1);
	CHECK(proxy.data(proxy.index(0, 0), Qt::DisplayRole).toString() == "Peasant");
}

TEST_CASE("proxy – editor suffix filter resolves TRIGSTR (russian/localized maps)") {
	ensure_qapp();
	TestData d;
	// Russian-localized maps store the suffix as a war3map.wts reference, not the literal
	// text. The filter must resolve it (as the grid does) or kyrillic suffixes never match.
	d.data_slk.set_shadow_data("editorsuffix", "hpea", "TRIGSTR_008");
	d.data_slk.set_shadow_data("editorsuffix", "hfoo", "TRIGSTR_009");
	TestTableModel tm(&d.data_slk, &d.meta_slk);
	tm.trigger_strings["TRIGSTR_008"] = QString::fromUtf8(u8"ЭК");
	tm.trigger_strings["TRIGSTR_009"] = QString::fromUtf8(u8"Кампания");
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	proxy.setEditorSuffixFilter(QString::fromUtf8(u8"ЭК"));
	CHECK(proxy.rowCount() == 1);
	CHECK(proxy.data(proxy.index(0, 0), Qt::DisplayRole).toString() == "Peasant");

	// Case-insensitive match on kyrillic.
	proxy.setEditorSuffixFilter(QString::fromUtf8(u8"эк"));
	CHECK(proxy.rowCount() == 1);

	proxy.setEditorSuffixFilter(QString::fromUtf8(u8"камп"));
	CHECK(proxy.rowCount() == 1);
	CHECK(proxy.data(proxy.index(0, 0), Qt::DisplayRole).toString() == "Footman");

	proxy.setEditorSuffixFilter("");
	CHECK(proxy.rowCount() == 5);
}

TEST_CASE("proxy вЂ“ units tab exposes built-in computed columns") {
	ensure_qapp();
	TestData d;
	d.data_slk.add_column("abillist");
	d.data_slk.add_column("heroabillist");
	d.data_slk.add_column("abilskinlist");
	d.data_slk.add_column("heroabilskinlist");
	d.data_slk.add_column("dmgplus1");
	d.data_slk.add_column("dice1");
	d.data_slk.add_column("sides1");
	d.data_slk.add_column("cool1");

	d.data_slk.set_shadow_data("abillist", "hpea", "Adef,Aatk");
	d.data_slk.set_shadow_data("heroabillist", "hpea", "AHhb");
	d.data_slk.set_shadow_data("abilskinlist", "hpea", "Aatk");
	d.data_slk.set_shadow_data("dmgplus1", "hpea", "10");
	d.data_slk.set_shadow_data("dice1", "hpea", "5");
	d.data_slk.set_shadow_data("sides1", "hpea", "2");
	d.data_slk.set_shadow_data("cool1", "hpea", "2");

	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name", {}, "Units");

	const int abilities_col = proxy.findColumnByKey("__builtin_abilities");
	const int avg_col = proxy.findColumnByKey("__builtin_avgattack");
	const int dps_col = proxy.findColumnByKey("__builtin_dps");
	const int spellcount_col = proxy.findColumnByKey("__builtin_spellcount");

	CHECK(abilities_col >= 0);
	CHECK(avg_col >= 0);
	CHECK(dps_col >= 0);
	CHECK(spellcount_col >= 0);
	CHECK(proxy.columnCount() == static_cast<int>(d.data_slk.columns()) + 4);

	const QString abilities_text = proxy.data(proxy.index(0, abilities_col), Qt::DisplayRole).toString();
	CHECK(abilities_text.contains("AHhb"));
	CHECK(abilities_text.contains("Adef"));
	CHECK(proxy.data(proxy.index(0, avg_col), Qt::DisplayRole).toString() == "17.5");
	CHECK(proxy.data(proxy.index(0, dps_col), Qt::DisplayRole).toString() == "8.75");
	CHECK(proxy.data(proxy.index(0, spellcount_col), Qt::DisplayRole).toString() == "3");
}

TEST_CASE("proxy вЂ“ custom formula columns evaluate arithmetic expressions") {
	ensure_qapp();
	TestData d;
	d.data_slk.add_column("dmgplus1");
	d.data_slk.add_column("dice1");
	d.data_slk.add_column("sides1");
	d.data_slk.set_shadow_data("dmgplus1", "hpea", "12");
	d.data_slk.set_shadow_data("dice1", "hpea", "3");
	d.data_slk.set_shadow_data("sides1", "hpea", "4");

	TestTableModel tm(&d.data_slk, &d.meta_slk);
	SpreadsheetProxy proxy(&tm, &d.data_slk, &d.meta_slk, "name");

	QString error;
	const int formula_col = proxy.addCustomFormulaColumn("Burst", "dmgplus1 + dice1 * sides1", "Computed", &error);
	CHECK(error.isEmpty());
	CHECK(formula_col >= 0);
	CHECK(proxy.isComputedColumn(formula_col));
	CHECK(proxy.data(proxy.index(0, formula_col), Qt::DisplayRole).toString() == "24");
}
