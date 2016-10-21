#!/bin/sh

# set -ex

if [[ `file ./q2pro.exe` == *"x86-64"* ]]; then
    MINGWDIR=/mingw64
    SUFFIX="-x86_64"
else
    MINGWDIR=/mingw32
    SUFFIX="-x86"
fi

INSTALLDIR="`pwd`/build$SUFFIX"
INSTALLDIRLIBS="$INSTALLDIR/libs-windows$SUFFIX"

function dependencies_single_target_no_depth {
    local TARGET=$1

    local DEPENDENCIESFILTER="| grep 'DLL Name' | sed -r 's/\s+DLL\s+Name\:\s+//' | xargs -i{} which {} | grep $MINGWDIR/bin"
    local COMMAND="objdump -x $TARGET $DEPENDENCIESFILTER | xargs -i{} echo {}"

    local DEPENDENCIES=`eval "$COMMAND"`

    if [ "$DEPENDENCIES" != "" ]; then
        echo "$DEPENDENCIES"
    fi
}

function dependencies {
    local TARGETS=$@

    local TEMPORARYFILEA="install-dlls-msys2-mingw.alldependencies.tmp"
    local TEMPORARYFILEB="install-dlls-msys2-mingw.dependencies.tmp"

    local ALLDEPENDENCIES=""

    for TARGET in $TARGETS; do
        local ALLDEPENDENCIES=`dependencies_single_target_no_depth "$TARGET" && echo "$ALLDEPENDENCIES"`
    done

    local ALLDEPENDENCIES=`echo "$ALLDEPENDENCIES" | sort -u`

    local NEWDEPENDENCIES="$ALLDEPENDENCIES"

    while [ "$NEWDEPENDENCIES" != "" ]; do
        local DEPENDENCIES=""

        for DEPENDENCY in $NEWDEPENDENCIES; do
            DEPENDENCIES=`dependencies_single_target_no_depth "$DEPENDENCY" && echo "$DEPENDENCIES"`
        done

        echo "$ALLDEPENDENCIES" > "$TEMPORARYFILEA"
        echo "$DEPENDENCIES" | sort -u > "$TEMPORARYFILEB"

        local NEWDEPENDENCIES=`comm -13 "$TEMPORARYFILEA" "$TEMPORARYFILEB"`

        if [ "$NEWDEPENDENCIES" != "" ]; then
            local ALLDEPENDENCIES=`printf '%s\n' "$ALLDEPENDENCIES" "$NEWDEPENDENCIES" | sort`
        fi

        rm "$TEMPORARYFILEA" "$TEMPORARYFILEB"
    done

    if [ "$ALLDEPENDENCIES" != "" ]; then
        echo "$ALLDEPENDENCIES"
    fi
}

rm -Rf "$INSTALLDIR"
mkdir -p "$INSTALLDIR/baseq2"
mkdir -p "$INSTALLDIRLIBS"

for DLL in `ls ./*.dll`; do
    mv -v "$DLL" "$INSTALLDIR/baseq2"
done

for EXE in `ls ./*.exe`; do
    EXE_BASENAME="`basename $EXE .exe`"
    mv -v "$EXE" "$INSTALLDIR/$EXE_BASENAME$SUFFIX.exe"
    cp -v "./launchers/$EXE_BASENAME$SUFFIX.cmd" "$INSTALLDIR"
done

for DEPENDENCY in `dependencies $INSTALLDIR/*.exe`; do
    cp -v "$DEPENDENCY" "$INSTALLDIRLIBS"
done
