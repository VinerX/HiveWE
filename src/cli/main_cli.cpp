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
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <StormLib.h>
#include <nlohmann/json.hpp>

// Object-data commands live in the ObjectDataCli module (which imports the
// Qt-free HiveWE_core modules). hivewe_object_command() returns a JSON string and
// sets `ok` for the exit code; `warcraft_fallback` is used when --warcraft is not
// passed.
import ObjectDataCli;

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

std::string wstring_to_utf8(const std::wstring& text) {
	if (text.empty()) {
		return {};
	}
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0) {
		return {};
	}
	std::string out(static_cast<std::size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
	return out;
}

struct Utf8Args {
	std::vector<std::string> storage;
	std::vector<char*> argv;
};

Utf8Args windows_utf8_args() {
	Utf8Args converted;
	int wide_argc = 0;
	LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
	if (wide_argv == nullptr) {
		return converted;
	}
	converted.storage.reserve(static_cast<std::size_t>(wide_argc));
	converted.argv.reserve(static_cast<std::size_t>(wide_argc));
	for (int i = 0; i < wide_argc; ++i) {
		converted.storage.push_back(wstring_to_utf8(wide_argv[i]));
	}
	for (auto& arg : converted.storage) {
		converted.argv.push_back(arg.data());
	}
	LocalFree(wide_argv);
	return converted;
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

struct LaunchedProcess {
	HANDLE process = nullptr;
	HANDLE thread = nullptr;
	DWORD pid = 0;
};

struct CloseWindowContext {
	DWORD pid = 0;
	bool posted = false;
};

struct WindowLookupContext {
	DWORD pid = 0;
	HWND hwnd = nullptr;
};

struct TimedChatCommand {
	std::size_t after_seconds = 0;
	std::string text;
};

struct BridgeCommand {
	std::size_t after_seconds = 0;
	std::string op;
	std::string arg;
};

std::optional<LaunchedProcess> launch_process(const std::wstring& command_line, const fs::path& working_dir) {
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	std::wstring mutable_cmd = command_line;
	const wchar_t* cwd = working_dir.empty() ? nullptr : working_dir.c_str();
	const BOOL started = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi);
	if (!started) {
		return std::nullopt;
	}
	return LaunchedProcess{pi.hProcess, pi.hThread, pi.dwProcessId};
}

void close_process_handles(LaunchedProcess& proc) {
	if (proc.process) {
		CloseHandle(proc.process);
		proc.process = nullptr;
	}
	if (proc.thread) {
		CloseHandle(proc.thread);
		proc.thread = nullptr;
	}
}

BOOL CALLBACK close_windows_for_pid(HWND hwnd, LPARAM lparam) {
	auto* ctx = reinterpret_cast<CloseWindowContext*>(lparam);
	DWORD hwnd_pid = 0;
	GetWindowThreadProcessId(hwnd, &hwnd_pid);
	if (hwnd_pid != ctx->pid || !IsWindowVisible(hwnd)) {
		return TRUE;
	}
	PostMessageW(hwnd, WM_CLOSE, 0, 0);
	ctx->posted = true;
	return TRUE;
}

BOOL CALLBACK find_window_for_pid(HWND hwnd, LPARAM lparam) {
	auto* ctx = reinterpret_cast<WindowLookupContext*>(lparam);
	DWORD hwnd_pid = 0;
	GetWindowThreadProcessId(hwnd, &hwnd_pid);
	if (hwnd_pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
		return TRUE;
	}
	ctx->hwnd = hwnd;
	return FALSE;
}

HWND find_main_window(DWORD pid) {
	WindowLookupContext ctx{pid, nullptr};
	EnumWindows(find_window_for_pid, reinterpret_cast<LPARAM>(&ctx));
	return ctx.hwnd;
}

bool click_window_center(DWORD pid) {
	HWND hwnd = find_main_window(pid);
	if (hwnd == nullptr) {
		return false;
	}

	ShowWindow(hwnd, SW_RESTORE);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	SetFocus(hwnd);

	RECT rect{};
	if (!GetClientRect(hwnd, &rect)) {
		return false;
	}

	const int x = (rect.right - rect.left) / 2;
	const int y = (rect.bottom - rect.top) / 2;
	const LPARAM lparam = MAKELPARAM(x, y);

	PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam);
	PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam);
	PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam);
	return true;
}

