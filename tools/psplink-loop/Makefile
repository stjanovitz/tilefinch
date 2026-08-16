# tfexec: the PSPLink-side half of the remote device loop.
#
#   PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH make -C tools/psplink-loop
#
# Produces tfexec.prx beside this file. Copy it (or serve it over host0) and
# run `ld host0:/tfexec.prx` from pspsh to start the installed browser; see
# docs/engineering/INPUT_SCRIPT_HARNESS.md, "PSPLink live loop".
#
# Deliberately the SDK's own build.mak rather than a CMake target: this links
# no engine code, and keeping it on the stock BUILD_PRX=1 recipe is what makes
# it a check on the recipe cmake/TilefinchTargets.cmake reproduces for
# psp-browser-script-dev-prx.
TARGET = tfexec
OBJS = main.o systemctrl_user_imports.o
BUILD_PRX = 1
PSP_FW_VERSION = 660
CFLAGS = -O2 -Wall
PSPSDK = $(shell psp-config --pspsdk-path)

# The CFW import stubs are the ones the browser and the launcher already use.
# Compiled from the shared source rather than copied, so a NID correction can
# never leave this tool calling a stale entry point.
vpath %.S ../../src

include $(PSPSDK)/lib/build.mak
