@echo off 
setlocal 
set PATH=%PATH%;%CD%\libs-windows-x86_64
set EXE=q2pro-x86_64.exe
start %EXE% +set cl_async 1 +set cl_protocol 36 +set cl_maxpackets 0 +set cl_instantpacket 1 +set r_maxfps 120 +set cl_maxfps 120 +set cl_updaterate 0 +set sv_lan_force_rate 1 +set sv_fps 60 %*
endlocal
