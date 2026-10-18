# AIO Audio, a NickelHook mod. Build with NickelTC (ghcr.io/pgaskin/nickeltc:1.0):
#   git clone https://github.com/pgaskin/NickelHook && git -C NickelHook checkout bcd90ba94d90d22562fb64ef2920ed3323c1dae5
#   make all koboroot
include NickelHook/NickelHook.mk

override LIBRARY  := src/libaioaudio.so
override SOURCES  += src/aioaudio.cc
override MOCS     += src/aioaudio.h
override PKGCONF  += Qt5Widgets
override CFLAGS   += -Wall -Wextra
override CXXFLAGS += -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/LEEME.txt:/mnt/onboard/.adds/aio-audio/LEEME.txt

include NickelHook/NickelHook.mk
