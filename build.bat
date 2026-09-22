@echo off
cd /d D:\ESP\监控\cc_mate

set PATH=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;%PATH%
set IDF_PATH=C:\esp\v5.5.4\esp-idf
set MSYSTEM=

if "%1"=="clean" goto :clean
if "%1"=="flash" goto :flash
if "%1"=="monitor" goto :monitor
goto :build

:build
if not exist build mkdir build
echo.
echo ===== Step 1: CMake =====
C:\Espressif\tools\cmake\3.30.2\bin\cmake.exe -G Ninja ^
  -DIDF_PATH=C:\esp\v5.5.4\esp-idf ^
  -DIDF_TARGET=esp32s3 ^
  -DPYTHON=C:\Users\xupeng\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe ^
  -DCMAKE_TOOLCHAIN_FILE=C:\esp\v5.5.4\esp-idf\tools\cmake\toolchain-esp32s3.cmake ^
  -B build ^
  -S .
if %errorlevel% neq 0 (
  echo CMake failed! error=%errorlevel%
  pause
  exit /b %errorlevel%
)

echo.
echo ===== Step 2: Ninja Build =====
C:\Espressif\tools\ninja\1.12.1\ninja.exe -C build -j8
if %errorlevel% neq 0 (
  echo Ninja failed! error=%errorlevel%
  pause
  exit /b %errorlevel%
)

echo.
echo ===== BUILD SUCCESS =====
dir build\cc_mate.bin
goto :eof

:clean
echo Cleaning...
if exist build (
  rmdir /s/q build\bootloader 2>nul
  del /f build\CMakeCache.txt 2>nul
  rmdir /s/q build\CMakeFiles 2>nul
  rmdir /s/q build\esp-idf 2>nul
  del /f build\build.ninja 2>nul
  del /f build\.ninja_log build\.ninja_deps build\.ninja_scan 2>nul
  del /f build\cc_mate.bin build\cc_mate.elf build\cc_mate.map 2>nul
)
echo Clean done.
goto :eof

:flash
C:\Users\xupeng\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe C:\esp\v5.5.4\esp-idf\components\esptool_py\esptool\esptool.py --chip esp32s3 -p %2 write_flash @build\flash_args
goto :eof

:monitor
C:\Users\xupeng\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe C:\esp\v5.5.4\esp-idf\tools\idf_monitor.py -p %2 build\cc_mate.elf
goto :eof