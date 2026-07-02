#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
HMODULE g_module = nullptr;
std::atomic_bool g_logEnabled{false};
std::atomic_bool g_logConsoleEnabled{true};
std::atomic_bool g_logCompactEnabled{true};
std::atomic_bool g_fileTraceEnabled{true};
std::atomic_bool g_debugStringTraceEnabled{true};
std::atomic_bool g_logFailuresOnly{false};
std::atomic_int g_logFocusMode{1};
std::atomic_bool g_freecamEnabled{false};
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
fs::path g_logPath;
fs::path g_relightDevelopmentConfigPath;

using CreateFileWFn = HANDLE(WINAPI*)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using OutputDebugStringWFn = VOID(WINAPI*)(LPCWSTR);
using OutputDebugStringAFn = VOID(WINAPI*)(LPCSTR);

CreateFileWFn g_realCreateFileW = nullptr;
CreateFileAFn g_realCreateFileA = nullptr;
OutputDebugStringWFn g_realOutputDebugStringW = nullptr;
OutputDebugStringAFn g_realOutputDebugStringA = nullptr;
thread_local bool g_insideHook = false;

std::wstring GetModulePath(HMODULE module) {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  return path;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return L"";
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 0) return L"";
  std::wstring result(static_cast<size_t>(size - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
  return result;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return "";
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "";
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring ToLower(std::wstring value) {
  for (wchar_t& ch : value) {
    ch = static_cast<wchar_t>(towlower(ch));
  }
  return value;
}

std::string ToLowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::wstring FocusModeText(int mode) {
  switch (mode) {
    case 0: return L"all";
    case 1: return L"useful";
    case 2: return L"saves";
    case 3: return L"relight";
    case 4: return L"mods";
    case 5: return L"archives";
    default: return L"useful";
  }
}

int ParseFocusMode(const std::string& value) {
  const std::string lower = ToLowerAscii(value);
  if (lower == "all" || lower == "verbose") return 0;
  if (lower == "saves" || lower == "save") return 2;
  if (lower == "relight" || lower == "freecam") return 3;
  if (lower == "mods" || lower == "mod") return 4;
  if (lower == "archives" || lower == "archive") return 5;
  return 1;
}

std::wstring NowText() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[64]{};
  swprintf_s(
      buffer,
      L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
      time.wYear,
      time.wMonth,
      time.wDay,
      time.wHour,
      time.wMinute,
      time.wSecond,
      time.wMilliseconds);
  return buffer;
}

std::wstring FileTimestampText() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[64]{};
  swprintf_s(
      buffer,
      L"%04u%02u%02u_%02u%02u%02u",
      time.wYear,
      time.wMonth,
      time.wDay,
      time.wHour,
      time.wMinute,
      time.wSecond);
  return buffer;
}

void OpenLogFile() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logFile) return;

  g_logPath = fs::current_path() / L"ttds-dev-console.log";
  _wfopen_s(&g_logFile, g_logPath.c_str(), L"a, ccs=UTF-8");
  if (g_logFile) {
    fwprintf(g_logFile, L"\n[%ls] ===== TTDS Dev Console attached to PID %lu =====\n", NowText().c_str(), GetCurrentProcessId());
    fflush(g_logFile);
  }
}

void CloseLogFile() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_logFile) return;
  fwprintf(g_logFile, L"[%ls] ===== TTDS Dev Console detached =====\n", NowText().c_str());
  fclose(g_logFile);
  g_logFile = nullptr;
}

void LogLine(const std::wstring& category, const std::wstring& message) {
  const std::wstring line = L"[" + NowText() + L"] [" + category + L"] " + message;
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logConsoleEnabled) {
    fwprintf(stdout, L"\n%ls\n", line.c_str());
    fflush(stdout);
  }
  if (g_logFile) {
    fwprintf(g_logFile, L"%ls\n", line.c_str());
    fflush(g_logFile);
  }
}

