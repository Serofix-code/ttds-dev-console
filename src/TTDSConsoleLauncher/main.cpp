#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr const wchar_t* kDefaultGameDir =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Walking Dead The Telltale Definitive Series";

void PrintUsage() {
  std::wcout
      << L"TTDSConsoleLauncher\n\n"
      << L"Usage:\n"
      << L"  TTDSConsoleLauncher.exe [--game <game folder>] [--allow-multiple]\n"
      << L"  TTDSConsoleLauncher.exe --watch-only [--game <game folder>] [--quiet]\n"
      << L"  TTDSConsoleLauncher.exe --install-autostart [--game <game folder>]\n"
      << L"  TTDSConsoleLauncher.exe --uninstall-autostart\n\n"
      << L"Default game folder:\n"
      << L"  " << kDefaultGameDir << L"\n";
}

std::wstring GetArgValue(const std::vector<std::wstring>& args, const std::wstring& name, const std::wstring& fallback) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return args[i + 1];
  }
  return fallback;
}

bool HasFlag(const std::vector<std::wstring>& args, const std::wstring& name) {
  for (const auto& arg : args) {
    if (arg == name) return true;
  }
  return false;
}

bool IsProcessRunning(const std::wstring& exeName) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, exeName.c_str()) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

bool IsPathInside(const fs::path& child, const fs::path& parent) {
  std::error_code error;
  const fs::path childAbs = fs::weakly_canonical(child, error);
  if (error) return false;
  const fs::path parentAbs = fs::weakly_canonical(parent, error);
  if (error) return false;

  auto childIt = childAbs.begin();
  auto parentIt = parentAbs.begin();
  for (; parentIt != parentAbs.end(); ++parentIt, ++childIt) {
    if (childIt == childAbs.end()) return false;
    if (_wcsicmp(childIt->c_str(), parentIt->c_str()) != 0) return false;
  }
  return true;
}

std::wstring GetLastErrorText(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      error,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer),
      0,
      nullptr);
  std::wstring message = size && buffer ? buffer : L"Unknown error";
  if (buffer) LocalFree(buffer);
  return message;
}

fs::path GetOwnDirectory() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return fs::path(path).parent_path();
}

fs::path GetOwnPath() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return fs::path(path);
}

bool ValidateGameDir(const fs::path& gameDir, fs::path* exePath) {
  const fs::path candidate = gameDir / L"WDC.exe";
  if (!fs::exists(candidate)) {
    std::wcerr << L"Could not find WDC.exe in: " << gameDir << L"\n";
    return false;
  }

  const std::wstring folderName = gameDir.filename().wstring();
  if (folderName.find(L"The Walking Dead") == std::wstring::npos) {
    std::wcerr << L"Refusing to launch: selected folder does not look like the Definitive Series folder.\n";
    std::wcerr << L"Folder was: " << gameDir << L"\n";
    return false;
  }

  *exePath = candidate;
  return true;
}

void* GetRemoteKernel32Proc(DWORD processId, const char* procName) {
  HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
  const auto localProc = reinterpret_cast<uintptr_t>(GetProcAddress(localKernel32, procName));
  if (!localKernel32 || !localProc) return nullptr;

  const uintptr_t offset = localProc - reinterpret_cast<uintptr_t>(localKernel32);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return reinterpret_cast<void*>(localProc);
  }

  MODULEENTRY32W module{};
  module.dwSize = sizeof(module);
  void* remoteProc = nullptr;
  if (Module32FirstW(snapshot, &module)) {
    do {
      if (_wcsicmp(module.szModule, L"kernel32.dll") == 0) {
        remoteProc = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module.modBaseAddr) + offset);
        break;
      }
    } while (Module32NextW(snapshot, &module));
  }
  CloseHandle(snapshot);
  return remoteProc ? remoteProc : reinterpret_cast<void*>(localProc);
}

bool InjectDll(HANDLE process, DWORD processId, const fs::path& dllPath) {
  const std::wstring dll = fs::absolute(dllPath).wstring();
  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);

  void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remotePath) {
    std::wcerr << L"VirtualAllocEx failed: " << GetLastErrorText(GetLastError()) << L"\n";
    return false;
  }

  if (!WriteProcessMemory(process, remotePath, dll.c_str(), bytes, nullptr)) {
    std::wcerr << L"WriteProcessMemory failed: " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetRemoteKernel32Proc(processId, "LoadLibraryW"));
  if (!loadLibrary) {
    std::wcerr << L"Could not find LoadLibraryW.\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
  if (!thread) {
    std::wcerr << L"CreateRemoteThread failed: " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  WaitForSingleObject(thread, 10000);
  DWORD remoteResult = 0;
  GetExitCodeThread(thread, &remoteResult);
  CloseHandle(thread);
  VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

  if (remoteResult == 0) {
    std::wcerr << L"LoadLibraryW returned null inside the game process.\n";
    return false;
  }

  return true;
}

bool InjectDllAsync(HANDLE process, DWORD processId, const fs::path& dllPath) {
  const std::wstring dll = fs::absolute(dllPath).wstring();
  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);

  void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remotePath) {
    std::wcerr << L"VirtualAllocEx failed for PID " << processId << L": " << GetLastErrorText(GetLastError()) << L"\n";
    return false;
  }

  if (!WriteProcessMemory(process, remotePath, dll.c_str(), bytes, nullptr)) {
    std::wcerr << L"WriteProcessMemory failed for PID " << processId << L": " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetRemoteKernel32Proc(processId, "LoadLibraryW"));
  if (!loadLibrary) {
    std::wcerr << L"Could not find LoadLibraryW for PID " << processId << L".\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
  if (!thread) {
    std::wcerr << L"CreateRemoteThread failed for PID " << processId << L": " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return false;
  }

  CloseHandle(thread);
  return true;
}

