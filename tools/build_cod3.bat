@echo off
REM Configure + build the COD3 port, capturing all output to build\cod3_build.log
setlocal
set "REPO=%~dp0.."
call "%REPO%\tools\env.bat"
cd /d "%REPO%"
if not exist "%REPO%\build" mkdir "%REPO%\build"
cmake -S "%REPO%\src" -B "%REPO%\build\cod3" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  > "%REPO%\build\cod3_build.log" 2>&1
if errorlevel 1 (
  echo CONFIG FAILED
  exit /b 1
)
REM XenonRecomp hardcodes the indirect-call table address in ppc_context.h.
REM Redirect it to the runtime table before compiling. Idempotent.
python "%REPO%\tools\patch_ppc_context.py" "%REPO%\build\ppc\ppc_context.h"
if errorlevel 1 (
  echo PATCH FAILED
  exit /b 1
)
REM Record which guest functions the recompiler left untranslated, so the crash
REM handler does not report an offset measured from a 4-byte stub. Generated at
REM build time rather than scanned at runtime: the runtime version read the same
REM files and found none of them.
python "%REPO%\tools\gen_stub_symbols.py" "%REPO%\build\ppc" "%REPO%\build\runtime\cod3_stub_symbols.h"
if errorlevel 1 (
  echo STUB SYMBOLS FAILED
  exit /b 1
)
cmake --build "%REPO%\build\cod3" --config Release -j %COD3_JOBS%
exit /b %ERRORLEVEL%