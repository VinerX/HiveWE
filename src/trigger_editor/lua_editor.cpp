#include "lua_editor.h"

#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <cmath>

LuaEditor::LuaEditor(QWidget* parent) : QsciScintilla(parent) {
	lexer = new QsciLexerLua(this);
	lexer->setDefaultFont(QFont("Consolas", 10));

	lexer->setColor(QColor(0, 128, 0), QsciLexerLua::Comment);
	lexer->setColor(QColor(0, 128, 0), QsciLexerLua::LineComment);
	lexer->setColor(QColor(128, 128, 0), QsciLexerLua::Number);
	lexer->setColor(QColor(0, 0, 255), QsciLexerLua::Keyword);
	lexer->setColor(QColor(255, 128, 0), QsciLexerLua::String);
	lexer->setColor(QColor(255, 128, 0), QsciLexerLua::Character);
	lexer->setColor(QColor(255, 128, 0), QsciLexerLua::LiteralString);
	lexer->setColor(QColor(0, 128, 128), QsciLexerLua::Operator);
	lexer->setColor(QColor(128, 128, 128), QsciLexerLua::Preprocessor);
	lexer->setColor(QColor(128, 0, 255), QsciLexerLua::KeywordSet5);

	setLexer(lexer);
	setCaretForegroundColor(QColor(255, 255, 255));
	setMargins(2);
	setMarginType(0, QsciScintilla::MarginType::NumberMargin);
	setMarginWidth(0, "0000");
	setMarginWidth(1, 0);
	setMarginLineNumbers(0, true);

	SendScintilla(SCI_STYLESETFORE, STYLE_LINENUMBER, palette().color(QPalette::ColorRole::Text).rgb());
	SendScintilla(SCI_STYLESETBACK, STYLE_LINENUMBER, palette().color(QPalette::ColorRole::Base).rgb());

	setIndentationsUseTabs(true);
	setTabIndents(true);
	setIndentationGuides(true);
	setAutoIndent(true);
	setTabWidth(4);
	setEolMode(QsciScintilla::EolUnix);

	api = new QsciAPIs(lexer);
	setAutoCompletionSource(QsciScintilla::AutoCompletionSource::AcsAll);
	setAutoCompletionUseSingle(QsciScintilla::AcusExplicit);
	setAutoCompletionReplaceWord(false);
	setAutoCompletionThreshold(1);
	setAutoCompletionCaseSensitivity(false);
	SendScintilla(SCI_AUTOCSETCASEINSENSITIVEBEHAVIOUR, SC_CASEINSENSITIVEBEHAVIOUR_RESPECTCASE);
	SendScintilla(SCI_AUTOCSETCANCELATSTART, false);

	setBraceMatching(QsciScintilla::BraceMatch::SloppyBraceMatch);

	SendScintilla(SCI_SETMULTIPLESELECTION, true);
	SendScintilla(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION | SCVS_NOWRAPLINESTART);
	SendScintilla(SCI_SETADDITIONALSELECTIONTYPING, true);
	SendScintilla(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);

	setCallTipsBackgroundColor(palette().color(QPalette::ColorRole::Base));
	setCallTipsForegroundColor(palette().color(QPalette::ColorRole::Text).darker());
	setCallTipsHighlightColor(palette().color(QPalette::ColorRole::Text));

	indicatorDefine(IndicatorStyle::StraightBoxIndicator, search_indicator);
	SendScintilla(SCI_INDICSETFORE, search_indicator, qRgb(0, 56, 119));
	SendScintilla(SCI_INDICSETALPHA, search_indicator, 255);
	SendScintilla(SCI_INDICSETUNDER, search_indicator, true);

	load_wc3_api();

	connect(this, &QsciScintilla::textChanged, this, &LuaEditor::calculate_margin_width);
}

void LuaEditor::load_wc3_api() {
	QString api_path = QCoreApplication::applicationDirPath() + "/data/tools/wc3_lua_api.json";
	if (!QFile::exists(api_path)) {
		api_path = QDir::currentPath() + "/data/tools/wc3_lua_api.json";
	}

	QFile file(api_path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	file.close();

	if (!doc.isObject()) {
		return;
	}

	QJsonObject root = doc.object();

	QStringList wc3_keywords;
	if (root.contains("functions")) {
		QJsonArray functions = root["functions"].toArray();
		for (const auto& fn : functions) {
			QJsonObject fn_obj = fn.toObject();
			QString name = fn_obj["name"].toString();
			QString args = fn_obj["args"].toString();
			QString returns = fn_obj["returns"].toString();

			wc3_keywords.append(name);

			QString declaration = name;
			if (!args.isEmpty()) {
				declaration += "(" + args + ")";
			}
			api->add(declaration);
		}
	}

	if (root.contains("types")) {
		QJsonArray types = root["types"].toArray();
		for (const auto& t : types) {
			wc3_keywords.append(t.toString());
			api->add(t.toString());
		}
	}

	if (root.contains("constants")) {
		QJsonArray constants = root["constants"].toArray();
		for (const auto& c : constants) {
			wc3_keywords.append(c.toString());
			api->add(c.toString());
		}
	}

	if (!wc3_keywords.isEmpty()) {
		QByteArray kw = wc3_keywords.join(' ').toUtf8();
		SendScintilla(SCI_SETKEYWORDS, 4, kw.data());
	}

	api->prepare();
}

void LuaEditor::highlight_text(const std::string& search_text) {
	SendScintilla(SCI_SETINDICATORCURRENT, search_indicator);
	SendScintilla(SCI_INDICATORCLEARRANGE, 0, text().length());

	if (search_text.empty()) {
		return;
	}

	SendScintilla(SCI_TARGETWHOLEDOCUMENT);

	int found = SendScintilla(SCI_SEARCHINTARGET, search_text.size(), search_text.data());
	while (found != -1) {
		int line_start = SendScintilla(SCI_LINEFROMPOSITION, found);
		int line_end = SendScintilla(SCI_LINEFROMPOSITION, found + (int)search_text.size());

		int index = SendScintilla(SCI_POSITIONFROMLINE, line_start);

		fillIndicatorRange(line_start, found - index, line_end, found - index + search_text.size(), 8);

		SendScintilla(SCI_SETTARGETRANGE, found + search_text.size(), text().length());

		found = SendScintilla(SCI_SEARCHINTARGET, search_text.size(), search_text.data());
	}
}

void LuaEditor::calculate_margin_width() {
	const int new_width = std::log10(lines()) + 2;
	if (new_width != max_line_number_width) {
		max_line_number_width = new_width;
		setMarginWidth(0, QString('0').repeated(new_width));
	}
}
