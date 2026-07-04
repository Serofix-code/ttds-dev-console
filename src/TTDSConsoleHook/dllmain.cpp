#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {
HMODULE g_module = nullptr;
std::atomic_bool g_logEnabled{false};
std::atomic_bool g_logConsoleEnabled{true};
std::atomic_bool g_logCompactEnabled{true};
std::atomic_bool g_fileTraceEnabled{true};
std::atomic_bool g_debugStringTraceEnabled{true};
std::atomic_bool g_writeTraceEnabled{true};
std::atomic_bool g_textureTraceEnabled{true};
std::atomic_bool g_cameraTraceEnabled{true};
std::atomic_bool g_logFailuresOnly{false};
std::atomic_int g_logFocusMode{1};
std::atomic_bool g_freecamEnabled{false};
std::mutex g_logMutex;
std::mutex g_handleMutex;
FILE* g_logFile = nullptr;
fs::path g_logPath;
fs::path g_relightDevelopmentConfigPath;
std::unordered_map<HANDLE, std::wstring> g_trackedHandles;
std::mutex g_cameraHookMutex;
alignas(8) volatile uintptr_t g_cameraAddressRaw = 0;
std::atomic_bool g_cameraPointerHookInstalled{false};
std::atomic_bool g_cameraLiveLogEnabled{false};
std::atomic_bool g_cameraLockEnabled{false};
std::atomic_bool g_cameraMonitorRunning{false};
std::atomic_int g_cameraLogIntervalMs{500};
HANDLE g_cameraMonitorThread = nullptr;
unsigned char* g_cameraHookTarget = nullptr;
void* g_cameraHookStub = nullptr;
unsigned char g_cameraHookOriginal[7]{};
std::mutex g_cameraLockMutex;
float g_cameraLockValues[13]{};
std::mutex g_activityMutex;
std::vector<std::wstring> g_recentFileActivity;
std::vector<std::wstring> g_recentFileFailures;
std::atomic_bool g_loadingWatchEnabled{true};
std::atomic_bool g_loadingWatchRunning{false};
std::atomic<unsigned long long> g_lastFileActivityTick{0};
std::atomic<unsigned long long> g_lastLoadingDiagnosticTick{0};
HANDLE g_loadingWatchThread = nullptr;

using CreateFileWFn = HANDLE(WINAPI*)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
using OutputDebugStringWFn = VOID(WINAPI*)(LPCWSTR);
using OutputDebugStringAFn = VOID(WINAPI*)(LPCSTR);

CreateFileWFn g_realCreateFileW = nullptr;
CreateFileAFn g_realCreateFileA = nullptr;
WriteFileFn g_realWriteFile = nullptr;
CloseHandleFn g_realCloseHandle = nullptr;
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

std::wstring ToLowerCopy(const std::wstring& value) {
  return ToLower(value);
}

std::string ToLowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

WORD LogColorFor(const std::wstring& category, const std::wstring& message, WORD originalColor) {
  const std::wstring lowerCategory = ToLowerCopy(category);
  const std::wstring lowerMessage = ToLowerCopy(message);
  if (lowerMessage.find(L"fail") != std::wstring::npos || lowerCategory == L"error") {
    return FOREGROUND_RED | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"save" || lowerCategory == L"write" || lowerMessage.find(L" save") != std::wstring::npos || lowerMessage.find(L"prefs") != std::wstring::npos) {
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"camera") {
    return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"texture") {
    return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"archive") {
    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"mod" || lowerCategory == L"relight" || lowerCategory == L"freecam") {
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  }
  if (lowerCategory == L"debug") {
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  }
  return originalColor;
}

void PrintColoredLine(WORD color, const std::wstring& text) {
  const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  const bool hasConsoleInfo = console != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(console, &info);
  const WORD originalColor = hasConsoleInfo ? info.wAttributes : static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
  if (hasConsoleInfo) SetConsoleTextAttribute(console, color);
  std::wcout << text << L"\n";
  if (hasConsoleInfo) SetConsoleTextAttribute(console, originalColor);
}

void PrintColorLegend() {
  std::wcout << L"\nColour codes:\n";
  PrintColoredLine(FOREGROUND_RED | FOREGROUND_INTENSITY, L"  bright red    0x0C  failures and errors");
  PrintColoredLine(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, L"  bright yellow 0x0E  save/prefs/write activity");
  PrintColoredLine(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, L"  bright cyan   0x0B  camera-named resource file activity");
  PrintColoredLine(FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY, L"  bright magenta 0x0D texture/txmesh resource activity");
  PrintColoredLine(FOREGROUND_GREEN | FOREGROUND_INTENSITY, L"  bright green  0x0A  archive activity");
  PrintColoredLine(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, L"  bright white  0x0F  mods, Relight, and freecam");
  PrintColoredLine(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, L"  gray/default  0x07  debug strings and ordinary lines");
  std::wcout << L"Camera note: camera-resource logs are off by default; use log cameras on.\n";
  std::wcout << L"Live POV logs are manual; use camera log on or pov log on.\n";
}

