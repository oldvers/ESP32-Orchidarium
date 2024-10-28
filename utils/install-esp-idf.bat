@echo off
if defined MSYSTEM (
    echo This .bat file is for Windows CMD.EXE shell only.
    goto :__end
)

set SCRIPT_EXIT_CODE=0

rem Check and create the file with paths to the main tools
set PATHS_WIN_TOOLS_FILE=paths-win-tools.bat
if exist %PATHS_WIN_TOOLS_FILE% (
    echo Start the installation...
) else (
    goto __error_missing_paths
)

rem Setup the environment variables
call %PATHS_WIN_TOOLS_FILE%
set IDF_PATH=%IDF_ROOT_PATH%\esp-idf
set IDF_TOOLS_PATH=%IDF_ROOT_PATH%
set PATH=%PYTHON_PATH%;%PYTHON_PATH%\Scripts;%GIT_PATH%;%VSCODE_PATH%;%IDF_PATH%

rem Missing requirements check
set MISSING_REQUIREMENTS=
python.exe --version >NUL 2>NUL
if %errorlevel% neq 0 (
    set SCRIPT_EXIT_CODE=%errorlevel%
    set "MISSING_REQUIREMENTS=  python &echo\"
)
git.exe --version >NUL 2>NUL
if %errorlevel% neq 0 (
    set SCRIPT_EXIT_CODE=%errorlevel%
    set "MISSING_REQUIREMENTS=%MISSING_REQUIREMENTS%  git &echo\"
)
call code --version >NUL 2>NUL
if %errorlevel% neq 0 (
    set SCRIPT_EXIT_CODE=%errorlevel%
    set "MISSING_REQUIREMENTS=%MISSING_REQUIREMENTS%  visual studio code"
)
if not "%MISSING_REQUIREMENTS%" == "" goto :__error_missing_requirements

echo Clone the ESP32 SDK to the specified folder
git.exe clone --progress -v "https://github.com/espressif/esp-idf.git" "%IDF_PATH%"
if %errorlevel% neq 0 (
    set SCRIPT_EXIT_CODE=0
    echo WARNING! It seems the SDK folder already exists. Trying to install the tools...
)

echo Switch to the ESP IDF folder
pushd .
cd "%IDF_PATH%"

echo Switch to the correct branch
git.exe checkout -b release/v5.4 remotes/origin/release/v5.4 --
if %errorlevel% neq 0 (
    set SCRIPT_EXIT_CODE=0
    echo WARNING! It seems the SDK branch already checked out. Trying to install the tools...
)

echo Restore the folder
popd

echo Installing ESP-IDF tools
call %IDF_PATH%\install.bat
if %errorlevel% neq 0 (
    echo ERROR! Something wrong with the tools installation...
	echo Delete all the folders and restart the installation.
	goto :__end
)

set PARAMETERS_IDF_TOOLS_FILE=parameters.bat
if exist %PARAMETERS_IDF_TOOLS_FILE% (
    goto :__done
)
echo Creating the ESP IDF parameters file
echo SET IDF_TARGET=esp32>> %PARAMETERS_IDF_TOOLS_FILE%
echo SET ESP_FLASH_PORT=COM1>> %PARAMETERS_IDF_TOOLS_FILE%
echo SET ESP_FLASH_BAUD=921600>> %PARAMETERS_IDF_TOOLS_FILE%
echo SET ESP_MONITOR_PORT=COM1>> %PARAMETERS_IDF_TOOLS_FILE%
echo SET ESP_MONITOR_BAUD=115200>> %PARAMETERS_IDF_TOOLS_FILE%

:__done
    echo All done!
    echo You can now run: "launch-vs-code.bat"
    goto :__end

:__error_missing_paths
    echo Creating the Windows tools' paths template file...
    echo SET GIT_PATH=C:\>> %PATHS_WIN_TOOLS_FILE%
    echo SET PYTHON_PATH=C:\>> %PATHS_WIN_TOOLS_FILE%
    echo SET VSCODE_PATH=C:\>> %PATHS_WIN_TOOLS_FILE%
    echo SET IDF_ROOT_PATH=C:\>> %PATHS_WIN_TOOLS_FILE%
	echo The file is created! You should modify it to set the absolute paths
    echo to the tools installed on your PC. Following examples: 
	echo  - GIT_PATH - absolute path to the Git (C:\Git\cmd)
	echo  - PYTHON_PATH - absolute path to the Python (C:\Python)
	echo  - VSCODE_PATH - absolute path to the VS Code (C:\VSCode\bin)
	echo  - IDF_ROOT_PATH - absolute path where ESP SDK and tools will be installed (C:\ESP-IDF)
	echo Modify the "%PATHS_WIN_TOOLS_FILE%" and run the installation again
	goto :__end

:__error_missing_requirements
    echo.
    echo ERROR! The following tools are not installed in your environment.
    echo %MISSING_REQUIREMENTS%
    echo.
    echo Please install the tools first for setting up your environment.
    goto :__end

:__end
exit /b %SCRIPT_EXIT_CODE%
