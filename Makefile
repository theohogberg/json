CC       ?= cc
CFLAGS   ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX   ?= /usr/local

# Recursion cap. Costs about 64 bytes of stack per level, so this is
# negligible against a default stack. Anything nested deeper than this is
# a generated structure rather than data anyone wrote or needs to read.
DEPTH    ?= 1024

INCS      = -Iinclude
LDFLAGS  += -Llib

# every src/<name>.c is built as its own variant
VARIANTS  = $(patsubst src/%.c,%,$(wildcard src/*.c))
OUT       = out
BINS      = $(addprefix $(OUT)/,$(VARIANTS))
ASANS     = $(addsuffix .asan,$(BINS))

# which variant `make install` ships
MAIN     ?= json

SAN       = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
# the tool deliberately never frees; exit reclaims. turn off the leak
# checker so it does not mask real findings.
SANENV    = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

all: $(BINS)

# order-only (|) so writing a binary into out/ does not stamp the directory
# newer than the binary and force a rebuild every time.
$(OUT):
	mkdir -p $(OUT)

# Make compares timestamps, not flags, so changing DEPTH or CFLAGS on the
# command line would otherwise reuse stale binaries and silently test the
# wrong thing. Record the flags in a stamp file and rebuild when it moves.
FLAGSIG = $(CC) $(CFLAGS) -DMAXDEPTH=$(DEPTH)
$(OUT)/.flags: FORCE | $(OUT)
	@echo '$(FLAGSIG)' | cmp -s - $@ 2>/dev/null || echo '$(FLAGSIG)' > $@
FORCE:

# %.asan is the more specific pattern, so make prefers it for out/x.asan
$(OUT)/%.asan: src/%.c $(OUT)/.flags | $(OUT)
	$(CC) $(SAN) -DMAXDEPTH=$(DEPTH) $(INCS) $(LDFLAGS) -o $@ $<

$(OUT)/%: src/%.c $(OUT)/.flags | $(OUT)
	$(CC) $(CFLAGS) -DMAXDEPTH=$(DEPTH) $(INCS) $(LDFLAGS) -o $@ $<

# ---- per-variant targets: test-<variant>, check-<variant>, ... ----

test-%: $(OUT)/%
	@echo "== test: $* =="
	@DEPTH=$(DEPTH) JSON=$(OUT)/$* ./test/test.sh

conformance-%: $(OUT)/% JSONTestSuite
	@echo "== conformance: $* =="
	@JSON=$(OUT)/$* ./test/conformance.sh

check-%: $(OUT)/%.asan JSONTestSuite
	@echo "== check (asan+ubsan): $* =="
	@$(SANENV) DEPTH=$(DEPTH) JSON=$(OUT)/$*.asan ./test/test.sh
	@$(SANENV) JSON=$(OUT)/$*.asan ./test/conformance.sh

fuzz-%: $(OUT)/%.asan
	@echo "== fuzz: $* =="
	@$(SANENV) python3 test/fuzz.py $(RUNS) $(OUT)/$*.asan

RUNS ?= 5000

# ---- aggregate targets: same thing across every variant ----

test:        $(addprefix test-,$(VARIANTS))
conformance: $(addprefix conformance-,$(VARIANTS))
check:       $(addprefix check-,$(VARIANTS))
fuzz:        $(addprefix fuzz-,$(VARIANTS))

# all variants must agree byte for byte on the same input
diff: $(BINS)
	@f=test/test.json; ref=$(firstword $(BINS)); \
	$$ref < $$f > $(OUT)/.ref; \
	for b in $(BINS); do \
		$$b < $$f > $(OUT)/.cmp; \
		if cmp -s $(OUT)/.ref $(OUT)/.cmp; then echo "  ok   $$b"; \
		else echo "  DIFF $$b"; rm -f $(OUT)/.ref $(OUT)/.cmp; exit 1; fi; \
	done; rm -f $(OUT)/.ref $(OUT)/.cmp

ci: test check fuzz conformance diff

JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git

tags: $(wildcard src/*.c)
	ctags -R src include

install: $(OUT)/$(MAIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(OUT)/$(MAIN) $(DESTDIR)$(PREFIX)/bin/json

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json

clean:
	rm -rf $(OUT)

# NB: the per-variant test-%/check-%/fuzz-% targets are deliberately NOT
# listed here. Declaring a target phony stops make from searching for a
# pattern rule to build it, which would break them. They never exist as
# files, so they always run anyway.
.PHONY: all test conformance check fuzz diff ci tags install uninstall clean FORCE
