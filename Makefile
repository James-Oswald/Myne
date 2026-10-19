ifeq ($(origin CC),default)
CC := $(shell command -v tcc >/dev/null 2>&1 && echo tcc || echo cc)
endif
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
ifeq ($(notdir $(CC)),tcc)
CFLAGS += -w
endif
CPPFLAGS += -Ivendor/stringzilla/include -DSZ_USE_NEON=0 -DSZ_USE_NEONAES=0 -DSZ_USE_NEONSHA=0
LDLIBS += -lpthread

all: myne

src/app_assets.inc: assets/style.css assets/runtime.js tools/embed_app.py
	python3 tools/embed_app.py

src/katex_assets.inc: tools/embed_katex.py
	python3 tools/embed_katex.py

myne: src/myne.c src/app_assets.inc src/katex_assets.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f myne src/app_assets.inc src/katex_assets.inc

.PHONY: all clean