bool CopyTextFile(const fs::path& source, const fs::path& destination) {
  std::ifstream input(source, std::ios::binary);
  if (!input) return false;
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) return false;

  output << "TTDS Dev Console transcript\n";
  output << "Saved from: " << WideToUtf8(source.wstring()) << "\n\n";
  output << input.rdbuf();
  return output.good();
}

fs::path RelightDevelopmentConfigPath() {
  if (!g_relightDevelopmentConfigPath.empty()) return g_relightDevelopmentConfigPath;
  g_relightDevelopmentConfigPath = fs::current_path() / L"RelightMod" / L"RelightConfiguration_Development.ini";
  return g_relightDevelopmentConfigPath;
}

bool ReadTextFile(const fs::path& path, std::string* output) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  *output = buffer.str();
  return true;
}

bool WriteTextFile(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::string TrimAscii(const std::string& value) {
  const char* whitespace = " \t\r\n";
  const size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) return "";
  const size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

bool TryReadIniBool(const std::string& text, const std::string& section, const std::string& key, bool* value) {
  std::istringstream stream(text);
  std::string line;
  bool inSection = false;
  while (std::getline(stream, line)) {
    std::string trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      inSection = ToLowerAscii(trimmed.substr(1, trimmed.size() - 2)) == ToLowerAscii(section);
      continue;
    }

    if (!inSection) continue;
    const size_t equals = trimmed.find('=');
    if (equals == std::string::npos) continue;
    const std::string foundKey = TrimAscii(trimmed.substr(0, equals));
    if (ToLowerAscii(foundKey) != ToLowerAscii(key)) continue;

    const std::string foundValue = ToLowerAscii(TrimAscii(trimmed.substr(equals + 1)));
    *value = foundValue == "true" || foundValue == "1" || foundValue == "yes" || foundValue == "on";
    return true;
  }
  return false;
}

std::string SetIniKey(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
  std::istringstream stream(text);
  std::ostringstream output;
  std::string line;
  bool inSection = false;
  bool sawSection = false;
  bool wroteKey = false;

  while (std::getline(stream, line)) {
    const bool hadCarriageReturn = !line.empty() && line.back() == '\r';
    if (hadCarriageReturn) line.pop_back();

    const std::string trimmed = TrimAscii(line);
    if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
      if (inSection && !wroteKey) {
        output << key << "=" << value << "\r\n";
        wroteKey = true;
      }
      inSection = ToLowerAscii(trimmed.substr(1, trimmed.size() - 2)) == ToLowerAscii(section);
      if (inSection) sawSection = true;
    } else if (inSection) {
      const size_t equals = trimmed.find('=');
      if (equals != std::string::npos) {
        const std::string foundKey = TrimAscii(trimmed.substr(0, equals));
        if (ToLowerAscii(foundKey) == ToLowerAscii(key)) {
          output << key << "=" << value << "\r\n";
          wroteKey = true;
          continue;
        }
      }
    }

    output << line << "\r\n";
  }

  if (inSection && !wroteKey) {
    output << key << "=" << value << "\r\n";
    wroteKey = true;
  }

  if (!sawSection) {
    output << "\r\n[" << section << "]\r\n";
    output << key << "=" << value << "\r\n";
  }

  return output.str();
}

bool BackupFileOnce(const fs::path& path) {
  const fs::path backup = path.wstring() + L".bak";
  if (fs::exists(backup)) return true;
  std::error_code error;
  fs::copy_file(path, backup, fs::copy_options::none, error);
  return !error;
}

bool GetRelightFreecamSetting(bool* enabled) {
  std::string text;
  if (!ReadTextFile(RelightDevelopmentConfigPath(), &text)) return false;
  return TryReadIniBool(text, "DevelopmentTools", "FreeCameraOnlyMode", enabled);
}

bool SetRelightFreecamSetting(bool enabled, std::wstring* errorMessage) {
  const fs::path path = RelightDevelopmentConfigPath();
  std::string text;
  if (!ReadTextFile(path, &text)) {
    if (errorMessage) *errorMessage = L"Could not read " + path.wstring();
    return false;
  }

  BackupFileOnce(path);
  text = SetIniKey(text, "DevelopmentTools", "FreeCameraOnlyMode", enabled ? "true" : "false");
  if (!WriteTextFile(path, text)) {
    if (errorMessage) *errorMessage = L"Could not write " + path.wstring();
    return false;
  }

  return true;
}

