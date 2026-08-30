
CASES = clapping claves glockenspiel organlike speech synth

CHECKS = $(addsuffix -check,$(CASES))
SOURCES = $(addsuffix .acm,$(CASES))
TEXTS = $(addsuffix .txt,$(CASES))

DECODES = $(addsuffix .dec.wav,$(CASES))
ENCODES = $(addsuffix .enc.acm,$(CASES))
RECODES = $(addsuffix .enc.wav,$(CASES))
SUMS = $(addsuffix .sha256,$(CASES))

ACMTOOL = ../src/acmtool

.PHONY: gen all dbg
.SECONDARY:

all: check

clean:
	rm -f $(DECODES) $(ENCODES) $(RECODES)

dbg:
	@echo "CHECKS=$(CHECKS)"
	@echo "DECODES=$(DECODES)"
	@echo "ENCODES=$(ENCODES)"
	@echo "SUMS=$(SUMS)"
	@echo "RECODES=$(RECODES)"
	@echo "SOURCES=$(SOURCES)"

%.dec.wav: %.acm $(ACMTOOL)
	$(ACMTOOL) -q -d -o $@ $<

%.enc.acm: %.dec.wav $(ACMTOOL)
	$(ACMTOOL) -q -e -o $@ $<

%.enc.wav: %.enc.acm $(ACMTOOL)
	$(ACMTOOL) -q -d -o $@ $<

%.sha256: %.acm %.dec.wav %.enc.acm %.enc.wav
	sha256sum -b $(foreach ext,.acm .dec.wav .enc.acm .enc.wav, $(subst .sha256,,$@)$(ext)) > $@

check: $(DECODES) $(ENCODES) $(RECODES)
	sha256sum -c $(SUMS)

gen: $(SUMS)