bool send_enter_to_window(DWORD pid) {
	HWND hwnd = find_main_window(pid);
	if (hwnd == nullptr) {
		return false;
	}

	ShowWindow(hwnd, SW_RESTORE);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	SetFocus(hwnd);

	PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
	PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
	return true;
}

std::wstring utf8_to_wstring(const std::string& text) {
	if (text.empty()) {
		return {};
	}

	const int wide_size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (wide_size <= 0) {
		return {};
	}

	std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
	const int converted = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), wide_size);
	if (converted <= 0) {
		return {};
	}
	wide.resize(static_cast<std::size_t>(converted - 1));
	return wide;
}

bool focus_window(HWND hwnd) {
	if (hwnd == nullptr) {
		return false;
	}

	const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
	const DWORD my_thread = GetCurrentThreadId();
	AttachThreadInput(my_thread, target_thread, TRUE);

	ShowWindow(hwnd, SW_RESTORE);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	SetActiveWindow(hwnd);
	SetFocus(hwnd);

	AttachThreadInput(my_thread, target_thread, FALSE);
	return true;
}

bool send_virtual_key_input(WORD vk) {
	INPUT inputs[2]{};
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = vk;
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = vk;
	inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
	return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

bool send_unicode_text_input(const std::wstring& text) {
	if (text.empty()) {
		return true;
	}

	std::vector<INPUT> inputs;
	inputs.reserve(text.size() * 2);
	for (const wchar_t ch : text) {
		INPUT down{};
		down.type = INPUT_KEYBOARD;
		down.ki.wScan = ch;
		down.ki.dwFlags = KEYEVENTF_UNICODE;
		inputs.push_back(down);

		INPUT up{};
		up.type = INPUT_KEYBOARD;
		up.ki.wScan = ch;
		up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		inputs.push_back(up);
	}

	return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

bool set_clipboard_text(const std::wstring& text) {
	if (!OpenClipboard(nullptr)) {
		return false;
	}
	EmptyClipboard();
	const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem == nullptr) {
		CloseClipboard();
		return false;
	}
	wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
	if (dst == nullptr) {
		GlobalFree(mem);
		CloseClipboard();
		return false;
	}
	std::memcpy(dst, text.c_str(), bytes);
	GlobalUnlock(mem);
	SetClipboardData(CF_UNICODETEXT, mem);
	CloseClipboard();
	return true;
}

bool send_ctrl_v() {
	INPUT inputs[4]{};
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = VK_CONTROL;
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = 'V';
	inputs[2].type = INPUT_KEYBOARD;
	inputs[2].ki.wVk = 'V';
	inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
	inputs[3].type = INPUT_KEYBOARD;
	inputs[3].ki.wVk = VK_CONTROL;
	inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
	return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

HKL force_english_layout(HWND hwnd) {
	HKL english = LoadKeyboardLayoutW(L"00000409", 0);
	HKL previous = reinterpret_cast<HKL>(SendMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(english)));
	PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(english));
	Sleep(50);
	ActivateKeyboardLayout(english, 0);
	return previous;
}

void restore_layout(HKL layout) {
	if (layout != nullptr) {
		ActivateKeyboardLayout(layout, 0);
	}
}

bool send_chat_to_window(DWORD pid, const std::string& text) {
	HWND hwnd = find_main_window(pid);
	if (hwnd == nullptr) {
		return false;
	}

	const std::wstring wide = utf8_to_wstring(text);
	if (!text.empty() && wide.empty()) {
		return false;
	}

	focus_window(hwnd);
	HKL prev_layout = force_english_layout(hwnd);
	Sleep(100);

	send_virtual_key_input(VK_RETURN);
	Sleep(150);
	send_unicode_text_input(wide);
	Sleep(100);
	send_virtual_key_input(VK_RETURN);

	Sleep(50);
	restore_layout(prev_layout);
	return true;
}

