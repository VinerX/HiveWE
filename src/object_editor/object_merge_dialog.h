#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>

import std;
import ObjectData;

// Lets the user resolve object-data merge conflicts (cells that both the editor
// and the on-disk files changed). Writes the chosen side back into the plan's
// conflicts (take_theirs) on accept; the caller then applies the plan.
class ObjectMergeDialog : public QDialog {
  public:
	explicit ObjectMergeDialog(ObjectMergePlan& plan, QWidget* parent = nullptr)
		: QDialog(parent), plan(plan) {
		setWindowTitle("Merge object data from disk — resolve conflicts");
		resize(860, 500);

		auto* layout = new QVBoxLayout(this);

		auto* summary = new QLabel(
			QString("%1 change(s) will be merged automatically. %2 conflict(s) below were changed "
					"by BOTH the editor and the disk — choose which value to keep for each.")
				.arg(plan.changes.size() + plan.row_adds.size())
				.arg(plan.conflicts.size()),
			this);
		summary->setWordWrap(true);
		layout->addWidget(summary);

		table = new QTableWidget(static_cast<int>(plan.conflicts.size()), 5, this);
		table->setHorizontalHeaderLabels({ "Object", "Field", "Editor value (mine)", "Disk value (theirs)", "Keep" });
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setSelectionMode(QAbstractItemView::NoSelection);
		table->verticalHeader()->setVisible(false);

		const auto display = [](const std::optional<std::string>& v) {
			return v ? QString::fromStdString(*v) : QString("(base default)");
		};

		for (int i = 0; i < static_cast<int>(plan.conflicts.size()); i++) {
			const ObjectMergeConflict& c = plan.conflicts[i];
			table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(c.table + "  " + c.id)));
			table->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(c.field)));
			table->setItem(i, 2, new QTableWidgetItem(display(c.mine)));
			table->setItem(i, 3, new QTableWidgetItem(display(c.theirs)));

			auto* combo = new QComboBox;
			combo->addItem("Disk (theirs)");
			combo->addItem("Editor (mine)");
			combo->setCurrentIndex(c.take_theirs ? 0 : 1);
			combos.push_back(combo);
			table->setCellWidget(i, 4, combo);
		}
		table->resizeColumnsToContents();
		table->horizontalHeader()->setStretchLastSection(true);
		layout->addWidget(table);

		auto* tools = new QHBoxLayout;
		auto* all_disk = new QPushButton("Keep all disk", this);
		auto* all_editor = new QPushButton("Keep all editor", this);
		connect(all_disk, &QPushButton::clicked, [this] { for (auto* c : combos) c->setCurrentIndex(0); });
		connect(all_editor, &QPushButton::clicked, [this] { for (auto* c : combos) c->setCurrentIndex(1); });
		tools->addWidget(all_disk);
		tools->addWidget(all_editor);
		tools->addStretch();
		layout->addLayout(tools);

		auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, [this] {
			for (std::size_t i = 0; i < combos.size(); i++) {
				this->plan.conflicts[i].take_theirs = (combos[i]->currentIndex() == 0);
			}
			accept();
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);
	}

  private:
	ObjectMergePlan& plan;
	QTableWidget* table = nullptr;
	std::vector<QComboBox*> combos;
};