std::wstring FocusModeText(int mode) {
  switch (mode) {
    case 0: return L"all";
    case 1: return L"useful";
    case 2: return L"saves";
    case 3: return L"relight";
    case 4: return L"mods";
    case 5: return L"archives";
    case 6: return L"textures";
    case 7: return L"cameras";
    case 8: return L"resources";
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
  if (lower == "textures" || lower == "texture" || lower == "tx") return 6;
  if (lower == "cameras" || lower == "camera" || lower == "cam") return 7;
  if (lower == "resources" || lower == "resource" || lower == "assets") return 8;
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
    const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const bool hasConsoleInfo = console != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(console, &info);
    const WORD originalColor = hasConsoleInfo ? info.wAttributes : static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    const WORD color = LogColorFor(category, message, originalColor);
    if (hasConsoleInfo) SetConsoleTextAttribute(console, color);
    fwprintf(stdout, L"\n%ls\n", line.c_str());
    if (hasConsoleInfo) SetConsoleTextAttribute(console, originalColor);
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

fs::path SaveDirectoryPath() {
  wchar_t userProfile[MAX_PATH]{};
  const DWORD size = GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) return {};
  return fs::path(userProfile) / L"Documents" / L"Telltale Games" / L"The Walking Dead Definitive";
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

bool IsTexturePathLower(const std::wstring& lower) {
  return lower.find(L".d3dtx") != std::wstring::npos ||
         lower.find(L".dds") != std::wstring::npos ||
         lower.find(L".png") != std::wstring::npos ||
         lower.find(L".jpg") != std::wstring::npos ||
         lower.find(L".jpeg") != std::wstring::npos ||
         lower.find(L".tga") != std::wstring::npos ||
         lower.find(L".bmp") != std::wstring::npos ||
         lower.find(L"txmesh") != std::wstring::npos ||
         lower.find(L"texture") != std::wstring::npos ||
         lower.find(L"\\textures\\") != std::wstring::npos ||
         lower.find(L"/textures/") != std::wstring::npos;
}

bool IsMeshPathLower(const std::wstring& lower) {
  return lower.find(L".d3dmesh") != std::wstring::npos ||
         lower.find(L".mesh") != std::wstring::npos ||
         lower.find(L"_skl") != std::wstring::npos ||
         lower.find(L"_skeleton") != std::wstring::npos ||
         lower.find(L"\\meshes\\") != std::wstring::npos ||
         lower.find(L"/meshes/") != std::wstring::npos;
}

bool IsCameraPathLower(const std::wstring& lower) {
  return lower.find(L"camera") != std::wstring::npos ||
         lower.find(L"cam_") != std::wstring::npos ||
         lower.find(L"_cam") != std::wstring::npos ||
         lower.find(L".cam") != std::wstring::npos ||
         lower.find(L"cameracut") != std::wstring::npos ||
         lower.find(L"lens") != std::wstring::npos;
}

bool IsResourcePathLower(const std::wstring& lower) {
  return IsTexturePathLower(lower) ||
         IsMeshPathLower(lower) ||
         IsCameraPathLower(lower) ||
         lower.find(L".scene") != std::wstring::npos ||
         lower.find(L".chore") != std::wstring::npos ||
         lower.find(L".dlog") != std::wstring::npos;
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
         IsResourcePathLower(lower) ||
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

std::wstring FormatFileTimeText(const FILETIME& fileTime) {
  FILETIME localFileTime{};
  SYSTEMTIME systemTime{};
  FileTimeToLocalFileTime(&fileTime, &localFileTime);
  FileTimeToSystemTime(&localFileTime, &systemTime);

  wchar_t buffer[64]{};
  swprintf_s(
      buffer,
      L"%04u-%02u-%02u %02u:%02u:%02u",
      systemTime.wYear,
      systemTime.wMonth,
      systemTime.wDay,
      systemTime.wHour,
      systemTime.wMinute,
      systemTime.wSecond);
  return buffer;
}

uint64_t FileTimeValue(const FILETIME& fileTime) {
  ULARGE_INTEGER value{};
  value.LowPart = fileTime.dwLowDateTime;
  value.HighPart = fileTime.dwHighDateTime;
  return value.QuadPart;
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
  if (IsTexturePathLower(lower)) return L"texture";
  if (IsMeshPathLower(lower)) return L"mesh";
  if (IsCameraPathLower(lower)) return L"camera";
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

bool IsOwnConsoleLogPath(const std::wstring& lower) {
  const std::wstring name = ToLower(FileNameOnly(lower));
  return name == L"ttds-dev-console.log" ||
         name.find(L"ttds-console-") == 0;
}

std::wstring LogCategoryForPath(const std::wstring& path, const std::wstring& fallback) {
  const std::wstring lower = ToLower(path);
  if (IsSaveOrPrefsPath(lower)) return L"save";
  if (IsCameraPathLower(lower)) return L"camera";
  if (IsTexturePathLower(lower)) return L"texture";
  if (IsRelightPath(lower)) return L"relight";
  if (IsModPath(lower)) return L"mod";
  if (IsArchivePath(lower)) return L"archive";
  if (IsResourcePathLower(lower)) return L"resource";
  return fallback;
}

std::wstring LogCategoryForDebugMessage(const std::wstring& message) {
  const std::wstring lower = ToLower(message);
  if (IsCameraPathLower(lower) ||
      lower.find(L" fov") != std::wstring::npos ||
      lower.find(L"fieldofview") != std::wstring::npos ||
      lower.find(L"viewmatrix") != std::wstring::npos ||
      lower.find(L"projectionmatrix") != std::wstring::npos) {
    return L"camera";
  }
  if (IsTexturePathLower(lower)) return L"texture";
  if (IsRelightPath(lower)) return L"relight";
  if (IsModPath(lower)) return L"mod";
  if (IsArchivePath(lower)) return L"archive";
  if (IsSaveOrPrefsPath(lower)) return L"save";
  return L"debug";
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
  const int focus = g_logFocusMode.load();
  const std::wstring lower = ToLower(path);
  if (IsOwnConsoleLogPath(lower)) return false;
  if (focus != 0 && !LooksInterestingPath(path)) return false;
  if (g_logFailuresOnly && result != INVALID_HANDLE_VALUE) return false;

  const bool failed = result == INVALID_HANDLE_VALUE;
  const bool texture = IsTexturePathLower(lower);
  const bool camera = IsCameraPathLower(lower);

  if (texture && !g_textureTraceEnabled) return false;
  if (camera && !g_cameraTraceEnabled) return false;

  if (focus == 0) return true;
  if (focus == 2) return IsSaveOrPrefsPath(lower);
  if (focus == 3) return failed || IsRelightPath(lower);
  if (focus == 4) return failed || IsModPath(lower) || IsRelightPath(lower);
  if (focus == 5) return IsArchivePath(lower);
  if (focus == 6) return failed || texture;
  if (focus == 7) return failed || camera;
  if (focus == 8) return failed || IsResourcePathLower(lower);

  return failed ||
         IsSaveOrPrefsPath(lower) ||
         IsRelightPath(lower) ||
         IsModPath(lower) ||
         texture ||
         camera ||
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

bool ShouldTrackHandle(const std::wstring& path, HANDLE result) {
  const std::wstring lower = ToLower(path);
  return result != INVALID_HANDLE_VALUE &&
         result != nullptr &&
         !IsOwnConsoleLogPath(lower) &&
         (g_logFocusMode.load() == 0 || LooksInterestingPath(path));
}

void RememberHandle(HANDLE handle, const std::wstring& path) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  g_trackedHandles[handle] = path;
}

std::wstring ForgetHandle(HANDLE handle) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  const auto found = g_trackedHandles.find(handle);
  if (found == g_trackedHandles.end()) return L"";
  const std::wstring path = found->second;
  g_trackedHandles.erase(found);
  return path;
}

std::wstring PathForHandle(HANDLE handle) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  const auto found = g_trackedHandles.find(handle);
  return found == g_trackedHandles.end() ? L"" : found->second;
}

bool ShouldLogWriteEvent(const std::wstring& path, BOOL ok) {
  if (path.empty()) return false;
  const int focus = g_logFocusMode.load();
  const std::wstring lower = ToLower(path);
  if (IsOwnConsoleLogPath(lower)) return false;
  if (focus != 0 && !LooksInterestingPath(path)) return false;
  if (g_logFailuresOnly && ok) return false;

  const bool failed = !ok;
  const bool texture = IsTexturePathLower(lower);
  const bool camera = IsCameraPathLower(lower);

  if (texture && !g_textureTraceEnabled) return false;
  if (camera && !g_cameraTraceEnabled) return false;

  if (focus == 0) return true;
  if (focus == 2) return IsSaveOrPrefsPath(lower);
  if (focus == 3) return failed || IsRelightPath(lower);
  if (focus == 4) return failed || IsModPath(lower) || IsRelightPath(lower);
  if (focus == 5) return IsArchivePath(lower);
  if (focus == 6) return failed || texture;
  if (focus == 7) return failed || camera;
  if (focus == 8) return failed || IsResourcePathLower(lower);

  return failed ||
         IsSaveOrPrefsPath(lower) ||
         IsRelightPath(lower) ||
         IsModPath(lower) ||
         texture ||
         camera;
}

std::wstring FormatWriteEvent(BOOL ok, DWORD requestedBytes, DWORD actualBytes, const std::wstring& path) {
  std::wstringstream message;
  message << (ok ? L"OK   " : L"FAIL ")
          << L"WRITE "
          << FileKind(path) << L"  "
          << CompactPath(path)
          << L"  bytes=" << actualBytes;
  if (requestedBytes != actualBytes) {
    message << L"/" << requestedBytes;
  }
  return message.str();
}

void PushRecent(std::vector<std::wstring>* values, const std::wstring& value, size_t limit) {
  values->push_back(value);
  if (values->size() > limit) {
    values->erase(values->begin(), values->begin() + static_cast<std::ptrdiff_t>(values->size() - limit));
  }
}

void RecordFileActivity(const std::wstring& operation, const std::wstring& path, bool ok) {
  g_lastFileActivityTick = GetTickCount64();
  std::wstringstream entry;
  entry << (ok ? L"OK " : L"FAIL ") << operation << L" " << FileKind(path) << L" " << CompactPath(path);

  std::lock_guard<std::mutex> lock(g_activityMutex);
  PushRecent(&g_recentFileActivity, entry.str(), 16);
  if (!ok) {
    PushRecent(&g_recentFileFailures, entry.str(), 16);
  }
}

std::wstring JoinRecent(const std::vector<std::wstring>& values) {
  if (values.empty()) return L"(none)";
  std::wstringstream joined;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) joined << L" | ";
    joined << values[i];
  }
  return joined.str();
}