// UNSTABLE: chat bridge requires EN keyboard layout and window focus.
// SendInput unreliable with fullscreen DirectX games. Needs further work.
// Preloader bridge (via .pld files) is the primary stable path.
bool send_chat_all_methods(DWORD pid, const std::string& text) {
	HWND hwnd = find_main_window(pid);
	if (hwnd == nullptr) {
		return false;
	}

	const std::wstring wide = utf8_to_wstring(text);
	if (!text.empty() && wide.empty()) {
		return false;
	}

	// Method A: force EN layout + SendInput UNICODE
	focus_window(hwnd);
	HKL prev = force_english_layout(hwnd);
	Sleep(100);
	send_virtual_key_input(VK_RETURN);
	Sleep(150);
	send_unicode_text_input(wide);
	Sleep(100);
	send_virtual_key_input(VK_RETURN);
	restore_layout(prev);

	Sleep(3000);

	// Method B: force EN layout + PostMessage WM_CHAR
	focus_window(hwnd);
	prev = force_english_layout(hwnd);
	Sleep(100);
	PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
	PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
	Sleep(150);
	for (const wchar_t ch : wide) {
		PostMessageW(hwnd, WM_CHAR, ch, 1);
	}
	Sleep(100);
	PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
	PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
	restore_layout(prev);

	Sleep(3000);

	// Method C: clipboard + Ctrl+V
	focus_window(hwnd);
	set_clipboard_text(wide);
	Sleep(100);
	send_virtual_key_input(VK_RETURN);
	Sleep(150);
	send_ctrl_v();
	Sleep(100);
	send_virtual_key_input(VK_RETURN);

	return true;
}

bool request_process_close(DWORD pid) {
	CloseWindowContext ctx{pid, false};
	EnumWindows(close_windows_for_pid, reinterpret_cast<LPARAM>(&ctx));
	return ctx.posted;
}

// Launch a program detached (do not wait). Returns the new PID, or 0 on failure.
DWORD launch_detached(const std::wstring& command_line, const fs::path& working_dir) {
	auto proc = launch_process(command_line, working_dir);
	if (!proc) {
		return 0;
	}
	const DWORD pid = proc->pid;
	close_process_handles(*proc);
	return pid;
}

std::wstring quote(const std::wstring& s) {
	return L"\"" + s + L"\"";
}

fs::path war3_log_path(const Args& args) {
	if (const auto log = args.get("log")) {
		return fs::path(*log);
	}
	if (const char* userprofile = std::getenv("USERPROFILE")) {
		return fs::path(userprofile) / "Documents" / "Warcraft III" / "Logs" / "War3Log.txt";
	}
	fail("USERPROFILE is not set; pass --log <path-to-War3Log.txt>");
}

fs::path war3_custom_map_data_dir() {
	if (const char* userprofile = std::getenv("USERPROFILE")) {
		return fs::path(userprofile) / "Documents" / "Warcraft III" / "CustomMapData";
	}
	fail("USERPROFILE is not set; pass an absolute --file path");
}

fs::path custom_map_data_file_path(const std::string& file_option) {
	const fs::path path = fs::path(file_option);
	if (path.is_absolute()) {
		return path;
	}
	return war3_custom_map_data_dir() / path;
}

