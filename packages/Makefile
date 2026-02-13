# SPDX-License-Identifier: GPL-2.0
# SPIN Package Manager - Build System

CC = gcc
CFLAGS = -O2 -Wall -Wextra

TOOL = tools/spin-build
MANIFESTS = $(wildcard manifests/*.spinpkg)
PACKAGES = $(patsubst manifests/%.spinpkg,build/%.spk,$(MANIFESTS))

.PHONY: all clean index tool help

all: tool $(PACKAGES) index

tool: $(TOOL)

$(TOOL): tools/spin-build.c
	@echo "[CC] spin-build"
	@$(CC) $(CFLAGS) $< -o $@

build/%.spk: manifests/%.spinpkg $(TOOL)
	@echo "[BUILD] $*"
	@mkdir -p build
	@cd .. && ./packages/$(TOOL) packages/$< packages/$@

index: $(PACKAGES)
	@echo "[INDEX] repo.idx"
	@cd .. && bash packages/tools/update-index.sh

clean:
	@rm -f $(TOOL)
	@rm -f build/*.spk
	@rm -f repo.idx

help:
	@echo "SPIN Package Manager - Build Targets"
	@echo ""
	@echo "  all      - Build spin-build tool and all packages"
	@echo "  tool     - Build spin-build tool only"
	@echo "  index    - Generate repo.idx from built packages"
	@echo "  clean    - Remove built packages and tools"
	@echo "  help     - Show this help"
