@echo off 
setlocal 
set PATH=%PATH%;%CD%\libs-windows-x86
set EXE=q2proded-x86.exe
start %EXE% %*
endlocal