bool LooksInterestingPath(const std::wstring& path) {
  const std::wstring lower = ToLower(path);
  return lower.find(L"\\archives\\") != std::wstring::npos ||
         lower.find(L"/archives/") != std::wstring::npos ||
         lower.find(L".ttarch") != std::wstring::npos ||
         lower.find(L".lua") != std::wstring::npos ||
         lower.find(L".scene") != std::wstring::npos ||
         lower.find(L".chore") != std::wstring::npos ||
         lower.find(L".dlog") != std::wstring::npos ||
         lower.find(L".prop") != std::wstring::npos ||
         lower.find(L"save") != std::wstring::npos ||
         lower.find(L"prefs") != std::wstring::npos;
}

std::wstring AccessText(DWORD desiredAccess) {
  std::wstring text;
  if (desiredAccess & GENERIC_READ) text += L"R";
  if (desiredAccess & GENERIC_WRITE) text += L"W";
  if (desiredAccess & GENERIC_EXECUTE) text += L"X";
  if (desiredAccess & GENERIC_ALL) text += L"A";
  if (text.empty()) text = L"0";
  return text;
}

std::wstring DispositionText(DWORD creationDisposition) {
  switch (creationDisposition) {
    case CREATE_NEW: return L"CREATE_NEW";
    case CREATE_ALWAYS: return L"CREATE_ALWAYS";
    case OPEN_EXISTING: return L"OPEN_EXISTING";
    case OPEN_ALWAYS: return L"OPEN_ALWAYS";
    case TRUNCATE_EXISTING: return L"TRUNCATE_EXISTING";
    default: return L"UNKNOWN";
  }
}

std::wstring AccessVerb(DWORD desiredAccess) {
  if (desiredAccess & GENERIC_WRITE) return L"WRITE";
  if (desiredAccess & GENERIC_READ) return L"READ ";
  if (desiredAccess & GENERIC_EXECUTE) return L"EXEC ";
  return L"OPEN ";
}

std::wstring FileNameOnly(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return path;
  return path.substr(slash + 1);
}

std::wstring CompactPath(const std::wstring& path) {
  const std::wstring lower = ToLower(path);
  const std::wstring game = ToLower(fs::current_path().wstring());
  const std::wstring marker = L"the walking dead definitive series\\";
  const size_t gameAt = lower.find(marker);
  if (gameAt != std::wstring::npos) {
    return path.substr(gameAt + marker.size());
  }

  if (lower.find(L"telltale games\\the walking dead definitive\\") != std::wstring::npos) {
    return L"saves\\" + FileNameOnly(path);
  }

  if (!game.empty()) {
    const size_t gameRootAt = lower.find(game);
    if (gameRootAt != std::wstring::npos) {
      size_t start = gameRootAt + game.size();
      while (start < path.size() && (path[start] == L'\\' || path[start] == L'/')) ++start;
      return path.substr(start);
    }
  }

  return path;
}

std::wstring FileKind(const std::wstring& path) {
  const std::wstring lower = ToLower(path);
  if (lower.find(L"relightlevels/") != std::wstring::npos || lower.find(L"relightlevels\\") != std::wstring::npos) return L"relight";
  if (lower.find(L"\\archives\\") != std::wstring::npos || lower.find(L"/archives/") != std::wstring::npos || lower.find(L".ttarch") != std::wstring::npos) return L"archive";
  if (lower.find(L"prefs.prop") != std::wstring::npos) return L"prefs";
  if (lower.find(L"save") != std::wstring::npos || lower.find(L".bundle") != std::wstring::npos) return L"save";
  if (lower.find(L".lua") != std::wstring::npos) return L"lua";
  if (lower.find(L".prop") != std::wstring::npos) return L"prop";
  return L"file";
}

bool IsSaveOrPrefsPath(const std::wstring& lower) {
  return lower.find(L"prefs.prop") != std::wstring::npos ||
         lower.find(L".bundle") != std::wstring::npos ||
         lower.find(L"save") != std::wstring::npos;
}