void LogLoadingDiagnostic() {
  std::vector<std::wstring> activity;
  std::vector<std::wstring> failures;
  {
    std::lock_guard<std::mutex> lock(g_activityMutex);
    activity = g_recentFileActivity;
    failures = g_recentFileFailures;
  }

  const unsigned long long now = GetTickCount64();
  const unsigned long long lastActivity = g_lastFileActivityTick.load();
  const unsigned long long quietSeconds = lastActivity == 0 ? 0 : (now - lastActivity) / 1000;

  LogLine(L"watchdog", L"possible long load/stall: no tracked file activity for " + std::to_wstring(quietSeconds) + L" seconds");
  LogLine(L"watchdog", L"recent activity: " + JoinRecent(activity));
  LogLine(L"watchdog", L"recent failures: " + JoinRecent(failures));
  LogLine(L"watchdog", L"tips: check the last FAIL line, disabled/quarantine mod folders, missing archives, and huge texture/txmesh mods");
}

DWORD WINAPI LoadingWatchThread(void*) {
  while (g_loadingWatchRunning) {
    if (g_logEnabled && g_loadingWatchEnabled) {
      const unsigned long long now = GetTickCount64();
      const unsigned long long lastActivity = g_lastFileActivityTick.load();
      const unsigned long long lastDiagnostic = g_lastLoadingDiagnosticTick.load();
      if (lastActivity != 0 && now > lastActivity + 60000 && now > lastDiagnostic + 60000) {
        g_lastLoadingDiagnosticTick = now;
        LogLoadingDiagnostic();
      }
    }
    Sleep(5000);
  }
  return 0;
}

void StartLoadingWatchThread() {
  if (g_loadingWatchRunning) return;
  g_loadingWatchRunning = true;
  g_loadingWatchThread = CreateThread(nullptr, 0, LoadingWatchThread, nullptr, 0, nullptr);
  if (!g_loadingWatchThread) {
    g_loadingWatchRunning = false;
  }
}

void StopLoadingWatchThread() {
  if (!g_loadingWatchRunning) return;
  g_loadingWatchRunning = false;
  if (g_loadingWatchThread) {
    WaitForSingleObject(g_loadingWatchThread, 1000);
    CloseHandle(g_loadingWatchThread);
    g_loadingWatchThread = nullptr;
  }
}

struct SaveCandidate {
  fs::path path;
  std::wstring kind;
  int priority = 99;
  uint64_t writeTimeValue = 0;
  uintmax_t size = 0;
  FILETIME writeTime{};
};

int SavePriority(const std::wstring& lowerName, std::wstring* kind) {
  if (lowerName.find(L"quick") != std::wstring::npos) {
    if (kind) *kind = L"quicksave";
    return 0;
  }
  if (lowerName.find(L"autosave") != std::wstring::npos) {
    if (kind) *kind = L"autosave";
    return 1;
  }
  if (lowerName.find(L"checkpoint") != std::wstring::npos) {
    if (kind) *kind = L"checkpoint";
    return 2;
  }
  if (lowerName.find(L"saveslot") != std::wstring::npos) {
    if (kind) *kind = L"save";
    return 3;
  }
  return 99;
}

std::vector<SaveCandidate> FindSaveCandidates() {
  std::vector<SaveCandidate> candidates;
  const fs::path saveDir = SaveDirectoryPath();
  if (saveDir.empty() || !fs::exists(saveDir)) return candidates;

  std::error_code error;
  for (const auto& entry : fs::directory_iterator(saveDir, error)) {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;

    const fs::path path = entry.path();
    if (_wcsicmp(path.extension().c_str(), L".bundle") != 0) continue;

    const std::wstring lowerName = ToLower(path.filename().wstring());
    if (lowerName == L"menu.bundle" || lowerName == L"global.bundle") continue;

    std::wstring kind;
    const int priority = SavePriority(lowerName, &kind);
    if (priority >= 99) continue;

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) continue;

    SaveCandidate candidate;
    candidate.path = path;
    candidate.kind = kind;
    candidate.priority = priority;
    candidate.writeTime = attributes.ftLastWriteTime;
    candidate.writeTimeValue = FileTimeValue(attributes.ftLastWriteTime);
    candidate.size = entry.file_size(error);
    candidates.push_back(candidate);
  }

  std::sort(candidates.begin(), candidates.end(), [](const SaveCandidate& left, const SaveCandidate& right) {
    if (left.priority != right.priority) return left.priority < right.priority;
    return left.writeTimeValue > right.writeTimeValue;
  });
  return candidates;
}

void PrintSaveCandidate(const SaveCandidate& candidate, size_t index) {
  std::wcout << L"  " << index << L". [" << candidate.kind << L"] "
             << candidate.path.filename().wstring()
             << L" | " << FormatFileTimeText(candidate.writeTime)
             << L" | " << candidate.size << L" bytes\n";
}

void HandleReloadCommand(const std::string& sub) {
  std::vector<SaveCandidate> candidates = FindSaveCandidates();
  const fs::path saveDir = SaveDirectoryPath();
  if (candidates.empty()) {
    std::wcout << L"No quicksave/autosave/checkpoint/save bundles found in: " << saveDir << L"\n";
    LogLine(L"reload", L"no save candidates found in " + saveDir.wstring());
    return;
  }

  if (sub == "list") {
    std::wcout << L"Reload candidates from: " << saveDir << L"\n";
    const size_t count = std::min<size_t>(candidates.size(), 12);
    for (size_t i = 0; i < count; ++i) {
      PrintSaveCandidate(candidates[i], i + 1);
    }
    if (candidates.size() > count) {
      std::wcout << L"  ... " << (candidates.size() - count) << L" more\n";
    }
    return;
  }

  const SaveCandidate& candidate = candidates.front();
  std::wcout << L"Newest reload candidate:\n";
  PrintSaveCandidate(candidate, 1);
  std::wcout << L"Path: " << candidate.path << L"\n";
  std::wcout << L"Live engine reload is not attached yet. This command only finds the safest save candidate for now.\n";
  std::wcout << L"Use 'reload list' to inspect candidates; a true reload needs a Telltale engine/Lua reload function hook.\n";
  LogLine(L"reload", L"candidate " + candidate.kind + L" " + candidate.path.wstring());
}

