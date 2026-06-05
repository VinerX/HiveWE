cmake_minimum_required(VERSION 3.29)

# Hack because vcpkg doesn't allow you to just declare that you want to be static on all platforms
# Linux and MacOS are static by default
# if(WIN32)
#    SET(VCPKG_TARGET_TRIPLET "x64-windows-static")
# endif()

project (HiveWE CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets REQUIRED)
find_package(Bullet CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(soil2 CONFIG REQUIRED)
find_package(stormlib CONFIG REQUIRED)
find_package(BZip2 REQUIRED) # Required by Stormlib
find_package(CascLib CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(outcome CONFIG REQUIRED)
find_package(unordered_dense CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(unofficial-qscintilla CONFIG REQUIRED)
find_package(tracy CONFIG REQUIRED)

qt_standard_project_setup()

option(HIVEWE_BUILD_TESTS "Build HiveWE unit tests" ON)

add_library(HiveWE_lib STATIC)
add_executable(HiveWE
	${QT_RESOURCES}
)

add_subdirectory(src)

target_include_directories(HiveWE_lib PUBLIC
	"src"
	"src/base"
	"src/brush"
	"src/custom_widgets"
	"src/file_formats"
	"src/main_window"
	"src/menus"
	"src/models"
	"src/model_editor"
	"src/object_editor"
	"src/trigger_editor"
	"src/resources"
	"src/resources/skinned_mesh"
	"src/utilities"

	${BULLET_INCLUDE_DIR}
)

target_link_libraries(HiveWE_lib PUBLIC
	mimalloc
	Qt6::Gui
	Qt6::Widgets
	Qt6::OpenGL
	Qt6::OpenGLWidgets
	glm::glm
	glad::glad
	soil2::soil2
	StormLib::storm
	BZip2::BZip2
	libjpeg-turbo::jpeg
	absl::base
	libjpeg-turbo::turbojpeg
	ads::qtadvanceddocking-qt6
	TBB::tbb
	imgui::imgui
	outcome::hl
	unordered_dense::unordered_dense
	nlohmann_json::nlohmann_json
	unofficial::qscintilla::qscintilla
	${BULLET_LIBRARIES}
)

target_compile_options(HiveWE_lib PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus /MP /sdl /diagnostics:caret /permissive->
	$<$<CXX_COMPILER_ID:Clang>:-Wextra -Wpedantic -Werror -Wno-multichar>
	$<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror -Wno-multichar>
)

target_compile_definitions(HiveWE_lib PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:UNICODE _UNICODE>
	$<$<CXX_COMPILER_ID:Clang>:TBB_SUPPRESS_DEPRECATED_MESSAGES>
	$<$<CXX_COMPILER_ID:GNU>:TBB_SUPPRESS_DEPRECATED_MESSAGES>
)

if(TRACY_ENABLE)
	target_link_libraries(HiveWE_lib PUBLIC Tracy::TracyClient)
	target_compile_definitions(HiveWE_lib PUBLIC TRACY_ENABLE)
else()
	# Headers must still resolve so `#include <tracy/Tracy.hpp>` (with no-op
	# macros) compiles
	target_include_directories(HiveWE_lib PUBLIC
		$<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

if(TRACY_NO_EXIT)
	target_compile_definitions(HiveWE_lib PUBLIC TRACY_NO_EXIT)
endif()

if(WIN32)
	target_link_libraries(HiveWE_lib PUBLIC CascLib::casc)
else()
	target_link_libraries(HiveWE_lib PUBLIC CascLib::casc_static)
endif()

target_compile_features(HiveWE_lib PUBLIC cxx_std_23)

# AUTOUIC generates ui_*.h into HiveWE_lib's private autogen, need to expose that to consumers
target_include_directories(HiveWE_lib PUBLIC
	"$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,${CMAKE_CURRENT_BINARY_DIR}/HiveWE_lib_autogen/include_$<CONFIG>,${CMAKE_CURRENT_BINARY_DIR}/HiveWE_lib_autogen/include>"
)

target_link_libraries(HiveWE PRIVATE HiveWE_lib)

# Headless data core: the Qt-free subset of modules needed to load and edit
# object data (SLK + modification tables) without any Qt/OpenGL. Built as a
# separate static lib (its own module BMIs) so the CLI can link it and stay
# Qt-free. These same .ixx files are also compiled into HiveWE_lib; that
# duplicate compilation is intentional and harmless (distinct targets, distinct
# BMIs).
add_library(HiveWE_core STATIC)
target_sources(HiveWE_core PUBLIC FILE_SET core_modules TYPE CXX_MODULES FILES
	"src/types.ixx"
	"src/globals.ixx"
	"src/utilities/no_init_allocator.ixx"
	"src/utilities/unordered_map.ixx"
	"src/utilities/utilities.ixx"
	"src/base/binary_reader.ixx"
	"src/base/binary_writer.ixx"
	"src/file_formats/casc.ixx"
	"src/file_formats/json.ixx"
	"src/base/hierarchy.ixx"
	"src/file_formats/ini.ixx"
	"src/file_formats/slk.ixx"
	"src/utilities/modification_tables.ixx"
	"src/base/object_data.ixx"
)
target_include_directories(HiveWE_core PUBLIC
	"src"
	"src/base"
	"src/file_formats"
	"src/utilities"
)
target_link_libraries(HiveWE_core PUBLIC
	glm::glm
	absl::base
	BZip2::BZip2
	StormLib::storm
	nlohmann_json::nlohmann_json
	unordered_dense::unordered_dense
)
if(WIN32)
	target_link_libraries(HiveWE_core PUBLIC CascLib::casc)
else()
	target_link_libraries(HiveWE_core PUBLIC CascLib::casc_static)
endif()
target_compile_features(HiveWE_core PUBLIC cxx_std_23)
target_compile_definitions(HiveWE_core PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:UNICODE _UNICODE>
)
target_compile_options(HiveWE_core PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus /permissive->
)

# Command-line companion for AI-agent driven map work. Phase 1 (build/run/validate)
# is self-contained (StormLib + nlohmann/json). Object-data commands link the
# headless HiveWE_core so the CLI can load/edit SLK data without Qt.
add_executable(HiveWE_cli "src/cli/main_cli.cpp")
# object_data_cli is a module unit (it imports HiveWE_core's modules); it must
# live in a CXX_MODULES file set so its BMI is built before main_cli imports it.
target_sources(HiveWE_cli PRIVATE FILE_SET cli_modules TYPE CXX_MODULES FILES
	"src/cli/object_data_cli.cpp"
)
target_link_libraries(HiveWE_cli PRIVATE
	HiveWE_core
	StormLib::storm
	BZip2::BZip2
	nlohmann_json::nlohmann_json
)
target_compile_features(HiveWE_cli PRIVATE cxx_std_23)
# StormLib's TCHAR APIs follow UNICODE; define it so they take wchar_t paths
# (matching std::filesystem::path::c_str() on Windows). __STORMLIB_NO_STATIC_LINK__
# disables StormLib.h's #pragma comment(lib) auto-linking (which would request a
# variant-named .lib like StormLibRUD that vcpkg doesn't ship); the StormLib::storm
# imported target supplies the correct library instead.
target_compile_definitions(HiveWE_cli PRIVATE UNICODE _UNICODE __STORMLIB_NO_STATIC_LINK__)
# Expose data/ (pjass.exe, luac.exe, common.j, ...) next to the CLI exe so
# validate-script can find its tools via the executable directory.
add_custom_command(TARGET HiveWE_cli POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E create_symlink
	"${PROJECT_SOURCE_DIR}/data"
	"$<TARGET_FILE_DIR:HiveWE_cli>/data"
)

if(HIVEWE_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND}
	-E create_symlink
	"${PROJECT_SOURCE_DIR}/data"
	"$<TARGET_FILE_DIR:HiveWE>/data"
)

# vcpkg's tracy port installs the Debug DLL under debug/bin/Debug/ (extra
# subfolder), which VCPKG_APPLOCAL_DEPS doesn't pick up. Copy it manually using
# the imported target's known location.
if(TRACY_ENABLE)
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:Tracy::TracyClient>"
		"$<TARGET_FILE_DIR:HiveWE>"
	)
endif()

if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
	set(QT_VCPKG_PATH "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows/debug")
else()
	set(QT_VCPKG_PATH "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows")
endif()

file(COPY "${QT_VCPKG_PATH}/Qt6/plugins/platforms/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/platforms")
file(COPY "${QT_VCPKG_PATH}/Qt6/plugins/styles/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/styles")
file(COPY "${QT_VCPKG_PATH}/Qt6/plugins/imageformats/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/imageformats")