bool IsRelightPath(const std::wstring& lower) {
  return lower.find(L"relight") != std::wstring::npos ||
         lower.find(L"tlse") != std::wstring::npos;
}

bool IsModPath(const std::wstring& lower) {
  return lower.find(L"debugloader") != std::wstring::npos ||
         lower.find(L"loadanylevel") != std::wstring::npos ||
         lower.find(L"serofix") != std::wstring::npos ||
         lower.find(L"friendjames") != std::wstring::npos ||
         lower.find(L"modinfo") != std::wstring::npos ||
         lower.find(L"disabled_mods") != std::wstring::npos ||
         lower.find(L"_serofix_disabled") != std::wstring::npos;
}

bool IsArchivePath(const std::wstring& lower) {
  return lower.find(L"\\archives\\") != std::wstring::npos ||
         lower.find(L"/archives/") != std::wstring::npos ||
         lower.find(L".ttarch") != std::wstring::npos;
}

bool IsBootArchiveScanNoise(const std::wstring& lower) {
  const std::wstring name = ToLower(FileNameOnly(lower));
  return name.find(L"_resdesc_50_") == 0 ||
         name.find(L"_resourcedescriptions") == 0 ||
         name.find(L"_compressed.ttarch2") != std::wstring::npos ||
         name.find(L"audio") != std::wstring::npos ||
         name.find(L"chinese") != std::wstring::npos ||
         name.find(L"french") != std::wstring::npos ||
         name.find(L"german") != std::wstring::npos ||
         name.find(L"italian") != std::wstring::npos ||
         name.find(L"portuguese") != std::wstring::npos ||
         name.find(L"russian") != std::wstring::npos ||
         name.find(L"spanish") != std::wstring::npos;
}

bool ShouldLogFileEvent(const std::wstring& path, HANDLE result) {
  if (!LooksInterestingPath(path)) return false;
  if (g_logFailuresOnly && result != INVALID_HANDLE_VALUE) return false;

  const std::wstring lower = ToLower(path);
  const bool failed = result == INVALID_HANDLE_VALUE;
  const int focus = g_logFocusMode.load();

  if (focus == 0) return true;
  if (focus == 2) return IsSaveOrPrefsPath(lower);
  if (focus == 3) return failed || IsRelightPath(lower);
  if (focus == 4) return failed || IsModPath(lower) || IsRelightPath(lower);
  if (focus == 5) return IsArchivePath(lower);

  return failed ||
         IsSaveOrPrefsPath(lower) ||
         IsRelightPath(lower) ||
         IsModPath(lower) ||
         (IsArchivePath(lower) && !IsBootArchiveScanNoise(lower));
}

std::wstring FormatFileEvent(const std::wstring& apiName, DWORD desiredAccess, DWORD creationDisposition, HANDLE result, const std::wstring& path) {
  const bool ok = result != INVALID_HANDLE_VALUE;
  std::wstringstream message;
  if (g_logCompactEnabled) {
    message << (ok ? L"OK   " : L"FAIL ")
            << AccessVerb(desiredAccess) << L" "
            << FileKind(path) << L"  "
            << CompactPath(path);
    const std::wstring lower = ToLower(path);
    if (IsModPath(lower) && (lower.find(L"disabled_mods") != std::wstring::npos || lower.find(L"_serofix_disabled") != std::wstring::npos)) {
      message << L"  [disabled-folder-read]";
    }
  } else {
    message << apiName << L" " << AccessText(desiredAccess) << L" "
            << DispositionText(creationDisposition) << L" -> "
            << (ok ? L"OK " : L"FAIL ")
            << path;
  }
  return message.str();
}

std::vector<HMODULE> GetProcessModules() {
  std::vector<HMODULE> modules;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return modules;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      modules.push_back(entry.hModule);
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return modules;
}

