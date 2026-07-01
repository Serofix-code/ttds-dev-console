#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
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
      << L"  TTDSConsoleLauncher.exe [--game <game folder>] [--no-wait] [--allow-multiple]\n\n"
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

bool InjectDll(DWORD processId, const fs::path& dllPath) {
  const std::wstring dll = fs::absolute(dllPath).wstring();
  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);

  HANDLE process = OpenProcess(
      PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
      FALSE,
      processId);
  if (!process) {
    std::wcerr << L"OpenProcess failed: " << GetLastErrorText(GetLastError()) << L"\n";
    return false;
  }

  void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remotePath) {
    std::wcerr << L"VirtualAllocEx failed: " << GetLastErrorText(GetLastError()) << L"\n";
    CloseHandle(process);
    return false;
  }

  if (!WriteProcessMemory(process, remotePath, dll.c_str(), bytes, nullptr)) {
    std::wcerr << L"WriteProcessMemory failed: " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
  if (!loadLibrary) {
    std::wcerr << L"Could not find LoadLibraryW.\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
  if (!thread) {
    std::wcerr << L"CreateRemoteThread failed: " << GetLastErrorText(GetLastError()) << L"\n";
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return false;
  }

  WaitForSingleObject(thread, 10000);
  DWORD remoteResult = 0;
  GetExitCodeThread(thread, &remoteResult);
  CloseHandle(thread);
  VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
  CloseHandle(process);

  if (remoteResult == 0) {
    std::wcerr << L"LoadLibraryW returned null inside the game process.\n";
    return false;
  }

  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::vector<std::wstring> args(argv + 1, argv + argc);
  if (HasFlag(args, L"--help") || HasFlag(args, L"-h")) {
    PrintUsage();
    return 0;
  }

  const fs::path gameDir = GetArgValue(args, L"--game", kDefaultGameDir);
  const bool noWait = HasFlag(args, L"--no-wait");
  const bool allowMultiple = HasFlag(args, L"--allow-multiple");
  const fs::path dllPath = GetOwnDirectory() / L"TTDSConsoleHook.dll";

  if (!fs::exists(dllPath)) {
    std::wcerr << L"Could not find hook DLL next to launcher: " << dllPath << L"\n";
    return 1;
  }

  fs::path exePath;
  if (!ValidateGameDir(gameDir, &exePath)) {
    return 1;
  }

  if (!allowMultiple && IsProcessRunning(L"WDC.exe")) {
    std::wcerr << L"WDC.exe is already running. Close the game first, or pass --allow-multiple if you really want another instance.\n";
    return 1;
  }

  std::wstring commandLine = L"\"" + exePath.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION processInfo{};

  std::wcout << L"Launching: " << exePath << L"\n";
  BOOL created = CreateProcessW(
      exePath.c_str(),
      commandLine.data(),
      nullptr,
      nullptr,
      FALSE,
      0,
      nullptr,
      gameDir.c_str(),
      &startup,
      &processInfo);

  if (!created) {
    std::wcerr << L"CreateProcess failed: " << GetLastErrorText(GetLastError()) << L"\n";
    return 1;
  }

  std::wcout << L"WDC.exe PID: " << processInfo.dwProcessId << L"\n";
  Sleep(1500);

  std::wcout << L"Injecting: " << dllPath << L"\n";
  const bool injected = InjectDll(processInfo.dwProcessId, dllPath);
  if (!injected) {
    std::wcerr << L"Injection failed. Leaving game running.\n";
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return 1;
  }

  std::wcout << L"Injected. The in-game console window should appear shortly.\n";
  CloseHandle(processInfo.hThread);

  if (!noWait) {
    std::wcout << L"Waiting for WDC.exe to exit...\n";
    WaitForSingleObject(processInfo.hProcess, INFINITE);
  }

  CloseHandle(processInfo.hProcess);
  return 0;
}
