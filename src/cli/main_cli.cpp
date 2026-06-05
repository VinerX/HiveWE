// HiveWE_cli — command-line companion to HiveWE for AI-agent driven map work.
//
// Current scope: build / run / validate for unpacked map folders.
// The next slices (object-data / triggers / map-info) are planned separately.
//
// Output contract: every invocation prints exactly one JSON object to stdout so
// an agent can parse it. Success -> {"ok": true, ...}; failure -> {"ok": false,
// "error": "..."}. The process exit code mirrors ok (0 / 1).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <StormLib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- output ---------------------------------------------------------------

[[noreturn]] void emit(const json& result) {
	const bool ok = result.value("ok", false);
	std::fputs(result.dump(2).c_str(), stdout);
	std::fputc('\n', stdout);
	std::exit(ok ? 0 : 1);
}

[[noreturn]] void fail(const std::string& message, json extra = json::object()) {
	extra["ok"] = false;
	extra["error"] = message;
	emit(extra);
}

// ---- argument parsing -----------------------------------------------------

// Minimal "--key value" / "--flag" parser. Positional[0] is the subcommand.
struct Args {
	std::string command;
	std::unordered_map<std::string, std::string> options;
	std::vector<std::string> flags;

	bool has_flag(const std::string& name) const {
		for (const auto& f : flags) {
			if (f == name) {
				return true;
			}
		}
		return false;
	}
	std::optional<std::string> get(const std::string& name) const {
		const auto it = options.find(name);
		if (it == options.end()) {
			return std::nullopt;
		}
		return it->second;
	}
	std::string require(const std::string& name) const {
		const auto value = get(name);
		if (!value) {
			fail("missing required option: --" + name);
		}
		return *value;
	}
};

Args parse_args(int argc, char* argv[]) {
	Args args;
	if (argc >= 2) {
		args.command = argv[1];
	}
	for (int i = 2; i < argc; ++i) {
		std::string token = argv[i];
		if (token.rfind("--", 0) != 0) {
			continue; // ignore stray positionals for now
		}
		const std::string key = token.substr(2);
		// A following non-"--" token is this option's value; otherwise it's a flag.
		if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
			args.options[key] = argv[++i];
		} else {
			args.flags.push_back(key);
		}
	}
	return args;
}

// ---- process helpers ------------------------------------------------------

fs::path executable_dir() {
	std::wstring buffer(MAX_PATH, L'\0');
	const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	buffer.resize(len);
	return fs::path(buffer).parent_path();
}

// Run a program, wait, and capture combined stdout/stderr. Returns exit code,
// or -1 if the process could not be started.
int run_capture(const std::wstring& command_line, const fs::path& working_dir, std::string& output) {
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE read_pipe = nullptr;
	HANDLE write_pipe = nullptr;
	if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
		return -1;
	}
	SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = write_pipe;
	si.hStdError = write_pipe;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};
	std::wstring mutable_cmd = command_line; // CreateProcessW may modify the buffer
	const wchar_t* cwd = working_dir.empty() ? nullptr : working_dir.c_str();
	const BOOL started = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi);
	CloseHandle(write_pipe);
	if (!started) {
		CloseHandle(read_pipe);
		return -1;
	}

	char buffer[4096];
	DWORD read = 0;
	while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
		output.append(buffer, read);
	}
	CloseHandle(read_pipe);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 0;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return static_cast<int>(exit_code);
}

// Launch a program detached (do not wait). Returns the new PID, or 0 on failure.
DWORD launch_detached(const std::wstring& command_line, const fs::path& working_dir) {
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	std::wstring mutable_cmd = command_line;
	const wchar_t* cwd = working_dir.empty() ? nullptr : working_dir.c_str();
	const BOOL started = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi);
	if (!started) {
		return 0;
	}
	const DWORD pid = pi.dwProcessId;
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return pid;
}

