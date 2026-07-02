#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
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
      << L"  TTDSConsoleLauncher.exe [--game <game folder>] [--allow-multiple]\n\n"
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

std::wstring ToLower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(towlower(c));
  });
  return value;
}

bool IsWdcImage(const std::wstring& path) {
  if (path.empty()) return false;
  return ToLower(fs::path(path).filename().wstring()) == L"wdc.exe";
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
    CloseHandle(process);
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

std::wstring GetPathFromFileHandle(HANDLE file) {
  if (!file || file == INVALID_HANDLE_VALUE) return L"";
  wchar_t path[MAX_PATH * 4]{};
  DWORD size = GetFinalPathNameByHandleW(file, path, static_cast<DWORD>(std::size(path)), FILE_NAME_NORMALIZED);
  if (size == 0 || size >= std::size(path)) return L"";
  std::wstring result(path);
  constexpr const wchar_t* devicePrefix = L"\\\\?\\";
  if (result.rfind(devicePrefix, 0) == 0) {
    result.erase(0, 4);
  }
  return result;
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

void ScanForWdcProcesses(std::map<DWORD, DebugProcessInfo>& processes) {
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
      std::wcout << L"Found relaunched WDC.exe PID " << entry.th32ProcessID;
      if (!imagePath.empty()) std::wcout << L" (" << imagePath << L")";
      std::wcout << L"\n";

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

void TryPendingInjections(std::map<DWORD, DebugProcessInfo>& processes, const fs::path& dllPath) {
  CleanupExitedProcesses(processes);
  ScanForWdcProcesses(processes);

  const ULONGLONG now = GetTickCount64();
  for (auto& [pid, info] : processes) {
    if (!info.process || info.injected || now < info.injectAfter) continue;

    std::wcout << L"Injecting into WDC.exe PID " << pid;
    if (!info.imagePath.empty()) std::wcout << L" (" << info.imagePath << L")";
    std::wcout << L"\n";

    info.injected = InjectDllAsync(info.process, pid, dllPath);
    if (info.injected) {
      std::wcout << L"Queued console DLL for PID " << pid << L".\n";
    } else {
      info.injectAfter = now + 1000;
      std::wcerr << L"Will retry PID " << pid << L" in 1 second.\n";
    }
  }
}

int DebugLaunchAndInject(const fs::path& exePath, const fs::path& gameDir, const fs::path& dllPath) {
  std::wstring commandLine = L"\"" + exePath.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION processInfo{};

  DebugSetProcessKillOnExit(FALSE);

  std::wcout << L"Launching under relaunch watcher: " << exePath << L"\n";
  BOOL created = CreateProcessW(
      exePath.c_str(),
      commandLine.data(),
      nullptr,
      nullptr,
      FALSE,
      DEBUG_PROCESS,
      nullptr,
      gameDir.c_str(),
      &startup,
      &processInfo);

  if (!created) {
    std::wcerr << L"CreateProcess failed: " << GetLastErrorText(GetLastError()) << L"\n";
    return 1;
  }

  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);

  std::map<DWORD, DebugProcessInfo> processes;
  while (true) {
    DEBUG_EVENT event{};
    if (!WaitForDebugEvent(&event, 250)) {
      const DWORD error = GetLastError();
      if (error != ERROR_SEM_TIMEOUT) {
        std::wcerr << L"Debug loop ended: " << GetLastErrorText(error) << L"\n";
        break;
      }
      TryPendingInjections(processes, dllPath);
      continue;
    }

    DWORD continueStatus = DBG_CONTINUE;
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
      const auto& info = event.u.CreateProcessInfo;
      std::wstring imagePath = GetPathFromFileHandle(info.hFile);
      if (info.hFile) CloseHandle(info.hFile);
      if (info.hThread) CloseHandle(info.hThread);

      if (IsWdcImage(imagePath)) {
        std::wcout << L"Detected WDC.exe PID " << event.dwProcessId;
        if (!imagePath.empty()) std::wcout << L" (" << imagePath << L")";
        std::wcout << L"\n";
        processes[event.dwProcessId] = DebugProcessInfo{
            info.hProcess,
            GetTickCount64() + 250,
            false,
            imagePath};
      } else if (info.hProcess) {
        CloseHandle(info.hProcess);
      }
    } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      auto found = processes.find(event.dwProcessId);
      if (found != processes.end()) {
        std::wcout << L"WDC.exe PID " << event.dwProcessId << L" exited.\n";
        if (found->second.process) CloseHandle(found->second.process);
        processes.erase(found);
      }
    } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
      if (code != EXCEPTION_BREAKPOINT && code != DBG_PRINTEXCEPTION_C && code != DBG_PRINTEXCEPTION_WIDE_C) {
        continueStatus = DBG_EXCEPTION_NOT_HANDLED;
      }
    }

    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    TryPendingInjections(processes, dllPath);
  }

  for (auto& [pid, info] : processes) {
    if (info.process) CloseHandle(info.process);
  }

  std::wcout << L"Watching for WDC.exe relaunches for 60 seconds...\n";
  processes.clear();
  const ULONGLONG watchUntil = GetTickCount64() + 60000;
  while (GetTickCount64() < watchUntil || HasTrackedProcess(processes)) {
    TryPendingInjections(processes, dllPath);
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

  return DebugLaunchAndInject(exePath, gameDir, dllPath);
}
