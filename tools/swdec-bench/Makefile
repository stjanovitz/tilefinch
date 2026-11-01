# PPSSPP bench for the optional swdec decoder (not a device or shipping
# target): CPU-only decode of length-prefixed access units, per-frame adler32
# for comparison with host ffmpeg framecrc, and portable-vs-VFPU colour
# conversion. See run-bench.sh for the whole loop.
#
#   make -f tools/swdec-bench/Makefile \
#       SWDEC_SOURCE_DIR=<repo>/src/swdec TILEFINCH_SOURCE_DIR=<repo> \
#       FFMPEG_SOURCE_DIR=<prepared>/ffmpeg FFMPEG_LIBRARY_DIR=<prepared>/libs/asm22-diag
#
# The bench reads the stage clock, so it links the diagnostic FFmpeg build.
PSPSDK := $(shell psp-config --pspsdk-path)
TARGET := swdecbench
OBJS := main.o swdec.o swdec_arena.o swdec_csc.o swdec_recon_diag.o swdec_chroma_replay.o
VPATH := $(SWDEC_SOURCE_DIR) $(TILEFINCH_SOURCE_DIR)/tools/swdec-bench
INCDIR := $(SWDEC_SOURCE_DIR) $(TILEFINCH_SOURCE_DIR)/include \
          $(TILEFINCH_SOURCE_DIR)/src $(FFMPEG_SOURCE_DIR)
LIBDIR := $(FFMPEG_LIBRARY_DIR)
CFLAGS := -O2 -G0 -Wall -std=gnu11 -DPSP -DHAVE_AV_CONFIG_H \
          -DSWDEC_FRAME_ONLY=1 -DSWDEC_PSP_ASM=1 -DSWDEC_LF_STRENGTH_C=1 \
          -DSWDEC_PSP_LF_CHROMA=1 -DSWDEC_DIAGNOSTICS=1 $(BENCH_CFLAGS)
LIBS := -lavcodec -lavutil -lm
EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := swdec bench
include $(PSPSDK)/lib/build.mak
