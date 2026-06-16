#pragma once

#include "ui_region_palette.h"

#include "palette.h"
#include "region_brush.h"

import QRibbon;

class RegionPalette : public Palette {
	Q_OBJECT

public:
	explicit RegionPalette(QWidget* parent = nullptr);
	~RegionPalette();

private:
	bool event(QEvent* e) override;

	/// Rebuilds the region list from the map regions and syncs the list selection with the brush selection
	void update_list();
	void update_properties();

	/// Creates a new, registered region centred on the camera and selects it.
	/// Mirrors the drag-to-create flow (unique name/number, preset colour, undo entry)
	/// so it is saved to war3map.w3r and emitted as a gg_rct_ rect in the map script.
	void create_region();

	/// Deletes the currently selected region(s) after a confirmation prompt.
	void delete_selected_regions();

	Ui::RegionPalette ui;
	RegionBrush brush;

	/// Guards against the list/property widgets and the brush updating each other in a loop
	bool updating = false;

	QRibbonTab* ribbon_tab = new QRibbonTab;
	QRibbonButton* selection_mode = new QRibbonButton;
	QRibbonButton* new_region = new QRibbonButton;
	QRibbonButton* delete_region = new QRibbonButton;

public slots:
	void deactivate(QRibbonTab* tab) override;
};
