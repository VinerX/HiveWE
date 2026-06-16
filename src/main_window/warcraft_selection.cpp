#include "warcraft_selection.h"

#include <algorithm>
#include <system_error>

#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

import Hierarchy;
import Utilities;

namespace warcraft_selection {

std::vector<fs::path> detect_candidates() {
	std::vector<fs::path> result;

	const auto add = [&](fs::path candidate) {
		if (candidate.empty()) {
			return;
		}
		std::error_code ec;
		if (!fs::exists(candidate, ec)) {
			return;
		}
		for (const fs::path& existing : result) {
			if (existing == candidate) {
				return;
			}
		}
		result.push_back(std::move(candidate));
	};

	// Registry: the install path written by the Warcraft III / Battle.net
	// installer. Offered as a candidate, but the user still confirms it (and sees
	// its version) so a stale classic key can't silently win.
	for (const char* key : {
			 "HKEY_CURRENT_USER\\Software\\Blizzard Entertainment\\Warcraft III",
			 "HKEY_LOCAL_MACHINE\\Software\\Blizzard Entertainment\\Warcraft III",
			 "HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Blizzard Entertainment\\Warcraft III",
		 }) {
		QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
		for (const char* value : { "InstallPath", "GamePath" }) {
			const QString path = reg.value(QString::fromLatin1(value)).toString();
			if (!path.isEmpty()) {
				add(fs::path(path.toStdWString()));
			}
		}
	}

	for (const fs::path& candidate : find_warcraft_directories()) {
		add(candidate);
	}

	// Prefer folders that look like a CASC/Reforged install (have .build.info)
	// over classic MPQ installs, so the default suggestion is the modern game.
	std::stable_partition(result.begin(), result.end(), [](const fs::path& p) {
		return looks_like_warcraft_directory(p);
	});

	return result;
}

std::string version_label(const fs::path& directory) {
	const std::string version = read_warcraft_version(directory);
	return version.empty() ? "unknown" : version;
}

bool validate_and_open(const fs::path& directory) {
	if (directory.empty()) {
		return false;
	}
	if (!hierarchy.open_casc(directory)) {
		return false;
	}
	// Strict check: an install can "open" yet lack usable data (old/incomplete).
	// common.j is required by the editor anyway, so use it as the readiness probe.
	return hierarchy.open_file("scripts/common.j").has_value();
}

namespace {
fs::path browse(QWidget* parent) {
	const QString dir = QFileDialog::getExistingDirectory(
		parent, "Select Warcraft III Directory", QString(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	return dir.isEmpty() ? fs::path{} : fs::path(dir.toStdWString());
}
}

std::optional<fs::path> prompt(QWidget* parent, const std::vector<fs::path>& candidates,
							   bool allow_cancel, const QString& reason) {
	// Best suggestion: first candidate that looks like a real install, else the
	// first detected, else nothing (user must browse).
	fs::path suggested;
	for (const fs::path& candidate : candidates) {
		if (looks_like_warcraft_directory(candidate)) {
			suggested = candidate;
			break;
		}
	}
	if (suggested.empty() && !candidates.empty()) {
		suggested = candidates.front();
	}

	while (true) {
		fs::path pick;

		if (!suggested.empty()) {
			QMessageBox box(parent);
			box.setIcon(QMessageBox::Question);
			box.setWindowTitle("Warcraft III folder");
			box.setText(reason);
			box.setInformativeText(
				QString("Detected installation:\n%1\n\nVersion: %2\n\nUse this folder, or choose another?")
					.arg(QString::fromStdString(suggested.string()),
						 QString::fromStdString(version_label(suggested))));

			QPushButton* use_button = box.addButton("Use this folder", QMessageBox::AcceptRole);
			QPushButton* browse_button = box.addButton("Choose another…", QMessageBox::ActionRole);
			QPushButton* cancel_button = allow_cancel ? box.addButton(QMessageBox::Cancel) : nullptr;
			box.setDefaultButton(use_button);
			box.exec();

			if (cancel_button && box.clickedButton() == cancel_button) {
				return std::nullopt;
			}
			if (box.clickedButton() == use_button) {
				pick = suggested;
			} else {
				pick = browse(parent);
				if (pick.empty()) {
					continue; // back to the suggestion
				}
			}
		} else {
			QMessageBox::information(
				parent, "Warcraft III folder",
				reason + "\n\nNo installation was auto-detected. Please select your Warcraft III folder.");
			pick = browse(parent);
			if (pick.empty()) {
				// Nothing detected and nothing chosen: let the caller decide what
				// to do (exit at startup, keep old folder when re-picking).
				return std::nullopt;
			}
		}

		if (validate_and_open(pick)) {
			return pick;
		}

		QMessageBox::warning(
			parent, "Warcraft III folder",
			QString("This folder does not contain a readable Warcraft III installation:\n%1\n\nVersion: %2\n\n"
					"It may be an old or incomplete install. Please choose another folder.")
				.arg(QString::fromStdString(pick.string()),
					 QString::fromStdString(version_label(pick))));
		// Keep showing what they just tried (with its version) as the suggestion.
		suggested = pick;
	}
}

}