bool PatchImport(HMODULE module, const char* importedModule, const char* functionName, void* replacement, void** original) {
  if (!module || module == g_module) return false;

  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!directory.VirtualAddress || !directory.Size) return false;

  auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
  bool patched = false;
  for (; imports->Name; ++imports) {
    const char* moduleName = reinterpret_cast<const char*>(base + imports->Name);
    if (_stricmp(moduleName, importedModule) != 0) continue;

    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
    auto* originalThunk = imports->OriginalFirstThunk
        ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk)
        : thunk;

    for (; originalThunk->u1.AddressOfData && thunk->u1.Function; ++originalThunk, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) continue;

      auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(importByName->Name), functionName) != 0) continue;

      void* current = reinterpret_cast<void*>(thunk->u1.Function);
      if (current == replacement) {
        patched = true;
        continue;
      }
      if (original && !*original) *original = current;

      DWORD oldProtect = 0;
      if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        thunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
        DWORD ignored = 0;
        VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignored);
        patched = true;
      }
    }
  }

  return patched;
}

HANDLE WINAPI HookCreateFileW(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile) {
  if (!g_realCreateFileW) {
    g_realCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
  }

  HANDLE result = g_realCreateFileW(
      fileName,
      desiredAccess,
      shareMode,
      securityAttributes,
      creationDisposition,
      flagsAndAttributes,
      templateFile);

  if (!g_insideHook && g_logEnabled && g_fileTraceEnabled && fileName) {
    g_insideHook = true;
    const std::wstring path(fileName);
    if (ShouldLogFileEvent(path, result)) {
      LogLine(L"file", FormatFileEvent(L"CreateFileW", desiredAccess, creationDisposition, result, path));
    }
    g_insideHook = false;
  }

  return result;
}

HANDLE WINAPI HookCreateFileA(
    LPCSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile) {
  if (!g_realCreateFileA) {
    g_realCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileA"));
  }

  HANDLE result = g_realCreateFileA(
      fileName,
      desiredAccess,
      shareMode,
      securityAttributes,
      creationDisposition,
      flagsAndAttributes,
      templateFile);

  if (!g_insideHook && g_logEnabled && g_fileTraceEnabled && fileName) {
    g_insideHook = true;
    const std::wstring path = Utf8ToWide(fileName);
    if (ShouldLogFileEvent(path, result)) {
      LogLine(L"file", FormatFileEvent(L"CreateFileA", desiredAccess, creationDisposition, result, path));
    }
    g_insideHook = false;
  }

  return result;
}

VOID WINAPI HookOutputDebugStringW(LPCWSTR message) {
  if (!g_realOutputDebugStringW) {
    g_realOutputDebugStringW = reinterpret_cast<OutputDebugStringWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringW"));
  }
  if (!g_insideHook && g_logEnabled && g_debugStringTraceEnabled && message) {
    g_insideHook = true;
    LogLine(L"debug", message);
    g_insideHook = false;
  }
  g_realOutputDebugStringW(message);
}

VOID WINAPI HookOutputDebugStringA(LPCSTR message) {
  if (!g_realOutputDebugStringA) {
    g_realOutputDebugStringA = reinterpret_cast<OutputDebugStringAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringA"));
  }
  if (!g_insideHook && g_logEnabled && g_debugStringTraceEnabled && message) {
    g_insideHook = true;
    LogLine(L"debug", Utf8ToWide(message));
    g_insideHook = false;
  }
  g_realOutputDebugStringA(message);
}

int InstallHooks() {
  g_realCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
  g_realCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileA"));
  g_realOutputDebugStringW = reinterpret_cast<OutputDebugStringWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringW"));
  g_realOutputDebugStringA = reinterpret_cast<OutputDebugStringAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringA"));

  int patched = 0;
  for (HMODULE module : GetProcessModules()) {
    patched += PatchImport(module, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_realCreateFileW)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_realCreateFileA)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "OutputDebugStringW", reinterpret_cast<void*>(&HookOutputDebugStringW), reinterpret_cast<void**>(&g_realOutputDebugStringW)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "OutputDebugStringA", reinterpret_cast<void*>(&HookOutputDebugStringA), reinterpret_cast<void**>(&g_realOutputDebugStringA)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_realCreateFileW)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_realCreateFileA)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "OutputDebugStringW", reinterpret_cast<void*>(&HookOutputDebugStringW), reinterpret_cast<void**>(&g_realOutputDebugStringW)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "OutputDebugStringA", reinterpret_cast<void*>(&HookOutputDebugStringA), reinterpret_cast<void**>(&g_realOutputDebugStringA)) ? 1 : 0;
  }
  return patched;
}

