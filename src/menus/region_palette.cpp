#include "region_palette.h"

#include <QListWidgetItem>
#include <QMessageBox>
#include <QPixmap>

import std;
import SLK;
import MapGlobal;
import Globals;
import Camera;
import Regions;
import RegionsUndo;
import WorldUndoManager;

RegionPalette::RegionPalette(QWidget* parent) : Palette(parent) {
	ui.setupUi(this);
	setAttribute(Qt::WA_DeleteOnClose);
	show();

	map->brush = &brush;

	QRibbonSection* region_section = new QRibbonSection;
	region_section->setText("Region");

	// Explicit create button: regions are otherwise only made by left-drag on the
	// terrain (placement mode), which is not discoverable. This drops a default-sized
	// region at the camera centre, fully registered (saved + gg_rct_ in the script).
	new_region->setText("New\nRegion");
	new_region->setIcon(QIcon("data/icons/ribbon/new.ico"));
	region_section->addWidget(new_region);

	delete_region->setText("Delete\nRegion");
	delete_region->setIcon(QIcon("data/icons/terrain/boundary_remove.png"));
	region_section->addWidget(delete_region);

	ribbon_tab->addSection(region_section);

	QRibbonSection* selection_section = new QRibbonSection;
	selection_section->setText("Selection");

	selection_mode->setText("Selection\nMode");
	selection_mode->setIcon(QIcon("data/icons/Ribbon/select.png"));
	selection_mode->setCheckable(true);
	selection_section->addWidget(selection_mode);
	selection_mode->setShortCut(Qt::Key_Space, { this, parent });

	ribbon_tab->addSection(selection_section);

	connect(selection_mode, &QRibbonButton::toggled, [&]() { brush.switch_mode(); });
	connect(new_region, &QRibbonButton::clicked, this, &RegionPalette::create_region);
	connect(delete_region, &QRibbonButton::clicked, this, &RegionPalette::delete_selected_regions);

	slk::SLK weather_slk("TerrainArt/Weather.slk");
	weather_slk.substitute(world_edit_strings, "WorldEditStrings");

	ui.weather->addItem("None", "");
	for (size_t i = 1; i < weather_slk.rows(); i++) {
		ui.weather->addItem(
			QString::fromUtf8(weather_slk.data<std::string_view>("name", i)),
			QString::fromUtf8(weather_slk.data<std::string_view>("effectid", i))
		);
	}

	ui.ambientSound->addItem("None", "");
	for (const auto& sound : map->sounds.sounds) {
		std::string variable = sound.name;
		if (!variable.starts_with("gg_snd_")) {
			variable = "gg_snd_" + variable;
		}
		ui.ambientSound->addItem(QString::fromStdString(sound.name), QString::fromStdString(variable));
	}

	// Queued so the rebuild happens after undo/redo has finished mutating the regions,
	// as the selection is cleared (and thus signalled) before the undo action runs
	connect(&brush, &RegionBrush::regions_changed, this, &RegionPalette::update_list, Qt::QueuedConnection);
	connect(&brush, &RegionBrush::selection_changed, this, &RegionPalette::update_list, Qt::QueuedConnection);

	connect(ui.regionList, &QListWidget::itemSelectionChanged, [&]() {
		if (updating) {
			return;
		}

		brush.selections.clear();
		for (const auto& item : ui.regionList->selectedItems()) {
			const int creation_number = item->data(Qt::UserRole).toInt();
			for (auto& region : map->regions.regions) {
				if (region.creation_number == creation_number) {
					brush.selections.emplace(&region);
				}
			}
		}
		update_properties();
	});

	connect(ui.regionList, &QListWidget::itemDoubleClicked, [&](QListWidgetItem* item) {
		const int creation_number = item->data(Qt::UserRole).toInt();
		for (const auto& region : map->regions.regions) {
			if (region.creation_number == creation_number) {
				camera.position.x = (region.left + region.right) / 2.f;
				camera.position.y = (region.bottom + region.top) / 2.f;
				break;
			}
		}
	});

	connect(ui.name, &QLineEdit::editingFinished, [&]() {
		if (updating || brush.selections.empty()) {
			return;
		}

		const std::string name = ui.name->text().toStdString();
		// editingFinished also fires on focus loss without any edits
		if ((*brush.selections.begin())->name == name) {
			return;
		}

		if (name.empty()) {
			update_properties();
			return;
		}

		// Region names have to be unique, so when the name is already taken
		// (or applied to multiple regions at once) a number is appended
		std::unordered_set<std::string> taken_names;
		for (auto& region : map->regions.regions) {
			if (!brush.selections.contains(&region)) {
				taken_names.emplace(region.name);
			}
		}

		brush.start_action();
		for (const auto& region : brush.selections) {
			std::string unique_name = name;
			for (int counter = 2; taken_names.contains(unique_name); counter++) {
				unique_name = std::format("{} {}", name, counter);
			}
			region->name = unique_name;
			taken_names.emplace(unique_name);
		}
		brush.end_action();
		update_list();
	});

	// ColorButton updates its color on click before this runs since its own clicked handler was connected first
	connect(ui.color, &QPushButton::clicked, [&]() {
		if (updating || brush.selections.empty()) {
			return;
		}

		const QColor color = ui.color->getColor();
		if (!color.isValid()) {
			return;
		}

		const glm::u8vec3 new_color = { color.red(), color.green(), color.blue() };
		if ((*brush.selections.begin())->color == new_color) {
			return;
		}

		brush.start_action();
		for (const auto& region : brush.selections) {
			region->color = new_color;
		}
		brush.end_action();
		update_list();
	});

	connect(ui.weather, QOverload<int>::of(&QComboBox::currentIndexChanged), [&]() {
		if (updating || brush.selections.empty()) {
			return;
		}

		const std::string weather_id = ui.weather->currentData().toString().toStdString();
		brush.start_action();
		for (const auto& region : brush.selections) {
			region->weather_id = weather_id;
		}
		brush.end_action();
	});

	connect(ui.ambientSound, QOverload<int>::of(&QComboBox::currentIndexChanged), [&]() {
		if (updating || brush.selections.empty()) {
			return;
		}

		const std::string ambient_id = ui.ambientSound->currentData().toString().toStdString();
		brush.start_action();
		for (const auto& region : brush.selections) {
			region->ambient_id = ambient_id;
		}
		brush.end_action();
	});

	update_list();
}