std::string read_text_file(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return {};
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> split_lines(const std::string& text) {
	std::vector<std::string> lines;
	std::string current;
	for (char c : text) {
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			lines.push_back(current);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	if (!current.empty()) {
		lines.push_back(current);
	}
	return lines;
}

std::vector<std::string> extract_preload_messages(const std::string& text) {
	const std::vector<std::string> lines = split_lines(text);
	std::vector<std::string> messages;
	const std::string prefix = "call Preload( \"";
	for (const std::string& line : lines) {
		const std::size_t start = line.find(prefix);
		if (start == std::string::npos) {
			continue;
		}

		std::size_t end = line.size();
		while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
			--end;
		}
		if (end == 0) {
			continue;
		}

		const std::size_t quote_end = line.rfind('"', end - 1);
		const std::size_t content_start = start + prefix.size();
		if (quote_end == std::string::npos || quote_end < content_start) {
			continue;
		}

		messages.push_back(line.substr(content_start, quote_end - content_start));
	}
	return messages;
}

std::string join_lines(const std::vector<std::string>& lines, std::size_t start_index) {
	std::string joined;
	for (std::size_t i = start_index; i < lines.size(); ++i) {
		if (!joined.empty()) {
			joined += '\n';
		}
		joined += lines[i];
	}
	return joined;
}

std::string read_preload_log_excerpt(const fs::path& path, std::size_t tail_lines) {
	if (!fs::is_regular_file(path)) {
		return {};
	}
	const std::vector<std::string> messages = extract_preload_messages(read_text_file(path));
	const std::size_t start = messages.size() > tail_lines ? messages.size() - tail_lines : 0;
	return join_lines(messages, start);
}

std::string normalized_log_path(const fs::path& path) {
	return fs::absolute(path).generic_string();
}

std::string extract_log_excerpt(const fs::path& log_path, const fs::path& map_path, std::size_t fallback_tail_lines, bool& found_map_marker) {
	found_map_marker = false;
	const std::vector<std::string> lines = split_lines(read_text_file(log_path));
	if (lines.empty()) {
		return {};
	}

	const std::string marker = "Opening map - " + normalized_log_path(map_path);
	for (std::size_t i = lines.size(); i-- > 0;) {
		if (lines[i].find(marker) != std::string::npos) {
			found_map_marker = true;
			return join_lines(lines, i);
		}
	}

	const std::size_t start = lines.size() > fallback_tail_lines ? lines.size() - fallback_tail_lines : 0;
	return join_lines(lines, start);
}

std::size_t parse_seconds_option(const Args& args, const std::string& key, std::size_t default_value) {
	if (const auto raw = args.get(key)) {
		try {
			return static_cast<std::size_t>(std::stoul(*raw));
		} catch (...) {
			fail("invalid integer for --" + key + ": " + *raw);
		}
	}
	return default_value;
}

TimedChatCommand parse_timed_chat_entry(const std::string& raw_entry) {
	const std::size_t separator = raw_entry.find(':');
	if (separator == std::string::npos || separator == 0 || separator + 1 >= raw_entry.size()) {
		fail("invalid chat schedule entry: " + raw_entry + " (expected <seconds>:<text>)");
	}

	TimedChatCommand command;
	try {
		command.after_seconds = static_cast<std::size_t>(std::stoul(raw_entry.substr(0, separator)));
	} catch (...) {
		fail("invalid chat schedule time: " + raw_entry);
	}

	command.text = raw_entry.substr(separator + 1);
	if (command.text.empty()) {
		fail("empty chat text in entry: " + raw_entry);
	}
	return command;
}

std::vector<TimedChatCommand> parse_chat_schedule(const Args& args) {
	std::vector<TimedChatCommand> commands;

	const auto chat_after = args.get("chat-after");
	const auto chat_text = args.get("chat-text");
	if (chat_after || chat_text) {
		if (!chat_after || !chat_text) {
			fail("--chat-after and --chat-text must be used together");
		}
		commands.push_back(parse_timed_chat_entry(*chat_after + ":" + *chat_text));
	}

	if (const auto chat_script = args.get("chat-script")) {
		std::size_t start = 0;
		while (start < chat_script->size()) {
			const std::size_t separator = chat_script->find('|', start);
			const std::string entry = chat_script->substr(start, separator == std::string::npos ? std::string::npos : separator - start);
			if (!entry.empty()) {
				commands.push_back(parse_timed_chat_entry(entry));
			}
			if (separator == std::string::npos) {
				break;
			}
			start = separator + 1;
		}
	}

	std::sort(commands.begin(), commands.end(), [](const TimedChatCommand& a, const TimedChatCommand& b) {
		if (a.after_seconds != b.after_seconds) {
			return a.after_seconds < b.after_seconds;
		}
		return a.text < b.text;
	});
	return commands;
}

bool is_safe_bridge_token(const std::string& text) {
	if (text.empty()) {
		return false;
	}
	for (const char ch : text) {
		const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
						ch == '_' || ch == '-' || ch == '.';
		if (!ok) {
			return false;
		}
	}
	return true;
}

bool is_safe_bridge_arg(const std::string& text) {
	if (text.empty()) {
		return false;
	}
	for (const char ch : text) {
		const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
						ch == '_' || ch == '-' || ch == '.' || ch == ':';
		if (!ok) {
			return false;
		}
	}
	return true;
}

BridgeCommand parse_bridge_entry(const std::string& raw_entry) {
	const std::size_t first = raw_entry.find(':');
	const std::size_t second = first == std::string::npos ? std::string::npos : raw_entry.find(':', first + 1);
	if (first == std::string::npos || second == std::string::npos || first == 0 || second <= first + 1 || second + 1 >= raw_entry.size()) {
		fail("invalid bridge schedule entry: " + raw_entry + " (expected <seconds>:<op>:<arg>)");
	}

	BridgeCommand command;
	try {
		command.after_seconds = static_cast<std::size_t>(std::stoul(raw_entry.substr(0, first)));
	} catch (...) {
		fail("invalid bridge schedule time: " + raw_entry);
	}
	command.op = raw_entry.substr(first + 1, second - first - 1);
	command.arg = raw_entry.substr(second + 1);
	if (!is_safe_bridge_token(command.op) || !is_safe_bridge_arg(command.arg)) {
		fail("bridge command contains unsupported characters: " + raw_entry);
	}
	return command;
}

std::vector<BridgeCommand> parse_bridge_schedule(const Args& args) {
	std::vector<BridgeCommand> commands;
	if (const auto bridge_script = args.get("bridge-script")) {
		std::size_t start = 0;
		while (start < bridge_script->size()) {
			const std::size_t separator = bridge_script->find('|', start);
			const std::string entry = bridge_script->substr(start, separator == std::string::npos ? std::string::npos : separator - start);
			if (!entry.empty()) {
				commands.push_back(parse_bridge_entry(entry));
			}
			if (separator == std::string::npos) {
				break;
			}
			start = separator + 1;
		}
	}

	std::sort(commands.begin(), commands.end(), [](const BridgeCommand& a, const BridgeCommand& b) {
		if (a.after_seconds != b.after_seconds) {
			return a.after_seconds < b.after_seconds;
		}
		if (a.op != b.op) {
			return a.op < b.op;
		}
		return a.arg < b.arg;
	});
	return commands;
}

void write_text_file_utf8(const fs::path& path, const std::string& text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		fail("failed to write file: " + path.string());
	}
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!out.good()) {
		fail("failed to flush file: " + path.string());
	}
}

