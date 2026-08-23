# Development-only buffered EBOOT deployment helper.
#
#   PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH make -C tools/psplink-deploy
#
# The resulting tfdeploy.prx is loaded from host0 by scripts/psplink-device.sh.
# It is not linked into, copied beside, or shipped with Tilefinch releases.
TARGET = tfdeploy
OBJS = main.o
BUILD_PRX = 1
PSP_FW_VERSION = 660
CFLAGS = -Os -Wall -Wextra -Werror
PSPSDK = $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