void AttachConsoleStreams() {
  FILE* ignored = nullptr;
  freopen_s(&ignored, "CONOUT$", "w", stdout);
  freopen_s(&ignored, "CONOUT$", "w", stderr);
  freopen_s(&ignored, "CONIN$", "r", stdin);
  std::ios::sync_with_stdio();
}

void PrintHelp() {
  std::cout
      << "Commands:\n"
      << "  help      Show this help\n"
      << "  status    Show process and hook info\n"
      << "  where     Show current working directory\n"
      << "  archives  Count files in the Archives folder\n"
      << "  log       Show log status\n"
      << "  log on    Start writing ttds-dev-console.log\n"
      << "  log off   Stop writing the log\n"
      << "  log console on/off  Show or hide live log lines in this console\n"
      << "  log format compact/full  Change file log readability\n"
      << "  log focus useful/all/saves/relight/mods/archives\n"
      << "  log failures on/off  Only show failed file opens\n"
      << "  log path  Print the log file path\n"
      << "  log mark <text>  Add a marker to the log\n"
      << "  hooks refresh    Re-apply file/debug hooks to newly loaded modules\n"
      << "  mods check  Find mod archives in folders that may still be scanned\n"
      << "  console save [path]  Save this session log as a .txt file\n"
      << "  freecam   Toggle Relight freecam INI setting\n"
      << "  freecam on/off/status/path\n"
      << "  clear     Clear this console\n"
      << "  detach    Unload the hook DLL and close this console\n";
}