std::string build_bridge_preloader_file(const std::string& payload) {
	return "function PreloadFiles takes nothing returns nothing\n"
		   "\r\n"
		   "\tcall PreloadStart()\r\n"
		   "\tcall Preload( \"\")\n"
		   "call BlzSetAbilityTooltip('AHbz',\"" + payload + "\",0)\n"
		   "call Preload(\"\" )\r\n"
		   "\tcall PreloadEnd( 0.0 )\r\n"
		   "\n"
		   "endfunction\n\n\r\n";
}

void cleanup_bridge_files(const fs::path& custom_map_data_dir) {
	std::error_code ec;
	if (!fs::exists(custom_map_data_dir, ec)) {
		return;
	}
	for (const auto& entry : fs::directory_iterator(custom_map_data_dir, ec)) {
		if (ec || !entry.is_regular_file()) {
			continue;
		}
		const std::string name = entry.path().filename().string();
		if (name == "23race_cmd_manifest.pld" || (name.rfind("23race_cmd_", 0) == 0 && entry.path().extension() == ".pld")) {
			fs::remove(entry.path(), ec);
		}
	}
}

json prepare_bridge_command_files(const std::vector<BridgeCommand>& commands) {
	const fs::path custom_map_data_dir = war3_custom_map_data_dir();
	std::error_code ec;
	fs::create_directories(custom_map_data_dir, ec);
	cleanup_bridge_files(custom_map_data_dir);

	json result = json::array();
	if (commands.empty()) {
		return result;
	}

	const fs::path manifest_path = custom_map_data_dir / "23race_cmd_manifest.pld";
	write_text_file_utf8(manifest_path, build_bridge_preloader_file("manifest|" + std::to_string(commands.size())));

	for (std::size_t i = 0; i < commands.size(); ++i) {
		char filename[64];
		std::snprintf(filename, sizeof(filename), "23race_cmd_%04zu.pld", i + 1);
		const fs::path command_path = custom_map_data_dir / filename;
		const std::string payload = "cmd|" + std::to_string(i + 1) + "|" + std::to_string(commands[i].after_seconds) + "|" + commands[i].op + "|" + commands[i].arg;
		write_text_file_utf8(command_path, build_bridge_preloader_file(payload));
		result.push_back({
			{"sequence", i + 1},
			{"after_seconds", commands[i].after_seconds},
			{"op", commands[i].op},
			{"arg", commands[i].arg},
			{"file", command_path.string()},
		});
	}
	return result;
}

