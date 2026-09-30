# d99 build configuration

CC      ?= cc
AR      ?= ar
INSTALL ?= install
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

HAVE_ZLIB    ?= $(shell test -f /usr/include/zlib.h    && echo 1 || echo 0)
HAVE_LIBLZMA ?= $(shell test -f /usr/include/lzma.h    && echo 1 || echo 0)
HAVE_ZSTD    ?= $(shell test -f /usr/include/zstd.h    && echo 1 || echo 0)

D99_BACKEND_CFLAGS := -DHAVE_ZLIB=$(HAVE_ZLIB) -DHAVE_LIBLZMA=$(HAVE_LIBLZMA) -DHAVE_ZSTD=$(HAVE_ZSTD)
D99_BACKEND_LIBS   := $(if $(filter 1,$(HAVE_ZLIB)),-lz ) $(if $(filter 1,$(HAVE_LIBLZMA)),-llzma ) $(if $(filter 1,$(HAVE_ZSTD)),-lzstd ) -ldl
