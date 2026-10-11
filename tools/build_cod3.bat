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
cmake --build "%REPO%\build\cod3" --config Release -j %COD3_JOBS%
exit /b %ERRORLEVEL%