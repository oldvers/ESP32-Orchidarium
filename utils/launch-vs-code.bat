@echo off
if defined MSYSTEM (
    echo This .bat file is for Windows CMD.EXE shell only.
    goto :__end
)

rem Setup the current directory
set CD_PATH=%~dp0
set CD_PATH=%CD_PATH:~0,-1%

rem Setup the files used
set PATHS_WIN_TOOLS_FILE=paths-win-tools.bat
set PARAMETERS_IDF_TOOLS_FILE=parameters.bat

rem Setup the environment variables in the correct order
rem Setup the Windows tools paths
call %PATHS_WIN_TOOLS_FILE%

rem Setup the ESP IDF paths
set IDF_PATH=%IDF_ROOT_PATH%\esp-idf
set IDF_TOOLS_PATH=%IDF_ROOT_PATH%

rem Setup the PATH environment variable
set PATH=%GIT_PATH%;%VSCODE_PATH%;%PYTHON_PATH%

rem Setup the ESP IDF additional parameters
call %PARAMETERS_IDF_TOOLS_FILE%

call %IDF_PATH%\export.bat
if %errorlevel% neq 0 (
    echo ERROR! Something wrong with the tools installation...
	echo Delete all the folders and restart the installation.
	goto :__end
)

rem Switch to the project's folder
pushd .
cd ..

rem Call the VS Code
start /b code .

rem Restore the folder
popd

:__end
