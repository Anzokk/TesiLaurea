# Makefile - banco di prova IEC 61850 BU9020
#
#   make            compila i tool C in bin/
#   make check      verifica la sintassi degli script Python
#   make clean      rimuove i file generati
#
# Percorso di libiec61850 (include/ e lib/): per default il link simbolico
# lib/libiec61850; si puo' cambiare con  make IEC61850_DIR=/altro/percorso

IEC61850_DIR ?= lib/libiec61850

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CPPFLAGS += -I$(IEC61850_DIR)/include
LDFLAGS  += -L$(IEC61850_DIR)/lib
LDLIBS   += -liec61850 -lpthread

BIN := bin/ParserMMSV2
PY  := source/goose/ParserGooseV2.py source/scl/scl_extract.py lib/python/bench_io.py

.PHONY: all check clean

all: $(BIN)

bin/ParserMMSV2: source/mms/ParserMMSV2.c | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

bin:
	mkdir -p $@

check:
	python3 -I -m py_compile $(PY) && echo "sintassi Python OK"

clean:
	rm -f $(BIN)
	find . -name __pycache__ -type d -prune -exec rm -rf {} +
