# TTDS Dev Console

Experimental injected dev console for **The Walking Dead: The Telltale Definitive Series**.

This is a first proof-of-concept. It launches `WDC.exe`, injects `TTDSConsoleHook.dll`, and opens a visible console window inside the game process.

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

## Run

```bat
bin\x64\Release\TTDSConsoleLauncher.exe --game "C:\Program Files (x86)\Steam\steamapps\common\The Walking Dead The Telltale Definitive Series"
```

The launcher starts `WDC.exe`, injects the hook DLL, then waits for the game to close.

## Console Commands

- `help`: show commands
- `status`: show process/module info
- `where`: show current directory
- `archives`: count files in the `Archives` folder
- `clear`: clear the console
- `detach`: unload the hook DLL and close the console

## Roadmap

1. Prove safe injection and console lifetime.
2. Add named-pipe communication between launcher and hook.
3. Add file-based commands such as backup, restore, enable/disable mod packs.
4. Investigate game scripting/scene functions only after the basic tool is stable.