RegionPalette::~RegionPalette() {
	map->brush = nullptr;
}

void RegionPalette::update_list() {
	updating = true;

	ui.regionList->clear();
	for (const auto& region : map->regions.regions) {
		QPixmap pixmap(16, 16);
		pixmap.fill(QColor(region.color.r, region.color.g, region.color.b));

		QListWidgetItem* item = new QListWidgetItem(QIcon(pixmap), QString::fromStdString(region.name));
		item->setData(Qt::UserRole, region.creation_number);
		ui.regionList->addItem(item);

		const bool selected = std::ranges::any_of(brush.selections, [&](const Region* i) {
			return i->creation_number == region.creation_number;
		});
		item->setSelected(selected);
	}

	updating = false;
	update_properties();
}

void RegionPalette::update_properties() {
	updating = true;

	if (brush.selections.empty()) {
		ui.properties->setEnabled(false);
		ui.name->clear();
	} else {
		ui.properties->setEnabled(true);

		const Region& region = **brush.selections.begin();
		ui.name->setText(QString::fromStdString(region.name));
		ui.color->setColor(QColor(region.color.r, region.color.g, region.color.b));

		const int weather_index = ui.weather->findData(QString::fromStdString(region.weather_id));
		ui.weather->setCurrentIndex(std::max(0, weather_index));

		const int ambient_index = ui.ambientSound->findData(QString::fromStdString(region.ambient_id));
		ui.ambientSound->setCurrentIndex(std::max(0, ambient_index));
	}

	updating = false;
}

void RegionPalette::create_region() {
	// Regions snap to the cell grid (a quarter of a tile), matching the brush
	const auto snap = [](float v) { return std::round(v * 4.f) / 4.f; };

	// Default 4x4 tile region centred on the current camera focus
	const float half = 2.f;
	const float cx = camera.position.x;
	const float cy = camera.position.y;

	Region region;
	region.left = snap(cx - half);
	region.right = snap(cx + half);
	region.bottom = snap(cy - half);
	region.top = snap(cy + half);
	region.name = map->regions.get_unique_name();
	region.creation_number = map->regions.get_unique_creation_number();
	region.color = region_preset_colors[region.creation_number % region_preset_colors.size()];

	map->regions.regions.push_back(region);
	Region* added = &map->regions.regions.back();

	// Same registration the drag-create path uses: an undo entry so the region is
	// part of the document, saved to war3map.w3r and written as gg_rct_<name>.
	auto undo = std::make_unique<RegionAddAction>();
	undo->regions.push_back(*added);
	map->world_undo.new_undo_group();
	map->world_undo.add_undo_action(std::move(undo));

	// Select the new region so its properties (name/colour/...) are ready to edit
	brush.selections = { added };
	update_list();
}

void RegionPalette::delete_selected_regions() {
	if (brush.selections.empty()) {
		return;
	}

	const size_t count = brush.selections.size();
	const QString text = count == 1
		? QString("Delete region \"%1\"?").arg(QString::fromStdString((*brush.selections.begin())->name))
		: QString("Delete %1 selected regions?").arg(count);

	if (QMessageBox::question(this, "Delete regions", text) != QMessageBox::Yes) {
		return;
	}

	// Handles the (batched) undo entry, removal and list refresh for all selected regions
	brush.delete_selection();
}

bool RegionPalette::event(QEvent* e) {
	if (e->type() == QEvent::Close) {
		// Remove shortcut from parent
		selection_mode->disconnectShortcuts();
		ribbon_tab->setParent(nullptr);
		delete ribbon_tab;
	} else if (e->type() == QEvent::WindowActivate) {
		selection_mode->enableShortcuts();
		map->brush = &brush;
		emit ribbon_tab_requested(ribbon_tab, "Region Palette");
		// The regions may have changed (e.g. through undo/redo) while another brush was active
		update_list();
	}
	return QWidget::event(e);
}

void RegionPalette::deactivate(QRibbonTab* tab) {
	if (tab != ribbon_tab) {
		brush.clear_selection();
		selection_mode->disableShortcuts();
	}
}
