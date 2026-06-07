#pragma once

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qsciapis.h>
#include <QSet>
#include <QString>
#include <string>

class LuaEditor : public QsciScintilla {
	Q_OBJECT

public:
	explicit LuaEditor(QWidget* parent = nullptr);

	void highlight_text(const std::string& search_text);

private:
	void calculate_margin_width();

	QsciLexerLua* lexer;
	QsciAPIs* api;

	static constexpr int search_indicator = 8;
	int max_line_number_width = 0;

	void load_wc3_api();
};
