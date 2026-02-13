# SPDX-License-Identifier: GPL-2.0
# SPIN Package Manager - Build System

CC = gcc
CFLAGS = -O2 -Wall -Wextra

TOOL = tools/spin-build
MANIFESTS = $(wildcard manifests/*.spinpkg)

.PHONY: all clean index tool help packages

all: tool packages index

tool: $(TOOL)

$(TOOL): tools/spin-build.c
	@echo "[CC] spin-build"
	@$(CC) $(CFLAGS) $< -o $@

packages: $(TOOL) $(MANIFESTS)
	@mkdir -p build
	@for manifest in $(MANIFESTS); do \
		name=$$(grep "^name " $$manifest | cut -d' ' -f2); \
		version=$$(grep "^version " $$manifest | cut -d' ' -f2); \
		output="build/$$name-$$version.spk"; \
		echo "[BUILD] $$name-$$version"; \
		cd .. && ./packages/$(TOOL) packages/$$manifest packages/$$output || exit 1; \
	done

index:
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
