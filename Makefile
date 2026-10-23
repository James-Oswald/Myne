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

ifeq ($(OS),Windows_NT)
MYNE := bin/myne.exe
LDFLAGS += -static
LDLIBS += -lws2_32
else
MYNE := bin/myne
endif
BIN_DIR := bin
SOURCES := src/myne.c src/state.c src/project.c src/source.c src/dependencies.c \
           src/html_content.c src/html_output.c src/tex.c src/assets.c src/server.c
OBJECTS := $(patsubst src/%.c,$(BIN_DIR)/%.o,$(SOURCES))

all: $(MYNE)

src/app_assets.inc: assets/style.css assets/runtime.js tools/embed_app.py
	python3 tools/embed_app.py

src/katex_assets.inc: tools/embed_katex.py
	python3 tools/embed_katex.py

$(BIN_DIR):
	mkdir -p $@

$(MYNE): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)


$(BIN_DIR)/%.o: src/%.c src/myne_internal.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN_DIR)/assets.o: src/app_assets.inc src/katex_assets.inc

pdfs: basic-pdf annotations-pdf thesis-pdf showcase-pdf

basic-pdf: $(MYNE)
	./$(MYNE) -i examples/basic -f both
	cd examples/basic/out/tex && latexmk -pdf -quiet -interaction=nonstopmode -halt-on-error sample.tex

annotations-pdf: $(MYNE)
	./$(MYNE) -i examples/annotations -f both
	cd examples/annotations/out/tex && latexmk -pdf -quiet -interaction=nonstopmode -halt-on-error main.tex

thesis-pdf: $(MYNE)
	./$(MYNE) -i examples/thesis -f both
	cd examples/thesis/out/tex && latexmk -pdf -quiet -interaction=nonstopmode -halt-on-error rpithes.tex

showcase-pdf: $(MYNE)
	./$(MYNE) -i examples/showcase -f both
	cd examples/showcase/out/tex && latexmk -pdf -quiet -interaction=nonstopmode -halt-on-error template.tex

clean:
	rm -rf $(BIN_DIR) src/app_assets.inc src/katex_assets.inc
	rm -f myne myne.exe src/*.o

.PHONY: all pdfs basic-pdf annotations-pdf thesis-pdf showcase-pdf clean
