@echo off 
setlocal 
set PATH=%PATH%;%CD%\libs-windows-x86_64
set EXE=q2proded-x86_64.exe
start %EXE% %*
endlocal