struct CameraState {
  uintptr_t base = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float pitch = 0.0f;
  float yaw = 0.0f;
  float roll = 0.0f;
  float fov = 0.0f;
};

float ClampFloat(float value, float minimum, float maximum) {
  return std::max(minimum, std::min(maximum, value));
}

float RadiansToDegrees(float radians) {
  return radians * 57.29577951308232f;
}

bool SafeReadFloat(uintptr_t address, float* value) {
  __try {
    *value = *reinterpret_cast<volatile float*>(address);
    return std::isfinite(*value);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool IsReadableAddress(uintptr_t address, size_t bytes) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
  if (info.State != MEM_COMMIT) return false;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
  const uintptr_t regionEnd = regionStart + info.RegionSize;
  return address >= regionStart && address + bytes <= regionEnd;
}

bool SafeWriteMemory(uintptr_t address, const void* data, size_t bytes) {
  DWORD oldProtect = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(address), bytes, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
  bool ok = false;
  __try {
    memcpy(reinterpret_cast<void*>(address), data, bytes);
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(address), bytes, oldProtect, &ignored);
  return ok;
}

bool SafeWriteFloat(uintptr_t address, float value) {
  return SafeWriteMemory(address, &value, sizeof(value));
}

bool SafeWriteWord(uintptr_t address, uint16_t value) {
  return SafeWriteMemory(address, &value, sizeof(value));
}

bool ReadCameraState(CameraState* state) {
  const uintptr_t base = g_cameraAddressRaw;
  if (base < 0x10000 || !IsReadableAddress(base, 0x1CC)) return false;

  CameraState next;
  next.base = base;
  if (!SafeReadFloat(base + 0x138, &next.x)) return false;
  if (!SafeReadFloat(base + 0x130, &next.y)) return false;
  if (!SafeReadFloat(base + 0x134, &next.z)) return false;
  if (!SafeReadFloat(base + 0x1C8, &next.fov)) return false;

  float rot2 = 0.0f;
  float rot5 = 0.0f;
  float rot7 = 0.0f;
  float rot8 = 0.0f;
  float rot9 = 0.0f;
  if (SafeReadFloat(base + 0x104, &rot2) &&
      SafeReadFloat(base + 0x114, &rot5) &&
      SafeReadFloat(base + 0x120, &rot7) &&
      SafeReadFloat(base + 0x124, &rot8) &&
      SafeReadFloat(base + 0x128, &rot9)) {
    const float pitch = std::asin(ClampFloat(rot8, -1.0f, 1.0f));
    const float cp = std::cos(pitch);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (std::fabs(cp) > 0.0001f) {
      yaw = std::acos(ClampFloat(rot9 / cp, -1.0f, 1.0f));
      if (rot7 < 0.0f) yaw = -yaw;
      roll = std::acos(ClampFloat(rot5 / cp, -1.0f, 1.0f));
      if (rot2 < 0.0f) roll = -roll;
    }
    next.pitch = RadiansToDegrees(pitch);
    next.yaw = RadiansToDegrees(yaw);
    next.roll = RadiansToDegrees(roll);
  }

  *state = next;
  return true;
}

const uintptr_t* CameraLockOffsets(size_t* count) {
  static const uintptr_t offsets[] = {
      0x100, 0x104, 0x108,
      0x110, 0x114, 0x118,
      0x120, 0x124, 0x128,
      0x130, 0x134, 0x138,
      0x1C8};
  *count = sizeof(offsets) / sizeof(offsets[0]);
  return offsets;
}

bool CaptureCameraLock(std::wstring* message) {
  const uintptr_t base = g_cameraAddressRaw;
  if (base < 0x10000 || !IsReadableAddress(base, 0x1CC)) {
    if (message) *message = L"camera pointer is not ready; load a scene and try camera lock on again";
    return false;
  }

  size_t count = 0;
  const uintptr_t* offsets = CameraLockOffsets(&count);
  float values[13]{};
  for (size_t i = 0; i < count; ++i) {
    if (!SafeReadFloat(base + offsets[i], &values[i])) {
      if (message) *message = L"could not read camera values for lock";
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_cameraLockMutex);
    memcpy(g_cameraLockValues, values, sizeof(values));
  }

  if (message) *message = L"POV lock captured current camera position/rotation/FOV";
  return true;
}

bool ApplyCameraLock() {
  const uintptr_t base = g_cameraAddressRaw;
  if (base < 0x10000 || !IsReadableAddress(base, 0x1CC)) return false;

  float values[13]{};
  {
    std::lock_guard<std::mutex> lock(g_cameraLockMutex);
    memcpy(values, g_cameraLockValues, sizeof(values));
  }

  size_t count = 0;
  const uintptr_t* offsets = CameraLockOffsets(&count);
  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    ok = SafeWriteFloat(base + offsets[i], values[i]) && ok;
  }
  SafeWriteWord(base + 0x1C2, 0x0101);
  return ok;
}

bool SetCameraFov(float fov, std::wstring* message) {
  if (fov < 1.2f) fov = 1.2f;
  if (fov > 169.0f) fov = 169.0f;

  const uintptr_t base = g_cameraAddressRaw;
  if (base < 0x10000 || !IsReadableAddress(base, 0x1CC)) {
    if (message) *message = L"camera pointer is not ready; load a scene and try camera fov again";
    return false;
  }

  if (!SafeWriteFloat(base + 0x1C8, fov)) {
    if (message) *message = L"could not write camera FOV";
    return false;
  }
  SafeWriteWord(base + 0x1C2, 0x0101);

  if (g_cameraLockEnabled) {
    std::lock_guard<std::mutex> lock(g_cameraLockMutex);
    g_cameraLockValues[12] = fov;
  }

  if (message) {
    std::wstringstream text;
    text << std::fixed << std::setprecision(2) << L"camera FOV set to " << fov;
    *message = text.str();
  }
  return true;
}

bool CameraStateChanged(const CameraState& left, const CameraState& right) {
  return left.base != right.base ||
         std::fabs(left.x - right.x) > 0.01f ||
         std::fabs(left.y - right.y) > 0.01f ||
         std::fabs(left.z - right.z) > 0.01f ||
         std::fabs(left.pitch - right.pitch) > 0.05f ||
         std::fabs(left.yaw - right.yaw) > 0.05f ||
         std::fabs(left.roll - right.roll) > 0.05f ||
         std::fabs(left.fov - right.fov) > 0.05f;
}

std::wstring FormatCameraState(const CameraState& state) {
  std::wstringstream text;
  text << std::fixed << std::setprecision(3)
       << L"POV ptr=0x" << std::hex << state.base << std::dec
       << L" pos=(" << state.x << L", " << state.y << L", " << state.z << L")"
       << L" rot=(" << state.pitch << L", " << state.yaw << L", " << state.roll << L")"
       << L" fov=" << state.fov;
  return text.str();
}

unsigned char* FindPatternInMainModule(const unsigned char* pattern, size_t patternSize) {
  auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  if (!base) return nullptr;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

  const size_t imageSize = nt->OptionalHeader.SizeOfImage;
  if (imageSize < patternSize) return nullptr;
  for (size_t i = 0; i <= imageSize - patternSize; ++i) {
    if (memcmp(base + i, pattern, patternSize) == 0) return base + i;
  }
  return nullptr;
}

