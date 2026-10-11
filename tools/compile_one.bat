@echo off
REM Syntax-check a single translation unit of the COD3 recomp before running
REM the full 3.1M line build. Usage: compile_one.bat <source.cpp>
setlocal
set "REPO=%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%REPO%\build\objtest" mkdir "%REPO%\build\objtest"
"%CLANG%" /c /nologo /std:c++17 /O1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /arch:AVX2 /EHsc ^
  /I"%REPO%\build\ppc" ^
  /I"%REPO%\src\runtime" ^
  /I"%REPO%\.tools\XenonRecomp\XenonUtils" ^
  /I"%REPO%\.tools\XenonRecomp\thirdparty\simde" ^
  "%~1" /Fo"%REPO%\build\objtest\\"
exit /b %ERRORLEVEL%