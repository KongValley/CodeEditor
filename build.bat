@echo off
rem ==========================================================================
rem  Intranet Safe Code Editor build script (idempotent)
rem  Builds a single static x86 exe with zero runtime dependencies.
rem  NOTE: GNU ld from MinGW mis-resolves paths containing non-ASCII chars
rem  (e.g. Chinese dir names). The build therefore happens in an ASCII path
rem  (C:\cedb) and the final exe is copied back.
rem ==========================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set FARM=C:\cedb
set BIN=%FARM%\third_party\mingw32\bin
set SC=%FARM%\third_party\scintilla
set LEX=%FARM%\third_party\lexilla

rem --- 1. toolchain & third-party sources (fetch into ASCII farm) ----------
if not exist "%FARM%\third_party\.complete" goto fetch_deps
echo [ok] dependencies already fetched
goto have_deps

:fetch_deps
echo [1/4] fetching dependencies...
if not exist "%FARM%\third_party" mkdir "%FARM%\third_party"

curl -fsSL -o "%FARM%\third_party\7zr.exe" https://www.7-zip.org/a/7zr.exe
if errorlevel 1 goto fail_dl7zr

if not exist "%FARM%\third_party\mingw32\bin\gcc.exe" (
  echo       downloading MinGW-w64 toolchain ^(~86MB^)...
  curl -fL -o "%FARM%\third_party\mingw.7z" "https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev1/i686-13.2.0-release-win32-dwarf-msvcrt-rt_v11-rev1.7z"
  if errorlevel 1 goto fail_dlmingw
  "%FARM%\third_party\7zr.exe" x -y -o"%FARM%\third_party" "%FARM%\third_party\mingw.7z" >nul
  del "%FARM%\third_party\mingw.7z"
)

if not exist "%FARM%\third_party\scintilla\win32\makefile" (
  echo       downloading Scintilla 5.6.7...
  curl -fsSL -o "%FARM%\third_party\sc.zip" https://www.scintilla.org/scintilla567.zip
  if errorlevel 1 goto fail_dlsci
  tar -xf "%FARM%\third_party\sc.zip" -C "%FARM%\third_party"
  del "%FARM%\third_party\sc.zip"
)

if not exist "%FARM%\third_party\lexilla\src\makefile" (
  echo       downloading Lexilla 5.5.4...
  curl -fsSL -o "%FARM%\third_party\lex.zip" https://www.scintilla.org/lexilla554.zip
  if errorlevel 1 goto fail_dllex
  tar -xf "%FARM%\third_party\lex.zip" -C "%FARM%\third_party"
  del "%FARM%\third_party\lex.zip"
)
echo done> "%FARM%\third_party\.complete"
goto have_deps

:have_deps

rem --- 2. sync our sources into the ASCII farm ------------------------------
echo [2/4] syncing sources...
if not exist "%FARM%\src" mkdir "%FARM%\src"
if not exist "%FARM%\res" mkdir "%FARM%\res"
if not exist "%FARM%\build" mkdir "%FARM%\build"
if not exist "%FARM%\dist" mkdir "%FARM%\dist"
copy /y src\*.cxx "%FARM%\src\" >nul
copy /y res\*.rc res\*.manifest "%FARM%\res\" >nul
if errorlevel 1 goto fail_copy

rem --- 3. static libraries (build once, keep) -------------------------------
if not exist "%SC%\bin\libscintilla.a" (
  echo [3/4] building Scintilla static lib...
  "%BIN%\mingw32-make.exe" -C "%SC%\win32" > "%FARM%\build\scintilla.log" 2>&1
  if errorlevel 1 goto fail_scibuild
) else (
  echo [3/4] Scintilla static lib already built
)
if not exist "%LEX%\bin\liblexilla.a" (
  echo       building Lexilla static lib...
  "%BIN%\mingw32-make.exe" -C "%LEX%\src" > "%FARM%\build\lexilla.log" 2>&1
  if errorlevel 1 goto fail_lexbuild
) else (
  echo       Lexilla static lib already built
)

rem --- 4. application link ---------------------------------------------------
echo [4/4] building application...
"%BIN%\windres.exe" -O coff "%FARM%\res\app.rc" -o "%FARM%\build\app_res.o"
if errorlevel 1 goto fail_rc

"%BIN%\g++" -std=c++17 -O2 -municode -mwindows ^
  -I"%SC%\include" -I"%LEX%\include" ^
  "%FARM%\src\main.cxx" "%FARM%\build\app_res.o" ^
  "%SC%\bin\libscintilla.a" "%LEX%\bin\liblexilla.a" ^
  -lgdi32 -luser32 -limm32 -lole32 -loleaut32 -luuid -ladvapi32 ^
  -lcomdlg32 -lcomctl32 -lshell32 -lshlwapi ^
  -static -static-libgcc -static-libstdc++ ^
  -o "%FARM%\dist\CodeEditor.exe"
if errorlevel 1 goto fail_link

copy /y "%FARM%\dist\CodeEditor.exe" "dist\CodeEditor.exe" >nul
copy /y "%FARM%\dist\CodeEditor.exe" "dist\editor-rename.tmp" >nul
if errorlevel 1 (
    echo ERROR: copying final exe failed
    exit /b 1
)
rem rename via ASCII temp name keeps the codepage path out of copy's argument
if exist "dist\代码编辑器.exe" del /f "dist\代码编辑器.exe"
ren "dist\editor-rename.tmp" "代码编辑器.exe"
if errorlevel 1 (
    echo ERROR: renaming final exe failed
    exit /b 1
)
echo.
echo ============================================
echo  BUILD OK: dist\代码编辑器.exe
echo ============================================
exit /b 0

rem --- error handlers --------------------------------------------------------
:fail_dl7zr
echo ERROR: cannot download 7zr.exe
exit /b 1
:fail_dlmingw
echo ERROR: cannot download MinGW toolchain
exit /b 1
:fail_dlsci
echo ERROR: cannot download Scintilla ^(check scintilla.org fallback: GitHub mirror rel-5-6-7^)
exit /b 1
:fail_dllex
echo ERROR: cannot download Lexilla ^(fallback: GitHub mirror rel-5-5-4^)
exit /b 1
:fail_copy
echo ERROR: copying sources into ASCII farm failed
exit /b 1
:fail_scibuild
echo ERROR: Scintilla build failed, see C:\cedb\build\scintilla.log
exit /b 1
:fail_lexbuild
echo ERROR: Lexilla build failed, see C:\cedb\build\lexilla.log
exit /b 1
:fail_rc
echo ERROR: windres failed
exit /b 1
:fail_link
echo ERROR: link failed ^(see output above^)
exit /b 1
