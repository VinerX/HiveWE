module;

#include "main_window/glwidget.h"

export module GLGlobals;

// The shared OpenGL context widget. Lives in its own module (separate from
// Globals) so that Globals — which holds the SLK/INI object-data tables — stays
// free of any Qt/OpenGL dependency and can be reused by the headless data layer
// (and the CLI). Only render-bound code (brushes, doodad/unit placement) needs
// this. GLWidget is defined in the global module fragment above; exporting a
// GLWidget* makes the full type reachable to importers, exactly as it was when
// this lived in Globals.
export inline GLWidget* context;
