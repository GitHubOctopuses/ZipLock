@echo off
rem Build script - MinGW GCC (pure ASCII path required by ld)
rem The empty local default-manifest.o is needed because GCC's spec auto-links the CRT
rem default-manifest.o; the -B flag makes GCC pick ours first so only our manifest
rem (from app.rc) gets embedded. It is regenerated automatically if missing.
set MINGW=D:\AZML\mingw64\bin
set PATH=%MINGW%;%PATH%
cd /d %~dp0

if not exist default-manifest.o (
  echo. | "%MINGW%\gcc.exe" -x assembler -c - -o default-manifest.o || goto :err
)

windres -c 65001 app.rc -O coff -o app.res.o || goto :err
g++ main.cpp app.res.o -B%~dp0 -O2 -std=c++20 -municode -mwindows ^
    -static -static-libgcc -static-libstdc++ ^
    -o ZipLock.exe ^
    -lcomctl32 -lshell32 -lole32 -lshlwapi -luuid -luxtheme || goto :err

del app.res.o
echo BUILD OK: ZipLock.exe
exit /b 0

:err
echo BUILD FAILED
exit /b 1
