#!/bin/sh

CPU=x86_64 CONFIG_WINDOWS=1 CONFIG_VARIABLE_SERVER_FPS=1 CONFIG_HTTP=1 CONFIG_CLIENT_GTV=1 CONFIG_OPENAL=1 CONFIG_PNG=1 CONFIG_JPEG=1 CONFIG_ANTICHEAT_SERVER=1 CONFIG_MVD_SERVER=1 CONFIG_MVD_CLIENT=1 make -j8
./install-dlls-msys2-mingw.sh