void PrintStatus() {
  std::wcout << L"Process ID: " << GetCurrentProcessId() << L"\n";
  std::wcout << L"Hook DLL:   " << GetModulePath(g_module) << L"\n";
  std::wcout << L"Log:        " << (g_logEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Live log:   " << (g_logConsoleEnabled ? L"console on" : L"console off") << L"\n";
  std::wcout << L"Log format: " << (g_logCompactEnabled ? L"compact" : L"full") << L"\n";
  std::wcout << L"Focus:      " << FocusModeText(g_logFocusMode.load()) << L"\n";
  std::wcout << L"Failures:   " << (g_logFailuresOnly ? L"only" : L"all") << L"\n";
  std::wcout << L"Log file:   " << g_logPath << L"\n";
  bool relightFreecam = false;
  const bool hasRelightFreecam = GetRelightFreecamSetting(&relightFreecam);
  std::wcout << L"Freecam:    " << (hasRelightFreecam ? (relightFreecam ? L"Relight INI on" : L"Relight INI off") : L"Relight INI unavailable") << L"\n";
  std::wcout << L"Freecam cfg:" << RelightDevelopmentConfigPath() << L"\n";
}

void CountArchives() {
  const fs::path archives = fs::current_path() / L"Archives";
  if (!fs::exists(archives)) {
    std::wcout << L"No Archives folder found at: " << archives << L"\n";
    return;
  }

  size_t count = 0;
  for (const auto& entry : fs::directory_iterator(archives)) {
    if (entry.is_regular_file()) ++count;
  }
  std::wcout << L"Archives folder: " << archives << L"\n";
  std::wcout << L"File count: " << count << L"\n";
}

void PrintLogStatus() {
  std::wcout << L"Log:        " << (g_logEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Live log:   " << (g_logConsoleEnabled ? L"console on" : L"console off") << L"\n";
  std::wcout << L"Format:     " << (g_logCompactEnabled ? L"compact" : L"full") << L"\n";
  std::wcout << L"Focus:      " << FocusModeText(g_logFocusMode.load()) << L"\n";
  std::wcout << L"Failures:   " << (g_logFailuresOnly ? L"only" : L"all") << L"\n";
  std::wcout << L"File trace: " << (g_fileTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Debug trace:" << (g_debugStringTraceEnabled ? L" on" : L" off") << L"\n";
  std::wcout << L"Path:       " << g_logPath << L"\n";
}

void CheckModFolders() {
  const fs::path root = fs::current_path();
  std::wcout << L"Checking for mod archives outside the root Archives folder...\n";
  size_t suspicious = 0;

  std::error_code error;
  for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error)) {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;

    const fs::path path = entry.path();
    const std::wstring lower = ToLower(path.wstring());
    if (lower.find(L".ttarch") == std::wstring::npos && lower.find(L"_resdesc_") == std::wstring::npos) continue;
    if (lower.find(L"disabled") == std::wstring::npos && lower.find(L"quarantine") == std::wstring::npos && lower.find(L"_serofix_disabled") == std::wstring::npos) continue;

    ++suspicious;
    std::wcout << L"  " << path << L"\n";
  }

  if (suspicious == 0) {
    std::wcout << L"No suspicious disabled/quarantine archive files found under the game folder.\n";
  } else {
    std::wcout << L"Found " << suspicious << L" archive/resource files in disabled/quarantine-looking folders.\n";
    std::wcout << L"If the game reads them, rename the inner 'archives' folder or move the folder outside the game root.\n";
  }
}

void SaveConsoleTranscript(const std::string& requestedPath) {
  OpenLogFile();
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) fflush(g_logFile);
  }

  fs::path outputPath;
  if (requestedPath.empty()) {
    outputPath = fs::current_path() / (L"ttds-console-" + FileTimestampText() + L".txt");
  } else {
    outputPath = Utf8ToWide(requestedPath);
    if (outputPath.is_relative()) outputPath = fs::current_path() / outputPath;
    if (outputPath.extension().empty()) outputPath += L".txt";
  }

  if (!CopyTextFile(g_logPath, outputPath)) {
    std::wcout << L"Could not save console transcript to: " << outputPath << L"\n";
    return;
  }

  std::wcout << L"Saved console transcript: " << outputPath << L"\n";
  LogLine(L"console", L"saved transcript to " + outputPath.wstring());
}

void SetLogEnabled(bool enabled) {
  OpenLogFile();
  g_logEnabled = enabled;
  LogLine(L"console", enabled ? L"log on" : L"log off");
  std::cout << (enabled ? "Log enabled. Live lines will print here too.\n" : "Log disabled.\n");
  std::wcout << L"Path: " << g_logPath << L"\n";
}

void SetFreecam(bool enabled) {
  g_freecamEnabled = enabled;
  std::wstring error;
  if (!SetRelightFreecamSetting(enabled, &error)) {
    std::wcout << L"Freecam request failed: " << error << L"\n";
    LogLine(L"freecam", L"failed: " + error);
    return;
  }

  LogLine(L"freecam", enabled
      ? L"Relight FreeCameraOnlyMode=true"
      : L"Relight FreeCameraOnlyMode=false");
  std::cout << (enabled ? "Relight FreeCameraOnlyMode is ON.\n" : "Relight FreeCameraOnlyMode is OFF.\n");
  std::wcout << L"Config: " << RelightDevelopmentConfigPath() << L"\n";
  std::cout << "Reload/load a scene for Relight to apply this.\n";
}

std::vector<std::string> SplitCommand(const std::string& command) {
  std::istringstream stream(command);
  std::vector<std::string> parts;
  std::string part;
  while (stream >> part) parts.push_back(part);
  return parts;
}