// Read the user-configured Warcraft directory that HiveWE stores via QSettings at
// HKCU\Software\HiveWE\HiveWE\warcraftDirectory (REG_SZ). Empty if unset.
std::string warcraft_dir_from_registry() {
	wchar_t buffer[1024];
	DWORD size = sizeof(buffer);
	const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\HiveWE\\HiveWE",
										L"warcraftDirectory", RRF_RT_REG_SZ, nullptr, buffer, &size);
	if (status != ERROR_SUCCESS) {
		return {};
	}
	return fs::path(std::wstring(buffer)).string();
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

// Launch Warcraft III, wait a bit, optionally terminate it, then return the
// relevant War3Log excerpt for this map. This closes the loop for automated
// "run -> inspect log -> patch" iterations.
void cmd_probe_map(const Args& args) {
	const fs::path map_path = fs::path(args.require("map"));
	const fs::path warcraft = fs::path(args.require("warcraft"));
	const fs::path log_path = war3_log_path(args);
	const auto probe_log_file = args.get("probe-log");
	const std::vector<BridgeCommand> bridge_schedule = parse_bridge_schedule(args);
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

	std::optional<fs::path> probe_log_path;
	if (probe_log_file) {
		probe_log_path = custom_map_data_file_path(*probe_log_file);
		std::error_code ec;
		fs::create_directories(probe_log_path->parent_path(), ec);
		fs::remove(*probe_log_path, ec);
	}
	const json bridge_files = prepare_bridge_command_files(bridge_schedule);

	auto proc = launch_process(command_line, exe.parent_path());
	if (!proc) {
		fail("failed to launch Warcraft III (error " + std::to_string(GetLastError()) + ")");
	}

	const std::size_t wait_seconds = parse_seconds_option(args, "wait", 12);
	const std::size_t tail_lines = parse_seconds_option(args, "tail", 120);
	const auto click_after_opt = args.get("click-after");
	const std::vector<TimedChatCommand> chat_schedule = parse_chat_schedule(args);
	const bool terminate_after_wait = !args.has_flag("keep-open");
	bool click_sent = false;
	bool click_window_found = false;
	bool enter_after_click_sent = false;
	bool close_confirm_enter_sent = false;
	std::vector<bool> chat_sent(chat_schedule.size(), false);
	std::vector<bool> bridge_sent(bridge_schedule.size(), !bridge_schedule.empty());
	DWORD wait_result = WAIT_TIMEOUT;

	if (click_after_opt) {
		const std::size_t click_after_seconds = parse_seconds_option(args, "click-after", 0);
		if (click_after_seconds >= wait_seconds) {
			fail("--click-after must be smaller than --wait");
		}
	}
	for (const auto& command : chat_schedule) {
		if (command.after_seconds >= wait_seconds) {
			fail("chat command scheduled outside probe window: " + std::to_string(command.after_seconds) + ":" + command.text);
		}
	}
	for (const auto& command : bridge_schedule) {
		if (command.after_seconds >= wait_seconds) {
			fail("bridge command scheduled outside probe window: " + std::to_string(command.after_seconds) + ":" + command.op + ":" + command.arg);
		}
	}

	const std::size_t click_after_seconds = click_after_opt ? parse_seconds_option(args, "click-after", 0) : 0;
	const ULONGLONG start_tick = GetTickCount64();
	std::size_t next_chat_index = 0;
	bool click_handled = false;

	while (true) {
		wait_result = WaitForSingleObject(proc->process, 100);
		if (wait_result == WAIT_OBJECT_0) {
			break;
		}

		const ULONGLONG elapsed_ms = GetTickCount64() - start_tick;
		const std::size_t elapsed_seconds = static_cast<std::size_t>(elapsed_ms / 1000);

		if (click_after_opt && !click_handled && elapsed_seconds >= click_after_seconds) {
			click_sent = click_window_center(proc->pid);
			click_window_found = find_main_window(proc->pid) != nullptr;
			if (click_sent) {
				Sleep(5000);
				send_enter_to_window(proc->pid);
				Sleep(5000);
				enter_after_click_sent = send_enter_to_window(proc->pid);
			}
			click_handled = true;
		}

		while (next_chat_index < chat_schedule.size() && elapsed_seconds >= chat_schedule[next_chat_index].after_seconds) {
			chat_sent[next_chat_index] = send_chat_to_window(proc->pid, chat_schedule[next_chat_index].text);
			++next_chat_index;
		}

		if (elapsed_ms >= static_cast<ULONGLONG>(wait_seconds) * 1000ULL) {
			break;
		}
	}
	const bool exited_within_wait = (wait_result == WAIT_OBJECT_0);
	bool close_requested = false;
	bool terminated = false;
	if (!exited_within_wait && terminate_after_wait) {
		close_requested = request_process_close(proc->pid);
		if (close_requested) {
			Sleep(150);
			close_confirm_enter_sent = send_enter_to_window(proc->pid);
			const DWORD close_wait = WaitForSingleObject(proc->process, 5000);
			terminated = (close_wait != WAIT_OBJECT_0) && (TerminateProcess(proc->process, 0) != FALSE);
		} else {
			terminated = TerminateProcess(proc->process, 0) != FALSE;
		}
		if (terminated) {
			WaitForSingleObject(proc->process, 5000);
		}
	}

	DWORD exit_code = STILL_ACTIVE;
	GetExitCodeProcess(proc->process, &exit_code);
	close_process_handles(*proc);

	bool found_map_marker = false;
	const std::string excerpt = extract_log_excerpt(log_path, map_path, tail_lines, found_map_marker);
	const bool has_error = excerpt.find("Map contains invalid Jass scripts that couldn't be compiled by war3") != std::string::npos;

	json result = {{"ok", true},
		  {"command", "probe-map"},
		  {"map", map_path.string()},
		  {"warcraft_exe", exe.string()},
		  {"log_path", log_path.string()},
		  {"pid", proc->pid},
		  {"wait_seconds", wait_seconds},
		  {"click_after_seconds", click_after_opt ? json(parse_seconds_option(args, "click-after", 0)) : json(nullptr)},
		  {"click_sent", click_sent},
		  {"click_window_found", click_window_found},
		  {"enter_after_click_sent", enter_after_click_sent},
		  {"close_confirm_enter_sent", close_confirm_enter_sent},
		  {"exited_within_wait", exited_within_wait},
		  {"close_requested", close_requested},
		  {"terminated", terminated},
		  {"exit_code", static_cast<std::uint64_t>(exit_code)},
		  {"found_map_marker", found_map_marker},
		  {"has_root_error", has_error},
		  {"log_excerpt", excerpt}};
	if (probe_log_path) {
		result["probe_log_path"] = probe_log_path->string();
		result["probe_log_found"] = fs::is_regular_file(*probe_log_path);
		result["probe_log_excerpt"] = read_preload_log_excerpt(*probe_log_path, tail_lines);
	}
	json chat_results = json::array();
	for (std::size_t i = 0; i < chat_schedule.size(); ++i) {
		chat_results.push_back({
			{"after_seconds", chat_schedule[i].after_seconds},
			{"text", chat_schedule[i].text},
			{"sent", chat_sent[i]},
		});
	}
	result["chat_results"] = chat_results;
	json bridge_results = json::array();
	for (std::size_t i = 0; i < bridge_schedule.size(); ++i) {
		bridge_results.push_back({
			{"after_seconds", bridge_schedule[i].after_seconds},
			{"op", bridge_schedule[i].op},
			{"arg", bridge_schedule[i].arg},
			{"delivery", "preloader-file"},
			{"sent", bridge_sent[i]},
		});
	}
	result["bridge_commands"] = bridge_results;
	emit(result);
}

