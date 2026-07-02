# TTDS Dev Console

Experimental injected dev console for **The Walking Dead: The Telltale Definitive Series**.

This is an early proof-of-concept. It launches `WDC.exe`, watches the game's startup handoff, injects `TTDSConsoleHook.dll` into the active game process, and opens a visible console window inside the game.

## Features

- injects a visible console into the active `WDC.exe` game process
- can auto-watch for Steam-launched game sessions
- logs readable file activity for saves, archives, Relight, and mods
- helps modding by showing which `.ttarch2` archives, resource description scripts, save bundles, and mod files the game actually loads
- can check for installed mod archives and flag disabled/quarantine folders that the game may still scan
- can show failed file opens to help diagnose missing Relight files, mod conflicts, and other load issues
- can save the console/log session to a `.txt` file
- can find the newest quicksave/autosave/checkpoint/save candidate with `reload`
- can toggle TTDS Relighting's freecam configuration when Relight is installed

## Modding / Archive Debugging

- helps with modding/debugging by showing which `.ttarch2` archives the game actually reads during startup and gameplay, making it easier to see what loads, in what order, and whether a mod/archive is being detected

The console can help with TTDS modding because it shows readable file activity while the game is running. This includes `.ttarch2` archive reads, save writes, Relight files, and mod-related file access.

For example, archive log lines like these show which game archives are being loaded:

```txt
[file] OK   READ  archive  archives\WDC_pc_WalkingDead101_data.ttarch2
[file] OK   READ  archive  archives\WDC_pc_WalkingDead101_voice.ttarch2
[file] OK   READ  archive  archives\WDC_pc_WalkingDead101_txmesh.ttarch2
```

This can be useful for checking whether the game is reading the expected episode archives, seeing when archives are loaded, and diagnosing mod load order or missing-file issues.

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
- `log focus useful/all/saves/relight/mods/archives`: choose which file events are shown
- `log failures on/off`: show all interesting file opens or only failed file opens
- `log path`: print the log file path
- `log mark <text>`: add a marker while testing a scene/menu/action
- `log files on/off`: enable or disable file-open tracing
- `log debug on/off`: enable or disable `OutputDebugString` tracing
- `hooks refresh`: re-apply hooks after new game DLLs/modules load
- `mods check`: find mod archives inside disabled/quarantine-looking folders that may still be scanned
- `console save [path]`: save the current console/log session as a `.txt` file
- `reload`: find the newest quicksave/autosave/checkpoint/save candidate, prioritizing quick/autosaves first
- `reload list`: list reload candidates from the save folder
- `freecam`: toggle Relight's `FreeCameraOnlyMode` setting
- `freecam on/off/status/path`: set or inspect Relight's freecam setting
- `clear`: clear the console
- `detach`: unload the hook DLL and close the console

The current freecam command requires [TTDS Relighting](https://github.com/Telltale-Modding-Group/TTDS-Relighting) and edits `RelightMod\RelightConfiguration_Development.ini`. Relight reads that value when a scene initializes, so you still need to reload or load a scene before the camera changes apply. A true live toggle needs a runtime Lua bridge or direct camera backend hooks.

The current `reload` command finds the safest reload candidate from the save folder, prioritizing quicksave, autosave, and checkpoint bundles before normal save-slot bundles. Live in-engine save loading is not attached yet, so it does not force the Telltale engine to load the bundle directly.

## License

This project uses the **Serofix Non-Commercial Source License 1.0**. You can read, study, copy, modify, and publish meaningfully different modified versions, but you cannot sell this software or modified versions of it without prior written permission. See [LICENSE](LICENSE).

## Roadmap

1. Prove safe injection and console lifetime.
2. Add named-pipe communication between launcher and hook.
3. Add file-based commands such as backup, restore, enable/disable mod packs.
4. Investigate game scripting/scene functions only after the basic tool is stable.