struct DebugProcessInfo {
  HANDLE process = nullptr;
  ULONGLONG injectAfter = 0;
  bool injected = false;
  std::wstring imagePath;
};

std::wstring GetProcessImagePath(HANDLE process) {
  wchar_t path[MAX_PATH * 4]{};
  DWORD size = static_cast<DWORD>(std::size(path));
  if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
    return L"";
  }
  return std::wstring(path, size);
}

void CleanupExitedProcesses(std::map<DWORD, DebugProcessInfo>& processes) {
  for (auto it = processes.begin(); it != processes.end();) {
    DWORD exitCode = 0;
    if (it->second.process && GetExitCodeProcess(it->second.process, &exitCode) && exitCode != STILL_ACTIVE) {
      CloseHandle(it->second.process);
      it = processes.erase(it);
    } else {
      ++it;
    }
  }
}

void ScanForWdcProcesses(std::map<DWORD, DebugProcessInfo>& processes, const fs::path& gameDir, bool quiet) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"WDC.exe") != 0 || processes.find(entry.th32ProcessID) != processes.end()) {
        continue;
      }

      HANDLE process = OpenProcess(
          PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
          FALSE,
          entry.th32ProcessID);
      if (!process) {
        continue;
      }

      const std::wstring imagePath = GetProcessImagePath(process);
      if (!imagePath.empty() && !IsPathInside(imagePath, gameDir)) {
        CloseHandle(process);
        continue;
      }

      if (!quiet) {
        std::wcout << L"Found WDC.exe PID " << entry.th32ProcessID;
        if (!imagePath.empty()) std::wcout << L" (" << imagePath << L")";
        std::wcout << L"\n";
      }

      processes[entry.th32ProcessID] = DebugProcessInfo{
          process,
          GetTickCount64() + 250,
          false,
          imagePath};
    } while (Process32NextW(snapshot, &entry));
  }

  CloseHandle(snapshot);
}

bool HasTrackedProcess(const std::map<DWORD, DebugProcessInfo>& processes) {
  for (const auto& [pid, info] : processes) {
    DWORD exitCode = 0;
    if (info.process && GetExitCodeProcess(info.process, &exitCode) && exitCode == STILL_ACTIVE) {
      return true;
    }
  }
  return false;
}

void TryPendingInjections(std::map<DWORD, DebugProcessInfo>& processes, const fs::path& gameDir, const fs::path& dllPath, bool quiet) {
  CleanupExitedProcesses(processes);
  ScanForWdcProcesses(processes, gameDir, quiet);

  const ULONGLONG now = GetTickCount64();
  for (auto& [pid, info] : processes) {
    if (!info.process || info.injected || now < info.injectAfter) continue;

    if (!quiet) {
      std::wcout << L"Injecting into WDC.exe PID " << pid;
      if (!info.imagePath.empty()) std::wcout << L" (" << info.imagePath << L")";
      std::wcout << L"\n";
    }

    info.injected = InjectDllAsync(info.process, pid, dllPath);
    if (info.injected && !quiet) {
      std::wcout << L"Queued console DLL for PID " << pid << L".\n";
    } else if (!info.injected) {
      info.injectAfter = now + 1000;
      if (!quiet) std::wcerr << L"Will retry PID " << pid << L" in 1 second.\n";
    }
  }
}

int WatchOnly(const fs::path& gameDir, const fs::path& dllPath, bool quiet) {
  std::map<DWORD, DebugProcessInfo> processes;
  if (!quiet) {
    std::wcout << L"Watching for WDC.exe from: " << gameDir << L"\n";
    std::wcout << L"Press Ctrl+C to stop the watcher.\n";
  }

  while (true) {
    TryPendingInjections(processes, gameDir, dllPath, quiet);
    Sleep(500);
  }
}

std::wstring EscapeForVbs(const std::wstring& value) {
  std::wstring escaped;
  for (wchar_t ch : value) {
    escaped += ch;
    if (ch == L'"') escaped += L'"';
  }
  return escaped;
}

bool WriteAutostartScript(const fs::path& scriptPath, const fs::path& launcherPath, const fs::path& gameDir) {
  std::wofstream script(scriptPath, std::ios::trunc);
  if (!script) return false;

  const std::wstring command =
      L"\"" + launcherPath.wstring() + L"\" --watch-only --quiet --game \"" + gameDir.wstring() + L"\"";
  script << L"Set shell = CreateObject(\"WScript.Shell\")\n";
  script << L"shell.Run \"" << EscapeForVbs(command) << L"\", 0, False\n";
  return script.good();
}

