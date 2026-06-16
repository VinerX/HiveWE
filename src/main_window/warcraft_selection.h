#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <QString>

class QWidget;

// Detection + interactive selection of the Warcraft III install folder. Shared by
// the first-run flow (main.cpp) and the "Change Game folder" action (hivewe.cpp)
// so both show the detected version and validate the pick the same way. Replaces
// the old silent auto-pick that could lock onto an outdated/wrong install.
namespace warcraft_selection {
namespace fs = std::filesystem;

// Candidate install dirs to offer, best first: registry (custom install paths)
// and well-known filesystem locations, de-duplicated, with CASC/Reforged-looking
// folders ordered ahead of classic ones.
std::vector<fs::path> detect_candidates();

// Human-readable version of a candidate (e.g. "1.36.2.21517"), or "unknown" when
// it cannot be read (e.g. a classic, pre-Reforged install with no .build.info).
std::string version_label(const fs::path& directory);

// Opens CASC on `directory` and confirms core game data is actually readable.
// Mutates the global hierarchy on success. Returns false for an old/incomplete
// install that fails to open or is missing required files.
bool validate_and_open(const fs::path& directory);

// Interactive selection loop. Shows the suggested folder and its version and lets
// the user accept it, browse for another, or cancel. Each pick is validated by
// actually opening CASC; failures explain why and loop. Returns the chosen,
// successfully-opened folder, or nullopt if the user cancelled.
// `reason` is shown at the top to distinguish first-run from an error retry.
// `allow_cancel` controls whether a Cancel button is offered (false at startup).
std::optional<fs::path> prompt(QWidget* parent, const std::vector<fs::path>& candidates,
							   bool allow_cancel, const QString& reason);
}