std::wstring quote(const std::wstring& s) {
	return L"\"" + s + L"\"";
}

fs::path require_map_dir(const Args& args) {
	const fs::path map_dir = fs::path(args.require("map"));
	if (!fs::is_directory(map_dir)) {
		fail("map folder not found: " + map_dir.string());
	}
	if (!fs::is_regular_file(map_dir / "war3map.w3i")) {
		fail("not a valid map folder (missing war3map.w3i): " + map_dir.string());
	}
	return map_dir;
}

// ---- commands -------------------------------------------------------------

// Package a map folder into a .w3x MPQ archive. Mirrors HiveWE::export_mpq.
void cmd_build_map(const Args& args) {
	const fs::path map_dir = require_map_dir(args);

	const auto out_opt = args.get("out");
	if (!out_opt) {
		// No output requested: just report that the folder is buildable/runnable.
		const bool has_jass = fs::is_regular_file(map_dir / "war3map.j");
		const bool has_lua = fs::is_regular_file(map_dir / "war3map.lua");
		emit({{"ok", true},
			  {"command", "build-map"},
			  {"map", map_dir.string()},
			  {"packaged", false},
			  {"script", has_lua ? "lua" : (has_jass ? "jass" : "none")},
			  {"note", "folder is valid; pass --out <file.w3x> to package"}});
	}

	const fs::path out_path = fs::path(*out_opt);
	std::error_code ec;
	fs::remove(out_path, ec);
	if (out_path.has_parent_path()) {
		fs::create_directories(out_path.parent_path(), ec);
	}

	// Count files first; StormLib wants an upper bound on the hash table size.
	uint64_t file_count = 0;
	for (const auto& entry : fs::recursive_directory_iterator(map_dir)) {
		if (entry.is_regular_file()) {
			++file_count;
		}
	}
	if (file_count == 0) {
		fail("map folder is empty: " + map_dir.string());
	}

	HANDLE archive = nullptr;
	if (!SFileCreateArchive(out_path.c_str(), MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES,
							static_cast<DWORD>(file_count), &archive)) {
		fail("failed to create archive (StormLib error " + std::to_string(GetLastError()) + "): " + out_path.string());
	}

	uint64_t added = 0;
	std::vector<std::string> failures;
	for (const auto& entry : fs::recursive_directory_iterator(map_dir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const std::string archived_name = entry.path().lexically_relative(map_dir).string();
		const bool ok = SFileAddFileEx(archive, entry.path().c_str(), archived_name.c_str(),
									   MPQ_FILE_COMPRESS, MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_NEXT_SAME);
		if (ok) {
			++added;
		} else {
			failures.push_back(archived_name + " (error " + std::to_string(GetLastError()) + ")");
		}
	}
	SFileCompactArchive(archive, nullptr, false);
	SFileCloseArchive(archive);

	const uint64_t size = fs::exists(out_path) ? fs::file_size(out_path) : 0;
	emit({{"ok", failures.empty()},
		  {"command", "build-map"},
		  {"map", map_dir.string()},
		  {"out", out_path.string()},
		  {"packaged", true},
		  {"files_added", added},
		  {"files_total", file_count},
		  {"size_bytes", size},
		  {"failures", failures}});
}

// Launch Warcraft III with the given map. Mirrors HiveWE::play_test.
void cmd_run_map(const Args& args) {
	const fs::path map_path = fs::path(args.require("map"));
	const fs::path warcraft = fs::path(args.require("warcraft"));
	if (!fs::exists(map_path)) {
		fail("map path not found: " + map_path.string());
	}

	const bool ptr = args.has_flag("ptr");
	const fs::path root = warcraft / (ptr ? "_ptr_" : "_retail_");
	const fs::path exe = root / "x86_64" / "Warcraft III.exe";
	if (!fs::is_regular_file(exe)) {
		fail("Warcraft III.exe not found: " + exe.string());
	}

	std::wstring command_line = quote(exe.wstring());
	command_line += L" -launch -loadfile " + quote(fs::absolute(map_path).wstring());
	if (const auto extra = args.get("args")) {
		command_line += L" " + fs::path(*extra).wstring();
	}

	const DWORD pid = launch_detached(command_line, exe.parent_path());
	if (pid == 0) {
		fail("failed to launch Warcraft III (error " + std::to_string(GetLastError()) + ")");
	}
	emit({{"ok", true},
		  {"command", "run-map"},
		  {"map", map_path.string()},
		  {"warcraft_exe", exe.string()},
		  {"pid", pid}});
}

// Static-validate the map script. JASS -> pjass; Lua -> luac -p (if available).
void cmd_validate_script(const Args& args) {
	const fs::path map_dir = require_map_dir(args);
	const fs::path tools = args.get("tools") ? fs::path(*args.get("tools"))
											 : executable_dir() / "data" / "tools";

	const fs::path lua = map_dir / "war3map.lua";
	const fs::path jass = map_dir / "war3map.j";

	if (fs::is_regular_file(lua)) {
		const fs::path luac = tools / "luac.exe";
		if (!fs::is_regular_file(luac)) {
			emit({{"ok", true},
				  {"command", "validate-script"},
				  {"language", "lua"},
				  {"validated", false},
				  {"note", "luac.exe not found in tools dir; static Lua check skipped. Looked at: " + luac.string()}});
		}
		std::string output;
		const std::wstring cmd = quote(luac.wstring()) + L" -p " + quote(lua.wstring());
		const int code = run_capture(cmd, tools, output);
		emit({{"ok", code == 0},
			  {"command", "validate-script"},
			  {"language", "lua"},
			  {"validated", true},
			  {"exit_code", code},
			  {"output", output}});
	}

	if (fs::is_regular_file(jass)) {
		const fs::path pjass = tools / "pjass.exe";
		const fs::path common = tools / "common.j";
		const fs::path blizzard = tools / "blizzard.j";
		if (!fs::is_regular_file(pjass) || !fs::is_regular_file(common) || !fs::is_regular_file(blizzard)) {
			emit({{"ok", true},
				  {"command", "validate-script"},
				  {"language", "jass"},
				  {"validated", false},
				  {"note", "pjass.exe / common.j / blizzard.j missing in tools dir; JASS check skipped (run HiveWE once to extract common.j & blizzard.j)"}});
		}
		std::string output;
		const std::wstring cmd = quote(pjass.wstring()) + L" " + quote(common.wstring()) + L" " +
								 quote(blizzard.wstring()) + L" " + quote(jass.wstring());
		const int code = run_capture(cmd, tools, output);
		const bool ok = output.find("Parse successful") != std::string::npos || (code == 0 && output.find("error") == std::string::npos);
		emit({{"ok", ok},
			  {"command", "validate-script"},
			  {"language", "jass"},
			  {"validated", true},
			  {"exit_code", code},
			  {"output", output}});
	}

	fail("no map script found (war3map.lua / war3map.j) in: " + map_dir.string());
}

} // namespace

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);

	if (args.command.empty() || args.command == "help" || args.has_flag("help")) {
		emit({{"ok", true},
			  {"tool", "HiveWE_cli"},
			  {"commands", json::array({"build-map", "run-map", "validate-script"})},
			  {"usage", json::object({
				   {"build-map", "--map <dir> [--out <file.w3x>]"},
				   {"run-map", "--map <dir|.w3x> --warcraft <dir> [--ptr] [--args \"...\"]"},
				   {"validate-script", "--map <dir> [--tools <dir>]"},
			   })}});
	}

	if (args.command == "build-map") {
		cmd_build_map(args);
	} else if (args.command == "run-map") {
		cmd_run_map(args);
	} else if (args.command == "validate-script") {
		cmd_validate_script(args);
	}

	fail("unknown command: " + args.command);
}
