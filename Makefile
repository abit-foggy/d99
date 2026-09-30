include config.mk

VERSION ?= 0.2.0

CFLAGS := -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g \
          -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 \
          -DD99_VERSION='"$(VERSION)"' \
          -Ilib/include $(D99_BACKEND_CFLAGS) -fPIC
LDLIBS := $(D99_BACKEND_LIBS)

LIB      := lib/libd99.a
SO_LIB   := lib/libd99.so
LIB_HDRS := $(wildcard lib/include/*.h)
LIB_SRCS := $(sort $(wildcard lib/src/*.c))
LIB_OBJS := $(LIB_SRCS:.c=.o)

TOOLS := d99-deb d99-query d99-inst d99-solve d99-build
OUT   := output
BIN   := $(OUT)/bin
TBIN  := $(OUT)/tests

TOOL_SRCS_d99-deb   = src/deb/main.c
TOOL_SRCS_d99-query = src/query/main.c
TOOL_SRCS_d99-inst  = $(wildcard src/inst/*.c)
TOOL_SRCS_d99-solve = $(wildcard src/solve/*.c)
TOOL_SRCS_d99-build = $(wildcard src/build/*.c)

all: lib tools
lib: $(LIB) $(SO_LIB)
tools: $(foreach t,$(TOOLS),$(BIN)/$(t))

# Per-tool link rules (generated once per tool name).
define TOOL_RULES
TOOL_OBJS_$(1) := $$(subst .c,.o,$$(TOOL_SRCS_$(1)))
$$(BIN)/$(1): $$(TOOL_OBJS_$(1)) $$(LIB)
	@mkdir -p $(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$(TOOL_OBJS_$(1)) $$(LIB) $$(LDLIBS)
endef
$(foreach t,$(TOOLS),$(eval $(call TOOL_RULES,$(t))))

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

$(SO_LIB): $(LIB_OBJS)
	@mkdir -p lib $(OUT)/lib
	$(CC) $(CFLAGS) -shared -o $@ $(LIB_OBJS) $(LDLIBS)
	cp $@ $(OUT)/lib/libd99.so

lib/src/%.o: lib/src/%.c $(LIB_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c $(LIB_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# tests
$(TBIN)/sat_test: tests/sat_test.c $(LIB)
	@mkdir -p $(TBIN)
	$(CC) $(CFLAGS) -o $@ tests/sat_test.c $(LIB) $(LDLIBS)

$(TBIN)/lib_test: tests/lib_test.c $(LIB)
	@mkdir -p $(TBIN)
	$(CC) $(CFLAGS) -o $@ tests/lib_test.c $(LIB) $(LDLIBS)

check: all $(TBIN)/sat_test $(TBIN)/lib_test
	$(TBIN)/sat_test
	$(TBIN)/lib_test
	sh tests/run.sh

clean:
	find lib src -name '*.o' -delete 2>/dev/null || true
	rm -rf output bin lib/libd99.a lib/libd99.so

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

install: all
	install -d $(DESTDIR)$(BINDIR)
	$(foreach t,$(TOOLS),install -m 0755 $(BIN)/$(t) $(DESTDIR)$(BINDIR)/$(t) &&) true
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(SO_LIB) $(DESTDIR)$(LIBDIR)/libd99.so
	install -d $(DESTDIR)$(INCLUDEDIR)/d99
	install -m 0644 lib/include/*.h $(DESTDIR)$(INCLUDEDIR)/d99/

.PHONY: all lib tools check clean install