void cmd_read_war3_log(const Args& args) {
	const fs::path log_path = war3_log_path(args);
	if (!fs::is_regular_file(log_path)) {
		fail("War3Log.txt not found: " + log_path.string());
	}

	const std::size_t tail_lines = parse_seconds_option(args, "tail", 120);
	bool found_map_marker = false;
	std::string excerpt;
	if (const auto map = args.get("map")) {
		excerpt = extract_log_excerpt(log_path, fs::path(*map), tail_lines, found_map_marker);
	} else {
		const std::vector<std::string> lines = split_lines(read_text_file(log_path));
		const std::size_t start = lines.size() > tail_lines ? lines.size() - tail_lines : 0;
		excerpt = join_lines(lines, start);
	}

	emit({{"ok", true},
		  {"command", "read-war3-log"},
		  {"log_path", log_path.string()},
		  {"found_map_marker", found_map_marker},
		  {"log_excerpt", excerpt}});
}

void cmd_read_custom_map_data_log(const Args& args) {
	const fs::path log_path = custom_map_data_file_path(args.require("file"));
	if (!fs::is_regular_file(log_path)) {
		fail("custom map data log not found: " + log_path.string());
	}

	const std::size_t tail_lines = parse_seconds_option(args, "tail", 120);
	const std::string excerpt = read_preload_log_excerpt(log_path, tail_lines);
	emit({{"ok", true},
		  {"command", "read-custom-map-data-log"},
		  {"log_path", log_path.string()},
		  {"log_excerpt", excerpt}});
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
	const Utf8Args utf8_args = windows_utf8_args();
	const int effective_argc = utf8_args.argv.empty() ? argc : static_cast<int>(utf8_args.argv.size());
	char** effective_argv = utf8_args.argv.empty() ? argv : const_cast<char**>(utf8_args.argv.data());
	const Args args = parse_args(effective_argc, effective_argv);

	if (args.command.empty() || args.command == "help" || args.has_flag("help")) {
		emit({{"ok", true},
			  {"tool", "HiveWE_cli"},
			  {"commands", json::array({"build-map", "run-map", "probe-map", "read-war3-log", "read-custom-map-data-log", "validate-script",
									   "list-object-types", "search-objects", "get-object", "set-field", "describe-race"})},
			  {"usage", json::object({
				   {"build-map", "--map <dir> [--out <file.w3x>]"},
				   {"run-map", "--map <dir|.w3x> --warcraft <dir> [--ptr] [--args \"...\"]"},
				   {"probe-map", "--map <dir|.w3x> --warcraft <dir> [--wait 12] [--click-after 6] [--chat-after 20 --chat-text -ai2] [--chat-script \"20:-ai2|35:-raceselect1\"] [--bridge-script \"90:create_ai:2|105:race_select:1\"] [--tail 120] [--keep-open] [--log <War3Log.txt>] [--probe-log <relative-file>]"},
				   {"read-war3-log", "[--map <dir|.w3x>] [--tail 120] [--log <War3Log.txt>]"},
				   {"read-custom-map-data-log", "--file <relative-or-absolute-file> [--tail 120]"},
				   {"validate-script", "--map <dir> [--tools <dir>]"},
				   {"list-object-types", "(no args)"},
				   {"search-objects", "--map <dir> --type <unit|item|ability|doodad|destructible|upgrade|buff> --query <substr> [--warcraft <dir>] [--limit N] [--hd]"},
				   {"get-object", "--map <dir> --type <...> --id <id> [--warcraft <dir>] [--fields a,b,c] [--hd]"},
				   {"set-field", "--map <dir> --type <...> --id <id> --field <col> --value <v> [--warcraft <dir>] [--hd]"},
				   {"describe-race", "--map <dir> --suffix <text> [--tokens a,b,c] [--warcraft <dir>] [--hd]"},
			   })}});
	}

	if (args.command == "build-map") {
		cmd_build_map(args);
	} else if (args.command == "run-map") {
		cmd_run_map(args);
	} else if (args.command == "probe-map") {
		cmd_probe_map(args);
	} else if (args.command == "read-war3-log") {
		cmd_read_war3_log(args);
	} else if (args.command == "read-custom-map-data-log") {
		cmd_read_custom_map_data_log(args);
	} else if (args.command == "validate-script") {
		cmd_validate_script(args);
	} else if (args.command == "list-object-types" || args.command == "search-objects" ||
			   args.command == "get-object" || args.command == "set-field" || args.command == "describe-race") {
		bool ok = false;
		const std::string result = hivewe_object_command(effective_argc, effective_argv, warcraft_dir_from_registry(), ok);
		std::fputs(result.c_str(), stdout);
		std::fputc('\n', stdout);
		std::exit(ok ? 0 : 1);
	}

	fail("unknown command: " + args.command);
}
