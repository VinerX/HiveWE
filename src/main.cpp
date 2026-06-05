#define MI_MALLOC_OVERRIDE
#include <mimalloc.h>

#define QT_NO_OPENGL

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QSurfaceFormat>
#include <QSettings>
#include <QStyleFactory>

#include "main_window/hivewe.h"
#include "DockManager.h"

#ifdef WIN32
// To force HiveWE to run on the discrete GPU if available
extern "C" {
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
}
#endif

#include <tracy/Tracy.hpp>

import std;
import Map;
import Timer;
import MapGlobal;
import Globals;
import Utilities;
import Hierarchy;
import BinaryReader;
import GLThreadPool;
import WindowHandler;
import "object_editor/object_editor.h";
namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
	ZoneScopedN("main");

	Timer start_timer;

	// Diagnostic logging: startup failures are otherwise lost because stdout is
	// buffered and an unhandled exception aborts the process (0xC0000409) before
	// the buffer is flushed. Write to a flushed log file next to the exe.
	const auto log_line = [](const std::string& msg) {
		std::ofstream log("hivewe.log", std::ios::app);
		log << msg << "\n";
		log.flush();
		std::println("{}", msg);
	};

	std::set_terminate([] {
		std::string msg = "[FATAL] std::terminate called (no active exception)";
		if (std::current_exception()) {
			try {
				std::rethrow_exception(std::current_exception());
			} catch (const std::exception& ex) {
				msg = std::string("[FATAL] Unhandled exception: ") + ex.what();
			} catch (...) {
				msg = "[FATAL] Unhandled non-std exception";
			}
		}
		std::ofstream log("hivewe.log", std::ios::app);
		log << msg << "\n";
		log.flush();
		std::println("{}", msg);
		std::abort();
	});

	QSurfaceFormat format;
	format.setDepthBufferSize(24);
	format.setStencilBufferSize(8);
	format.setVersion(4, 5);
	format.setProfile(QSurfaceFormat::CoreProfile);
	format.setOption(QSurfaceFormat::DebugContext);
	format.setSwapInterval(1);
	//format.setColorSpace(QSurfaceFormat::sRGBColorSpace);
	QSurfaceFormat::setDefaultFormat(format);

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QCoreApplication::setOrganizationName("HiveWE");
	QCoreApplication::setApplicationName("HiveWE");

	QLocale::setDefault(QLocale("en_US"));

	// Create a dark palette
	// For some magically unknown reason Qt draws Qt::white text as black, so we use QColor(255, 254, 255) instead
	QPalette darkPalette;
	darkPalette.setColor(QPalette::Window, QColor(53, 53, 53));
	darkPalette.setColor(QPalette::WindowText, QColor(255, 254, 255));
	darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
	darkPalette.setColor(QPalette::Base, QColor(42, 42, 42));
	darkPalette.setColor(QPalette::AlternateBase, QColor(66, 66, 66));
	darkPalette.setColor(QPalette::ToolTipBase, QColor(66, 66, 66));
	darkPalette.setColor(QPalette::ToolTipText, QColor(255, 254, 255));
	darkPalette.setColor(QPalette::Text, QColor(255, 254, 255));
	darkPalette.setColor(QPalette::PlaceholderText, Qt::gray);
	darkPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
	darkPalette.setColor(QPalette::Dark, QColor(35, 35, 35));
	darkPalette.setColor(QPalette::Shadow, QColor(20, 20, 20));
	darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
	darkPalette.setColor(QPalette::ButtonText, QColor(255, 254, 255));
	darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
	darkPalette.setColor(QPalette::BrightText, Qt::red);
	darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
	darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
	darkPalette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
	darkPalette.setColor(QPalette::HighlightedText, QColor(255, 254, 255));
	darkPalette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 127, 127));

	QApplication::setPalette(darkPalette);
	QApplication::setStyle("Fusion");

	QApplication a(argc, argv);

	ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting);
	ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton);
	ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaDynamicTabsMenuButtonVisibility);
	ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize);
	ads::CDockManager::setConfigFlag(ads::CDockManager::MiddleMouseButtonClosesTab);

	QSettings settings;
	QFile file("data/themes/" + settings.value("theme", "Dark").toString() + ".qss");
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning() << "Error: Reading theme failed:" << file.error() << ": " << file.errorString();
		return -1;
	}

	a.setStyleSheet(QLatin1String(file.readAll()));

	const auto load_files = [&] {
		// Open a file that must exist in the game's CASC. Logs the exact path when
		// missing so startup failures point at the culprit instead of crashing blind.
		const auto open_required = [&](const std::string& path) -> BinaryReader {
			auto file = hierarchy.open_file(path);
			if (!file.has_value()) {
				log_line("[ERROR] Required file not found in Warcraft III data: " + path);
				throw std::runtime_error("Missing required game file: " + path);
			}
			return std::move(file.value());
		};

		// Place common.j and blizzard.j in the data folder. Required by JassHelper
		BinaryReader common = open_required("scripts/common.j");
		std::ofstream output("data/tools/common.j");
		output.write(reinterpret_cast<char*>(common.buffer.data()), common.buffer.size());
		BinaryReader blizzard = open_required("scripts/blizzard.j");
		std::ofstream output2("data/tools/blizzard.j");
		output2.write(reinterpret_cast<char*>(blizzard.buffer.data()), blizzard.buffer.size());

		log_line("[INFO] Loading WorldEdit string/data files...");
		world_edit_strings.load("UI/WorldEditStrings.txt");
		world_edit_game_strings.load("UI/WorldEditGameStrings.txt");
		world_edit_data.load("UI/WorldEditData.txt");

		world_edit_data.substitute(world_edit_game_strings, "WorldEditStrings");
		world_edit_data.substitute(world_edit_strings, "WorldEditStrings");
	};

	// Hierarchy used to read these from QSettings/registry itself; that pulled Qt into the
	// Qt-free data layer, so the GUI now configures the flavour flags before opening CASC.
	hierarchy.ptr = settings.value("flavour", "Retail").toString() == "PTR";
	hierarchy.hd = settings.value("hd", "False").toString() == "True";
	hierarchy.teen = settings.value("teen", "False").toString() == "True";
	{
		QSettings war3reg("HKEY_CURRENT_USER\\Software\\Blizzard Entertainment\\Warcraft III", QSettings::NativeFormat);
		hierarchy.local_files = war3reg.value("Allow Local Files", 0).toInt() != 0;
	}

	// Prefer the user-configured Warcraft directory (QSettings), falling back to
	// the Qt-free filesystem probe in Utilities. Kept here so Utilities stays
	// headless.
	const auto warcraft_directory = [&]() -> fs::path {
		if (settings.contains("warcraftDirectory")) {
			return settings.value("warcraftDirectory").toString().toStdString();
		}
		return find_warcraft_directory();
	};

	bool is_casc_open = false;
	const auto casc_future = std::async(std::launch::async, [&]() {
		const fs::path directory = warcraft_directory();

		is_casc_open = hierarchy.open_casc(directory);
		if (is_casc_open) {
			load_files();
		}
	});

	gl_thread_pool.init(8);

	casc_future.wait();

	if (!is_casc_open) {
		fs::path directory = warcraft_directory();

		while (!hierarchy.open_casc(directory)) {
			directory = QFileDialog::getExistingDirectory(nullptr, "Select Warcraft Directory", "/home", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks).toStdWString();
			if (directory == "") {
				exit(EXIT_SUCCESS);
			}
		}
		settings.setValue("warcraftDirectory", QString::fromStdString(directory.string()));

		load_files();
	}

	log_line("[INFO] CASC opened, constructing main window...");
	HiveWE w;

	log_line(std::format("[INFO] Application start: {}ms", start_timer.elapsed_ms()));

	std::optional<fs::path> startup_map_path;
	if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
		startup_map_path = fs::path(argv[1]);
	}
	if (!startup_map_path) {
		const QByteArray env_map = qgetenv("HIVEWE_STARTUP_MAP");
		if (!env_map.isEmpty()) {
			startup_map_path = fs::path(env_map.toStdString());
		}
	}
	if (!startup_map_path) {
		const fs::path test_map = "data/test map/";
		if (fs::is_directory(test_map) && fs::is_regular_file(test_map / "war3map.w3i")) {
			startup_map_path = test_map;
		}
	}

	try {
		if (startup_map_path) {
			log_line("[INFO] Startup map: " + startup_map_path->string());
			map->load(*startup_map_path);
		}
	} catch (const std::exception& ex) {
		log_line(std::string("[ERROR] Failed to load startup map: ") + ex.what());
	}

	try {
		bool created = false;
		window_handler.create_or_raise<ObjectEditor>(nullptr, created);
	} catch (const std::exception& ex) {
		log_line(std::string("[ERROR] Failed to open ObjectEditor: ") + ex.what());
	}
	// map->load("C:/Users/User/Desktop/MCFC.w3x");

	return QApplication::exec();
}