int InstallAutostart(const fs::path& gameDir) {
  const fs::path launcherPath = GetOwnPath();
  const fs::path scriptPath = GetOwnDirectory() / L"TTDSConsoleWatcher.vbs";
  if (!WriteAutostartScript(scriptPath, launcherPath, gameDir)) {
    std::wcerr << L"Could not write autostart helper: " << scriptPath << L"\n";
    return 1;
  }

  const std::wstring value = L"wscript.exe \"" + scriptPath.wstring() + L"\"";
  HKEY key = nullptr;
  const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (result != ERROR_SUCCESS) {
    std::wcerr << L"Could not open HKCU Run key: " << GetLastErrorText(result) << L"\n";
    return 1;
  }

  result = RegSetValueExW(
      key,
      L"TTDSDevConsoleWatcher",
      0,
      REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);

  if (result != ERROR_SUCCESS) {
    std::wcerr << L"Could not install autostart entry: " << GetLastErrorText(result) << L"\n";
    return 1;
  }

  std::wcout << L"Installed autostart watcher.\n";
  std::wcout << L"It will inject the console whenever Steam starts WDC.exe after your next Windows login.\n";
  std::wcout << L"Helper: " << scriptPath << L"\n";
  return 0;
}

int UninstallAutostart() {
  HKEY key = nullptr;
  const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
  if (result == ERROR_SUCCESS) {
    RegDeleteValueW(key, L"TTDSDevConsoleWatcher");
    RegCloseKey(key);
  }

  std::error_code error;
  fs::remove(GetOwnDirectory() / L"TTDSConsoleWatcher.vbs", error);
  std::wcout << L"Uninstalled autostart watcher. If it is currently running, sign out/in or end TTDSConsoleLauncher.exe from Task Manager.\n";
  return 0;
}

int LaunchAndWatch(const fs::path& exePath, const fs::path& gameDir, const fs::path& dllPath) {
  std::wstring commandLine = L"\"" + exePath.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION processInfo{};

  std::wcout << L"Launching suspended: " << exePath << L"\n";
  BOOL created = CreateProcessW(
      exePath.c_str(),
      commandLine.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_SUSPENDED,
      nullptr,
      gameDir.c_str(),
      &startup,
      &processInfo);

  if (!created) {
    std::wcerr << L"CreateProcess failed: " << GetLastErrorText(GetLastError()) << L"\n";
    return 1;
  }

  std::wcout << L"WDC.exe PID: " << processInfo.dwProcessId << L"\n";
  std::wcout << L"Injecting first process: " << dllPath << L"\n";
  const bool initialInjected = InjectDll(processInfo.hProcess, processInfo.dwProcessId, dllPath);
  if (!initialInjected) {
    std::wcerr << L"Initial injection failed. Resuming game and watching for a relaunch target.\n";
  }

  ResumeThread(processInfo.hThread);
  CloseHandle(processInfo.hThread);

  std::map<DWORD, DebugProcessInfo> processes;
  processes[processInfo.dwProcessId] = DebugProcessInfo{
      processInfo.hProcess,
      initialInjected ? 0 : GetTickCount64() + 1000,
      initialInjected,
      exePath.wstring()};

  std::wcout << L"Watching for WDC.exe relaunches. Keep this launcher open while the game runs.\n";
  const ULONGLONG watchUntil = GetTickCount64() + 120000;
  while (GetTickCount64() < watchUntil || HasTrackedProcess(processes)) {
    TryPendingInjections(processes, gameDir, dllPath, false);
    Sleep(250);
  }

  for (auto& [pid, info] : processes) {
    if (info.process) CloseHandle(info.process);
  }
  return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::vector<std::wstring> args(argv + 1, argv + argc);
  if (HasFlag(args, L"--help") || HasFlag(args, L"-h")) {
    PrintUsage();
    return 0;
  }

  const fs::path gameDir = GetArgValue(args, L"--game", kDefaultGameDir);
  const bool allowMultiple = HasFlag(args, L"--allow-multiple");
  const bool quiet = HasFlag(args, L"--quiet");
  const fs::path dllPath = GetOwnDirectory() / L"TTDSConsoleHook.dll";

  if (!fs::exists(dllPath)) {
    std::wcerr << L"Could not find hook DLL next to launcher: " << dllPath << L"\n";
    return 1;
  }

  fs::path exePath;
  if (!ValidateGameDir(gameDir, &exePath)) {
    return 1;
  }

  if (HasFlag(args, L"--install-autostart")) {
    return InstallAutostart(gameDir);
  }

  if (HasFlag(args, L"--uninstall-autostart")) {
    return UninstallAutostart();
  }

  if (HasFlag(args, L"--watch-only")) {
    return WatchOnly(gameDir, dllPath, quiet);
  }

  if (!allowMultiple && IsProcessRunning(L"WDC.exe")) {
    std::wcerr << L"WDC.exe is already running. Close the game first, or pass --allow-multiple if you really want another instance.\n";
    return 1;
  }

  return LaunchAndWatch(exePath, gameDir, dllPath);
}
