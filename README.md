# TTDS Dev Console

Experimental injected dev console for **The Walking Dead: The Telltale Definitive Series**.

This is an early proof-of-concept. It launches `WDC.exe`, watches the game's startup handoff, injects `TTDSConsoleHook.dll` into the active game process, and opens a visible console window inside the game.

## Safety Notes

- This is for local single-player modding/testing only.
- It does not hide itself, bypass DRM, bypass anti-cheat, or target arbitrary software.
- It only launches and injects into `WDC.exe` from a selected The Walking Dead Definitive Series game folder.
- Injection tools can trigger antivirus warnings because DLL injection is also used by malware. Read the source before running builds from anyone else.

## Build

Run:

```bat
build.bat
```

Output:

```text
bin\x64\Release\TTDSConsoleLauncher.exe
bin\x64\Release\TTDSConsoleHook.dll
```

## Optional Dependencies

Freecam support requires **TTDS Relighting** by the Telltale Modding Group:

https://github.com/Telltale-Modding-Group/TTDS-Relighting

The console does not include Relight. It edits Relight's `RelightMod\RelightConfiguration_Development.ini`, so `freecam` commands will only work when Relight is installed in the game folder.

## Run

```bat
bin\x64\Release\TTDSConsoleLauncher.exe --game "C:\Program Files (x86)\Steam\steamapps\common\The Walking Dead The Telltale Definitive Series"
```

The launcher starts `WDC.exe`, injects the first process, and then watches for relaunches. Keep the launcher window open while the game starts; the first `WDC.exe` can hand off to another `WDC.exe`, and the watcher will inject again when that happens.

If the console appears and then disappears during the loading screen, that usually means the first bootstrap process exited. Leave the launcher running and wait for the final game process to be detected.

## Auto-Inject Watcher

To make the console appear when you start the game normally from Steam, install the background watcher:

```bat
bin\x64\Release\TTDSConsoleLauncher.exe --install-autostart --game "C:\Program Files (x86)\Steam\steamapps\common\The Walking Dead The Telltale Definitive Series"
```

This adds a current-user Windows startup entry that runs a hidden watcher after login. The watcher only targets `WDC.exe` from the configured game folder.

To remove it:

```bat
bin\x64\Release\TTDSConsoleLauncher.exe --uninstall-autostart
```

To run the watcher manually without installing it:

```bat
bin\x64\Release\TTDSConsoleLauncher.exe --watch-only --game "C:\Program Files (x86)\Steam\steamapps\common\The Walking Dead The Telltale Definitive Series"
```

## Console Commands

- `help`: show commands
- `status`: show process/module info
- `where`: show current directory
- `archives`: count files in the `Archives` folder
- `log`: show log status
- `log on`: start writing `ttds-dev-console.log` in the game folder and print live hook lines in the console
- `log off`: stop writing new log entries
- `log console on/off`: show or hide live log lines in the console
- `log format compact/full`: switch between readable short file logs and raw Windows file-open logs
- `log failures on/off`: show all interesting file opens or only failed file opens
- `log path`: print the log file path
- `log mark <text>`: add a marker while testing a scene/menu/action
- `log files on/off`: enable or disable file-open tracing
- `log debug on/off`: enable or disable `OutputDebugString` tracing
- `hooks refresh`: re-apply hooks after new game DLLs/modules load
- `freecam`: toggle Relight's `FreeCameraOnlyMode` setting
- `freecam on/off/status/path`: set or inspect Relight's freecam setting
- `clear`: clear the console
- `detach`: unload the hook DLL and close the console

The current freecam command requires [TTDS Relighting](https://github.com/Telltale-Modding-Group/TTDS-Relighting) and edits `RelightMod\RelightConfiguration_Development.ini`. Relight reads that value when a scene initializes, so you still need to reload or load a scene before the camera changes apply. A true live toggle needs a runtime Lua bridge or direct camera backend hooks.

## Roadmap

1. Prove safe injection and console lifetime.
2. Add named-pipe communication between launcher and hook.
3. Add file-based commands such as backup, restore, enable/disable mod packs.
4. Investigate game scripting/scene functions only after the basic tool is stable.