DWORD WINAPI ConsoleThread(void*) {
  AllocConsole();
  SetConsoleTitleW(L"TTDS Dev Console");
  AttachConsoleStreams();
  OpenLogFile();
  const int patchedImports = InstallHooks();

  std::cout << "TTDS Dev Console injected.\n";
  std::cout << "This is v1: console, command loop, file/debug logging, freecam state.\n";
  std::cout << "Hooked import entries: " << patchedImports << "\n";
  PrintHelp();

  std::string command;
  while (true) {
    std::cout << "\nttds> ";
    if (!std::getline(std::cin, command)) break;
    const std::vector<std::string> parts = SplitCommand(command);
    const std::string verb = parts.empty() ? "" : ToLowerAscii(parts[0]);

    if (verb == "help") {
      PrintHelp();
    } else if (verb == "status") {
      PrintStatus();
    } else if (verb == "where") {
      std::wcout << fs::current_path() << L"\n";
    } else if (verb == "archives") {
      CountArchives();
    } else if (verb == "log") {
      const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "status";
      if (sub == "on") {
        SetLogEnabled(true);
      } else if (sub == "off") {
        SetLogEnabled(false);
      } else if (sub == "path") {
        std::wcout << g_logPath << L"\n";
      } else if (sub == "mark") {
        const size_t markAt = command.find("mark");
        const std::string text = markAt == std::string::npos ? "" : command.substr(markAt + 4);
        LogLine(L"mark", Utf8ToWide(text));
        std::cout << "Marked.\n";
      } else if (sub == "console") {
        if (parts.size() > 2) g_logConsoleEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else if (sub == "format") {
        if (parts.size() > 2) g_logCompactEnabled = ToLowerAscii(parts[2]) != "full";
        PrintLogStatus();
      } else if (sub == "focus") {
        if (parts.size() > 2) g_logFocusMode = ParseFocusMode(parts[2]);
        PrintLogStatus();
      } else if (sub == "failures") {
        if (parts.size() > 2) g_logFailuresOnly = ToLowerAscii(parts[2]) == "on" || ToLowerAscii(parts[2]) == "only";
        PrintLogStatus();
      } else if (sub == "files") {
        if (parts.size() > 2) g_fileTraceEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else if (sub == "debug") {
        if (parts.size() > 2) g_debugStringTraceEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else {
        PrintLogStatus();
      }
    } else if (verb == "hooks" && parts.size() > 1 && ToLowerAscii(parts[1]) == "refresh") {
      const int patched = InstallHooks();
      std::cout << "Hook refresh patched entries: " << patched << "\n";
      LogLine(L"hooks", L"refresh");
    } else if (verb == "mods") {
      const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "check";
      if (sub == "check") {
        CheckModFolders();
      } else {
        std::cout << "Unknown mods command. Try: mods check\n";
      }
    } else if (verb == "console") {
      const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "";
      if (sub == "save") {
        const size_t saveAt = command.find("save");
        std::string requestedPath;
        if (saveAt != std::string::npos) {
          requestedPath = TrimAscii(command.substr(saveAt + 4));
        }
        SaveConsoleTranscript(requestedPath);
      } else {
        std::cout << "Unknown console command. Try: console save [path]\n";
      }
    } else if (verb == "freecam") {
      const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "";
      if (sub == "on") {
        SetFreecam(true);
      } else if (sub == "off") {
        SetFreecam(false);
      } else if (sub == "status") {
        bool relightFreecam = false;
        if (GetRelightFreecamSetting(&relightFreecam)) {
          std::cout << "Relight FreeCameraOnlyMode is " << (relightFreecam ? "ON" : "OFF") << ".\n";
        } else {
          std::cout << "Relight freecam config was not found/readable.\n";
        }
        std::wcout << L"Config: " << RelightDevelopmentConfigPath() << L"\n";
      } else if (sub == "path") {
        std::wcout << RelightDevelopmentConfigPath() << L"\n";
      } else {
        bool relightFreecam = false;
        if (GetRelightFreecamSetting(&relightFreecam)) {
          SetFreecam(!relightFreecam);
        } else {
          SetFreecam(!g_freecamEnabled);
        }
      }
    } else if (verb == "clear") {
      system("cls");
    } else if (verb == "detach" || verb == "quit") {
      break;
    } else if (verb.empty()) {
      continue;
    } else {
      std::cout << "Unknown command. Type help.\n";
    }
  }

  CloseLogFile();
  FreeConsole();
  FreeLibraryAndExitThread(g_module, 0);
  return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
    HANDLE thread = CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
  }
  return TRUE;
}
