#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {
HMODULE g_module = nullptr;

std::wstring GetModulePath(HMODULE module) {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  return path;
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
      << "  clear     Clear this console\n"
      << "  detach    Unload the hook DLL and close this console\n";
}

void PrintStatus() {
  std::wcout << L"Process ID: " << GetCurrentProcessId() << L"\n";
  std::wcout << L"Hook DLL:   " << GetModulePath(g_module) << L"\n";
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

DWORD WINAPI ConsoleThread(void*) {
  AllocConsole();
  SetConsoleTitleW(L"TTDS Dev Console");
  AttachConsoleStreams();

  std::cout << "TTDS Dev Console injected.\n";
  std::cout << "This is v0: console + command loop only, no game internals yet.\n";
  PrintHelp();

  std::string command;
  while (true) {
    std::cout << "\nttds> ";
    if (!std::getline(std::cin, command)) break;

    if (command == "help") {
      PrintHelp();
    } else if (command == "status") {
      PrintStatus();
    } else if (command == "where") {
      std::wcout << fs::current_path() << L"\n";
    } else if (command == "archives") {
      CountArchives();
    } else if (command == "clear") {
      system("cls");
    } else if (command == "detach" || command == "quit") {
      break;
    } else if (command.empty()) {
      continue;
    } else {
      std::cout << "Unknown command. Type help.\n";
    }
  }

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