bool IsRel32Reachable(uintptr_t fromInstruction, uintptr_t target) {
  const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(fromInstruction + 5);
  return delta >= INT32_MIN && delta <= INT32_MAX;
}

void AppendU64(std::vector<unsigned char>* bytes, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    bytes->push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xFF));
  }
}

void AppendRel32Jump(std::vector<unsigned char>* bytes, uintptr_t instructionAddress, uintptr_t targetAddress) {
  const int64_t delta = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 5);
  const int32_t relative = static_cast<int32_t>(delta);
  bytes->push_back(0xE9);
  for (int i = 0; i < 4; ++i) {
    bytes->push_back(static_cast<unsigned char>((static_cast<uint32_t>(relative) >> (i * 8)) & 0xFF));
  }
}

void* AllocateNear(uintptr_t target, size_t size) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const uintptr_t granularity = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
  const uintptr_t alignedTarget = target & ~(granularity - 1);
  const uintptr_t maxDistance = 0x70000000ULL;

  for (uintptr_t delta = 0; delta < maxDistance; delta += granularity) {
    const uintptr_t candidates[] = {
        alignedTarget + delta,
        alignedTarget > delta ? alignedTarget - delta : 0};
    for (uintptr_t candidate : candidates) {
      if (!candidate) continue;
      if (!IsRel32Reachable(target, candidate)) continue;
      void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (memory) return memory;
    }
  }
  return nullptr;
}

bool InstallCameraPointerHook(std::wstring* message) {
  std::lock_guard<std::mutex> lock(g_cameraHookMutex);
  if (g_cameraPointerHookInstalled) {
    if (message) *message = L"camera pointer hook already installed";
    return true;
  }

  const unsigned char pattern[] = {0x80, 0xB9, 0xC3, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9};
  unsigned char* target = FindPatternInMainModule(pattern, sizeof(pattern));
  if (!target) {
    if (message) *message = L"camera AOB was not found in WDC.exe";
    return false;
  }

  void* stub = AllocateNear(reinterpret_cast<uintptr_t>(target), 128);
  if (!stub) {
    if (message) *message = L"could not allocate nearby camera hook stub";
    return false;
  }

  memcpy(g_cameraHookOriginal, target, sizeof(g_cameraHookOriginal));

  std::vector<unsigned char> stubBytes;
  stubBytes.reserve(64);
  stubBytes.push_back(0x50);  // push rax
  stubBytes.push_back(0x48);  // mov rax, &g_cameraAddressRaw
  stubBytes.push_back(0xB8);
  AppendU64(&stubBytes, reinterpret_cast<uint64_t>(&g_cameraAddressRaw));
  stubBytes.push_back(0x48);  // mov [rax], rcx
  stubBytes.push_back(0x89);
  stubBytes.push_back(0x08);
  stubBytes.push_back(0x58);  // pop rax
  for (size_t i = 0; i < sizeof(g_cameraHookOriginal); ++i) {
    stubBytes.push_back(g_cameraHookOriginal[i]);
  }
  AppendRel32Jump(
      &stubBytes,
      reinterpret_cast<uintptr_t>(stub) + stubBytes.size(),
      reinterpret_cast<uintptr_t>(target) + sizeof(g_cameraHookOriginal));

  memcpy(stub, stubBytes.data(), stubBytes.size());

  DWORD oldProtect = 0;
  if (!VirtualProtect(target, sizeof(g_cameraHookOriginal), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    VirtualFree(stub, 0, MEM_RELEASE);
    if (message) *message = L"could not change camera hook page protection";
    return false;
  }

  std::vector<unsigned char> patch;
  AppendRel32Jump(&patch, reinterpret_cast<uintptr_t>(target), reinterpret_cast<uintptr_t>(stub));
  patch.push_back(0x90);
  patch.push_back(0x90);
  memcpy(target, patch.data(), patch.size());
  DWORD ignored = 0;
  VirtualProtect(target, sizeof(g_cameraHookOriginal), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), target, sizeof(g_cameraHookOriginal));

  g_cameraHookTarget = target;
  g_cameraHookStub = stub;
  g_cameraPointerHookInstalled = true;
  if (message) {
    std::wstringstream status;
    status << L"camera pointer hook installed at 0x" << std::hex << reinterpret_cast<uintptr_t>(target);
    *message = status.str();
  }
  return true;
}

