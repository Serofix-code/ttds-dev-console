@echo off
setlocal

set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "MSVC=%VSROOT%\VC\Tools\MSVC\14.44.35207"
set "MSBUILD=%VSROOT%\MSBuild\Current\Bin\MSBuild.exe"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "WINSDKVER=10.0.22621.0"

if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" (
  set "MSVC=%VSROOT%\VC\Tools\MSVC\14.34.31933"
)

if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" (
  echo Could not find x64 cl.exe under:
  echo %VSROOT%\VC\Tools\MSVC
  exit /b 1
)

if not exist "%MSBUILD%" (
  echo Could not find MSBuild at:
  echo %MSBUILD%
  exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%VSROOT%\MSBuild\Current\Bin;%PATH%"
set "VCToolsInstallDir=%MSVC%"
set "INCLUDE=%MSVC%\include;%WINSDK%\Include\%WINSDKVER%\ucrt;%WINSDK%\Include\%WINSDKVER%\shared;%WINSDK%\Include\%WINSDKVER%\um;%WINSDK%\Include\%WINSDKVER%\winrt;%WINSDK%\Include\%WINSDKVER%\cppwinrt;%INCLUDE%"
set "LIB=%MSVC%\lib\x64;%WINSDK%\Lib\%WINSDKVER%\ucrt\x64;%WINSDK%\Lib\%WINSDKVER%\um\x64;%LIB%"
set "LIBPATH=%MSVC%\lib\x64;%WINSDK%\UnionMetadata\%WINSDKVER%;%WINSDK%\References\%WINSDKVER%;%LIBPATH%"

if not exist "bin\x64\Release" mkdir "bin\x64\Release"
if not exist "obj\TTDSConsoleLauncher\x64\Release" mkdir "obj\TTDSConsoleLauncher\x64\Release"
if not exist "obj\TTDSConsoleHook\x64\Release" mkdir "obj\TTDSConsoleHook\x64\Release"

cl /nologo /EHsc /std:c++17 /O2 /MD /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_UNICODE /DUNICODE ^
  /Fo"obj\TTDSConsoleLauncher\x64\Release\\" ^
  /Fe"bin\x64\Release\TTDSConsoleLauncher.exe" ^
  "src\TTDSConsoleLauncher\main.cpp" ^
  /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

cl /nologo /EHsc /std:c++17 /O2 /MD /W4 /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_UNICODE /DUNICODE ^
  /Fo"obj\TTDSConsoleHook\x64\Release\\" ^
  /Fe"bin\x64\Release\TTDSConsoleHook.dll" ^
  "src\TTDSConsoleHook\dllmain.cpp" ^
  /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1

echo.
echo Built:
echo   bin\x64\Release\TTDSConsoleLauncher.exe
echo   bin\x64\Release\TTDSConsoleHook.dll
exit /b 0
