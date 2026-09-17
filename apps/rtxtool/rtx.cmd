@echo off
rem The Windows twin of `rtx`, so that one name starts the script from every shell: PowerShell
rem and cmd resolve `apps/rtxtool/rtx` to this file through PATHEXT, and a POSIX shell runs the
rem script itself. The bash that runs it is the one Git ships, found through git, since Git puts
rem itself on the PATH and not its bash.
for /f "delims=" %%p in ('git --exec-path') do set "gitcore=%%p"
"%gitcore%\..\..\..\bin\bash.exe" "%~dp0rtx" %*
exit /b %ERRORLEVEL%