void UninstallCameraPointerHook() {
  std::lock_guard<std::mutex> lock(g_cameraHookMutex);
  if (!g_cameraPointerHookInstalled || !g_cameraHookTarget) return;

  DWORD oldProtect = 0;
  if (VirtualProtect(g_cameraHookTarget, sizeof(g_cameraHookOriginal), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    memcpy(g_cameraHookTarget, g_cameraHookOriginal, sizeof(g_cameraHookOriginal));
    DWORD ignored = 0;
    VirtualProtect(g_cameraHookTarget, sizeof(g_cameraHookOriginal), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_cameraHookTarget, sizeof(g_cameraHookOriginal));
  }
  if (g_cameraHookStub) {
    VirtualFree(g_cameraHookStub, 0, MEM_RELEASE);
  }
  g_cameraAddressRaw = 0;
  g_cameraHookTarget = nullptr;
  g_cameraHookStub = nullptr;
  g_cameraPointerHookInstalled = false;
}

DWORD WINAPI CameraMonitorThread(void*) {
  CameraState last;
  bool hasLast = false;
  bool waitingLogged = false;
  while (g_cameraMonitorRunning) {
    if (g_cameraLockEnabled) {
      ApplyCameraLock();
    }
    if (g_cameraLiveLogEnabled && g_logEnabled) {
      CameraState current;
      if (ReadCameraState(&current)) {
        waitingLogged = false;
        if (!hasLast || CameraStateChanged(last, current)) {
          LogLine(L"camera", FormatCameraState(current));
          last = current;
          hasLast = true;
        }
      } else if (!waitingLogged) {
        LogLine(L"camera", L"waiting for active camera pointer");
        waitingLogged = true;
        hasLast = false;
      }
    }
    Sleep(static_cast<DWORD>(std::max(100, g_cameraLogIntervalMs.load())));
  }
  return 0;
}

void StartCameraMonitorThread() {
  if (g_cameraMonitorRunning) return;
  g_cameraMonitorRunning = true;
  g_cameraMonitorThread = CreateThread(nullptr, 0, CameraMonitorThread, nullptr, 0, nullptr);
  if (!g_cameraMonitorThread) {
    g_cameraMonitorRunning = false;
  }
}

void StopCameraMonitorThread() {
  if (!g_cameraMonitorRunning) return;
  g_cameraMonitorRunning = false;
  if (g_cameraMonitorThread) {
    WaitForSingleObject(g_cameraMonitorThread, 1000);
    CloseHandle(g_cameraMonitorThread);
    g_cameraMonitorThread = nullptr;
  }
}

void PrintCameraStatus() {
  std::wcout << L"Camera hook: " << (g_cameraPointerHookInstalled ? L"installed" : L"not installed") << L"\n";
  std::wcout << L"POV log:     " << (g_cameraLiveLogEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"POV lock:    " << (g_cameraLockEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Interval:    " << g_cameraLogIntervalMs.load() << L" ms\n";
  CameraState state;
  if (ReadCameraState(&state)) {
    std::wcout << FormatCameraState(state) << L"\n";
  } else {
    std::wcout << L"Camera pointer is not available yet. Load a scene or move the camera, then try again.\n";
  }
}

void HandleCameraCommand(const std::vector<std::string>& parts) {
  const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "status";
  if (sub == "help") {
    std::cout
        << "Camera/POV commands:\n"
        << "  camera hook           Install the read-only CE-style camera pointer hook\n"
        << "  camera status         Print live camera pointer, position, rotation, and FOV\n"
        << "  camera log on [ms]    Log live POV changes; default interval is 500 ms\n"
        << "  camera log off        Stop live POV logging\n"
        << "  camera fov [value]    Print or set FOV, clamped to 1.2..169\n"
        << "  camera lock on/off    Lock/unlock the current POV position, rotation, and FOV\n"
        << "  camera interval <ms>  Set POV logging interval\n"
        << "  pov ...               Alias for camera ...\n";
    return;
  }

  if (sub == "hook") {
    std::wstring message;
    const bool ok = InstallCameraPointerHook(&message);
    std::wcout << message << L"\n";
    LogLine(ok ? L"camera" : L"error", message);
    return;
  }

  if (sub == "fov") {
    std::wstring message;
    const bool hookOk = InstallCameraPointerHook(&message);
    if (!hookOk) {
      std::wcout << message << L"\n";
      LogLine(L"error", message);
      return;
    }
    if (parts.size() > 2) {
      const float fov = static_cast<float>(atof(parts[2].c_str()));
      const bool ok = SetCameraFov(fov, &message);
      std::wcout << message << L"\n";
      LogLine(ok ? L"camera" : L"error", message);
      return;
    }
    CameraState state;
    if (ReadCameraState(&state)) {
      std::wcout << L"Current FOV: " << state.fov << L"\n";
    } else {
      std::wcout << L"Camera pointer is not available yet.\n";
    }
    return;
  }

  if (sub == "lock") {
    const std::string state = parts.size() > 2 ? ToLowerAscii(parts[2]) : "status";
    if (state == "on") {
      std::wstring message;
      const bool hookOk = InstallCameraPointerHook(&message);
      if (!hookOk) {
        std::wcout << message << L"\n";
        LogLine(L"error", message);
        return;
      }
      const bool captured = CaptureCameraLock(&message);
      if (captured) {
        g_cameraLockEnabled = true;
        StartCameraMonitorThread();
      }
      std::wcout << message << L"\n";
      LogLine(captured ? L"camera" : L"error", captured ? L"POV lock on" : message);
      return;
    }
    if (state == "off") {
      g_cameraLockEnabled = false;
      std::cout << "POV lock is OFF.\n";
      LogLine(L"camera", L"POV lock off");
      return;
    }
    std::wcout << L"POV lock: " << (g_cameraLockEnabled ? L"on" : L"off") << L"\n";
    return;
  }

  if (sub == "log" || sub == "on" || sub == "off") {
    const std::string state = sub == "log"
        ? (parts.size() > 2 ? ToLowerAscii(parts[2]) : "status")
        : sub;
    if (state == "on") {
      if (parts.size() > 3) {
        g_cameraLogIntervalMs = std::max(100, atoi(parts[3].c_str()));
      }
      std::wstring message;
      const bool ok = InstallCameraPointerHook(&message);
      StartCameraMonitorThread();
      g_cameraLiveLogEnabled = ok;
      std::wcout << message << L"\n";
      std::cout << (ok ? "POV logging is ON.\n" : "POV logging could not start.\n");
      LogLine(ok ? L"camera" : L"error", ok ? L"POV logging on" : message);
      return;
    }
    if (state == "off") {
      g_cameraLiveLogEnabled = false;
      std::cout << "POV logging is OFF.\n";
      LogLine(L"camera", L"POV logging off");
      return;
    }
  }

  if (sub == "interval") {
    if (parts.size() > 2) {
      g_cameraLogIntervalMs = std::max(100, atoi(parts[2].c_str()));
    }
    std::wcout << L"POV logging interval: " << g_cameraLogIntervalMs.load() << L" ms\n";
    return;
  }

  PrintCameraStatus();
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

  if (fileName) {
    const std::wstring path(fileName);
    const std::wstring lower = ToLower(path);
    if (!IsOwnConsoleLogPath(lower) && LooksInterestingPath(path)) {
      RecordFileActivity(AccessVerb(desiredAccess), path, result != INVALID_HANDLE_VALUE);
    }
    if (ShouldTrackHandle(path, result)) {
      RememberHandle(result, path);
    }
    if (!g_insideHook && g_logEnabled && g_fileTraceEnabled) {
      g_insideHook = true;
      if (ShouldLogFileEvent(path, result)) {
        LogLine(LogCategoryForPath(path, L"file"), FormatFileEvent(L"CreateFileW", desiredAccess, creationDisposition, result, path));
      }
      g_insideHook = false;
    }
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

  if (fileName) {
    const std::wstring path = Utf8ToWide(fileName);
    const std::wstring lower = ToLower(path);
    if (!IsOwnConsoleLogPath(lower) && LooksInterestingPath(path)) {
      RecordFileActivity(AccessVerb(desiredAccess), path, result != INVALID_HANDLE_VALUE);
    }
    if (ShouldTrackHandle(path, result)) {
      RememberHandle(result, path);
    }
    if (!g_insideHook && g_logEnabled && g_fileTraceEnabled) {
      g_insideHook = true;
      if (ShouldLogFileEvent(path, result)) {
        LogLine(LogCategoryForPath(path, L"file"), FormatFileEvent(L"CreateFileA", desiredAccess, creationDisposition, result, path));
      }
      g_insideHook = false;
    }
  }

  return result;
}

BOOL WINAPI HookWriteFile(
    HANDLE file,
    LPCVOID buffer,
    DWORD numberOfBytesToWrite,
    LPDWORD numberOfBytesWritten,
    LPOVERLAPPED overlapped) {
  if (!g_realWriteFile) {
    g_realWriteFile = reinterpret_cast<WriteFileFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteFile"));
  }

  const BOOL ok = g_realWriteFile(file, buffer, numberOfBytesToWrite, numberOfBytesWritten, overlapped);

  if (!g_insideHook && g_logEnabled && g_writeTraceEnabled) {
    const std::wstring path = PathForHandle(file);
    if (!path.empty()) {
      const std::wstring lower = ToLower(path);
      if (!IsOwnConsoleLogPath(lower) && LooksInterestingPath(path)) {
        RecordFileActivity(L"WRITE", path, ok != FALSE);
      }
    }
    if (ShouldLogWriteEvent(path, ok)) {
      const DWORD actualBytes = numberOfBytesWritten ? *numberOfBytesWritten : (ok ? numberOfBytesToWrite : 0);
      g_insideHook = true;
      LogLine(LogCategoryForPath(path, L"write"), FormatWriteEvent(ok, numberOfBytesToWrite, actualBytes, path));
      g_insideHook = false;
    }
  }

  return ok;
}

BOOL WINAPI HookCloseHandle(HANDLE object) {
  if (!g_realCloseHandle) {
    g_realCloseHandle = reinterpret_cast<CloseHandleFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CloseHandle"));
  }

  const BOOL ok = g_realCloseHandle(object);
  if (ok) {
    ForgetHandle(object);
  }
  return ok;
}

VOID WINAPI HookOutputDebugStringW(LPCWSTR message) {
  if (!g_realOutputDebugStringW) {
    g_realOutputDebugStringW = reinterpret_cast<OutputDebugStringWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringW"));
  }
  if (!g_insideHook && g_logEnabled && g_debugStringTraceEnabled && message) {
    g_insideHook = true;
    const std::wstring category = LogCategoryForDebugMessage(message);
    if (category != L"camera" || g_cameraTraceEnabled) {
      LogLine(category, message);
    }
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
    const std::wstring wideMessage = Utf8ToWide(message);
    const std::wstring category = LogCategoryForDebugMessage(wideMessage);
    if (category != L"camera" || g_cameraTraceEnabled) {
      LogLine(category, wideMessage);
    }
    g_insideHook = false;
  }
  g_realOutputDebugStringA(message);
}

int InstallHooks() {
  g_realCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
  g_realCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileA"));
  g_realWriteFile = reinterpret_cast<WriteFileFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteFile"));
  g_realCloseHandle = reinterpret_cast<CloseHandleFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CloseHandle"));
  g_realOutputDebugStringW = reinterpret_cast<OutputDebugStringWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringW"));
  g_realOutputDebugStringA = reinterpret_cast<OutputDebugStringAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "OutputDebugStringA"));

  int patched = 0;
  for (HMODULE module : GetProcessModules()) {
    patched += PatchImport(module, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_realCreateFileW)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_realCreateFileA)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "WriteFile", reinterpret_cast<void*>(&HookWriteFile), reinterpret_cast<void**>(&g_realWriteFile)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "CloseHandle", reinterpret_cast<void*>(&HookCloseHandle), reinterpret_cast<void**>(&g_realCloseHandle)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "OutputDebugStringW", reinterpret_cast<void*>(&HookOutputDebugStringW), reinterpret_cast<void**>(&g_realOutputDebugStringW)) ? 1 : 0;
    patched += PatchImport(module, "KERNEL32.dll", "OutputDebugStringA", reinterpret_cast<void*>(&HookOutputDebugStringA), reinterpret_cast<void**>(&g_realOutputDebugStringA)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_realCreateFileW)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA), reinterpret_cast<void**>(&g_realCreateFileA)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "WriteFile", reinterpret_cast<void*>(&HookWriteFile), reinterpret_cast<void**>(&g_realWriteFile)) ? 1 : 0;
    patched += PatchImport(module, "KERNELBASE.dll", "CloseHandle", reinterpret_cast<void*>(&HookCloseHandle), reinterpret_cast<void**>(&g_realCloseHandle)) ? 1 : 0;
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
      << "  colors    Show colour code legend\n"
      << "  colours   Show colour code legend\n"
      << "  status    Show process and hook info\n"
      << "  where     Show current working directory\n"
      << "  archives  Count files in the Archives folder\n"
      << "  log       Show log status\n"
      << "  log on    Start writing ttds-dev-console.log; live POV logging stays off until camera log on\n"
      << "  log off   Stop writing the log\n"
      << "  log console on/off  Show or hide live log lines in this console\n"
      << "  log format compact/full  Change file log readability\n"
      << "  log focus useful/all/saves/relight/mods/archives/textures/cameras/resources\n"
      << "  log failures on/off  Only show failed file opens\n"
      << "  log files on/off  Track file-open activity\n"
      << "  log writes on/off  Track actual writes to save/prefs/resource handles\n"
      << "  log textures on/off  Track texture/txmesh resource file activity\n"
      << "  log cameras on/off  Track camera-ish scene/chore resource file activity\n"
      << "  log resources on/off  Toggle texture and camera resource activity together\n"
      << "  log debug on/off  Track OutputDebugString messages\n"
      << "  log path  Print the log file path\n"
      << "  log mark <text>  Add a marker to the log\n"
      << "  file info <path>  Show file size/timestamp\n"
      << "  file load <path>  Test-read/preload a .ttarch/.ttarch2 or any file\n"
      << "  loading status/on/off/diagnose  60-second long-load watchdog\n"
      << "  watchdog ...  Alias for loading ...\n"
      << "  hooks refresh    Re-apply file/debug hooks to newly loaded modules\n"
      << "  mods check  Find mod archives in folders that may still be scanned\n"
      << "  console save [path]  Save this session log as a .txt file\n"
      << "  reload    Find newest quicksave/autosave/checkpoint/save; live engine reload is not hooked yet\n"
      << "  reload list  List reload candidates\n"
      << "  camera hook/status/log on/log off/interval <ms>/fov <value>/lock on/off\n"
      << "  pov ...  Alias for camera ...\n"
      << "  freecam   Toggle Relight freecam INI setting\n"
      << "  freecam on/off/status/path\n"
      << "  clear     Clear this console\n"
      << "  detach    Unload the hook DLL and close this console\n";
  PrintColorLegend();
}

void PrintStatus() {
  std::wcout << L"Process ID: " << GetCurrentProcessId() << L"\n";
  std::wcout << L"Hook DLL:   " << GetModulePath(g_module) << L"\n";
  std::wcout << L"Log:        " << (g_logEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Live log:   " << (g_logConsoleEnabled ? L"console on" : L"console off") << L"\n";
  std::wcout << L"Log format: " << (g_logCompactEnabled ? L"compact" : L"full") << L"\n";
  std::wcout << L"Focus:      " << FocusModeText(g_logFocusMode.load()) << L"\n";
  std::wcout << L"Failures:   " << (g_logFailuresOnly ? L"only" : L"all") << L"\n";
  std::wcout << L"File trace: " << (g_fileTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Write trace:" << (g_writeTraceEnabled ? L" on" : L" off") << L"\n";
  std::wcout << L"Textures:   " << (g_textureTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Cameras:    " << (g_cameraTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"POV hook:   " << (g_cameraPointerHookInstalled ? L"installed" : L"not installed") << L"\n";
  std::wcout << L"POV log:    " << (g_cameraLiveLogEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"POV lock:   " << (g_cameraLockEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Watchdog:   " << (g_loadingWatchEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Debug trace:" << (g_debugStringTraceEnabled ? L" on" : L" off") << L"\n";
  std::wcout << L"Log file:   " << g_logPath << L"\n";
  CameraState state;
  if (ReadCameraState(&state)) {
    std::wcout << FormatCameraState(state) << L"\n";
  }
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
  std::wcout << L"Write trace:" << (g_writeTraceEnabled ? L" on" : L" off") << L"\n";
  std::wcout << L"Textures:   " << (g_textureTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Cameras:    " << (g_cameraTraceEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"POV hook:   " << (g_cameraPointerHookInstalled ? L"installed" : L"not installed") << L"\n";
  std::wcout << L"POV log:    " << (g_cameraLiveLogEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"POV lock:   " << (g_cameraLockEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Watchdog:   " << (g_loadingWatchEnabled ? L"on" : L"off") << L"\n";
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

std::string CommandTailAfterSubcommand(const std::string& command, const std::vector<std::string>& parts) {
  if (parts.size() < 2) return "";
  const size_t verbAt = command.find(parts[0]);
  const size_t subAt = command.find(parts[1], verbAt == std::string::npos ? 0 : verbAt + parts[0].size());
  if (subAt == std::string::npos) return "";
  return TrimAscii(command.substr(subAt + parts[1].size()));
}

std::string StripOuterQuotes(std::string value) {
  value = TrimAscii(value);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

fs::path ResolveConsolePath(const std::string& requestedPath) {
  fs::path path = Utf8ToWide(StripOuterQuotes(requestedPath));
  if (path.is_relative()) path = fs::current_path() / path;
  return path.lexically_normal();
}

uint64_t Fnv1aUpdate(uint64_t hash, const char* data, size_t size) {
  constexpr uint64_t prime = 1099511628211ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= prime;
  }
  return hash;
}

void HandleFileCommand(const std::vector<std::string>& parts, const std::string& command) {
  const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "help";
  if (sub == "help" || parts.size() < 3) {
    std::cout
        << "File commands:\n"
        << "  file info <path>  Show size and timestamp for any file\n"
        << "  file load <path>  Test-read/preload a .ttarch/.ttarch2 or any file\n"
        << "Note: file load does not mount archives into the Telltale engine yet.\n";
    return;
  }

  const fs::path path = ResolveConsolePath(CommandTailAfterSubcommand(command, parts));
  if (path.empty()) {
    std::cout << "Missing path.\n";
    return;
  }

  std::error_code error;
  if (!fs::exists(path, error)) {
    std::wcout << L"File not found: " << path << L"\n";
    LogLine(L"file", L"manual " + Utf8ToWide(sub) + L" missing " + path.wstring());
    return;
  }
  if (!fs::is_regular_file(path, error)) {
    std::wcout << L"Not a regular file: " << path << L"\n";
    return;
  }

  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes);
  const uintmax_t size = fs::file_size(path, error);

  if (sub == "info") {
    std::wcout << L"Path: " << path << L"\n";
    std::wcout << L"Size: " << size << L" bytes\n";
    std::wcout << L"Modified: " << FormatFileTimeText(attributes.ftLastWriteTime) << L"\n";
    return;
  }

  if (sub == "load" || sub == "read") {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      std::wcout << L"Could not open file: " << path << L"\n";
      LogLine(L"file", L"manual load failed " + path.wstring());
      return;
    }

    std::vector<char> buffer(1024 * 1024);
    uint64_t total = 0;
    uint64_t hash = 1469598103934665603ULL;
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize read = input.gcount();
      if (read <= 0) break;
      total += static_cast<uint64_t>(read);
      hash = Fnv1aUpdate(hash, buffer.data(), static_cast<size_t>(read));
    }

    std::wstringstream message;
    message << L"manual load OK " << path.wstring()
            << L" bytes=" << total
            << L" fnv1a=0x" << std::hex << hash;
    std::wcout << message.str() << L"\n";
    LogLine(L"file", message.str());
    return;
  }

  std::cout << "Unknown file command. Try: file info <path> or file load <path>\n";
}

void PrintLoadingWatchStatus() {
  const unsigned long long now = GetTickCount64();
  const unsigned long long lastActivity = g_lastFileActivityTick.load();
  const unsigned long long quietSeconds = lastActivity == 0 ? 0 : (now - lastActivity) / 1000;
  std::wcout << L"Loading watchdog: " << (g_loadingWatchEnabled ? L"on" : L"off") << L"\n";
  std::wcout << L"Watch thread:      " << (g_loadingWatchRunning ? L"running" : L"stopped") << L"\n";
  std::wcout << L"Quiet time:        " << quietSeconds << L" seconds\n";
}

void HandleLoadingCommand(const std::vector<std::string>& parts) {
  const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "status";
  if (sub == "help") {
    std::cout
        << "Loading watchdog commands:\n"
        << "  loading status       Show watchdog state\n"
        << "  loading on/off       Enable or disable the 60-second stall diagnostic\n"
        << "  loading diagnose     Log a diagnostic immediately\n"
        << "  watchdog ...         Alias for loading ...\n";
    return;
  }
  if (sub == "on") {
    g_loadingWatchEnabled = true;
    StartLoadingWatchThread();
    PrintLoadingWatchStatus();
    return;
  }
  if (sub == "off") {
    g_loadingWatchEnabled = false;
    PrintLoadingWatchStatus();
    return;
  }
  if (sub == "diagnose" || sub == "diag") {
    LogLoadingDiagnostic();
    std::cout << "Loading diagnostic logged.\n";
    return;
  }
  PrintLoadingWatchStatus();
}

void SetLogEnabled(bool enabled) {
  OpenLogFile();
  if (enabled) {
    g_logConsoleEnabled = true;
    g_logCompactEnabled = true;
    g_fileTraceEnabled = true;
    g_writeTraceEnabled = true;
    g_debugStringTraceEnabled = true;
    g_textureTraceEnabled = true;
    g_cameraTraceEnabled = false;
    g_logFailuresOnly = false;
    g_logFocusMode = 0;
    g_cameraLiveLogEnabled = false;
    g_cameraLockEnabled = false;
    g_lastFileActivityTick = GetTickCount64();
    g_lastLoadingDiagnosticTick = 0;
    StartLoadingWatchThread();
  } else {
    g_cameraLiveLogEnabled = false;
    g_cameraLockEnabled = false;
  }
  g_logEnabled = enabled;
  LogLine(L"console", enabled ? L"log on; verbose/all tracing enabled; POV logging manual" : L"log off");
  std::cout << (enabled ? "Log enabled. Default mode is now focus=all with file/write/debug/texture/watchdog tracing on. Camera tracing and live POV logging are off; use log cameras on or camera log on.\n" : "Log disabled.\n");
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
  std::cout << "This is v0.1.8: console, command loop, file loader, loading watchdog, manual camera tools.\n";
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
    } else if (verb == "colors" || verb == "colours") {
      PrintColorLegend();
    } else if (verb == "status") {
      PrintStatus();
    } else if (verb == "where") {
      std::wcout << fs::current_path() << L"\n";
    } else if (verb == "archives") {
      CountArchives();
    } else if (verb == "file") {
      HandleFileCommand(parts, command);
    } else if (verb == "loading" || verb == "watchdog") {
      HandleLoadingCommand(parts);
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
      } else if (sub == "writes") {
        if (parts.size() > 2) g_writeTraceEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else if (sub == "textures") {
        if (parts.size() > 2) g_textureTraceEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else if (sub == "cameras") {
        if (parts.size() > 2) g_cameraTraceEnabled = ToLowerAscii(parts[2]) != "off";
        PrintLogStatus();
      } else if (sub == "resources") {
        if (parts.size() > 2) {
          const bool enabled = ToLowerAscii(parts[2]) != "off";
          g_textureTraceEnabled = enabled;
          g_cameraTraceEnabled = enabled;
        }
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
    } else if (verb == "reload") {
      const std::string sub = parts.size() > 1 ? ToLowerAscii(parts[1]) : "";
      HandleReloadCommand(sub);
    } else if (verb == "camera" || verb == "pov") {
      HandleCameraCommand(parts);
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

  g_cameraLiveLogEnabled = false;
  g_cameraLockEnabled = false;
  StopCameraMonitorThread();
  StopLoadingWatchThread();
  UninstallCameraPointerHook();
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
