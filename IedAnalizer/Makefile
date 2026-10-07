# Makefile - banco di prova IEC 61850 BU9020
#
#   make            compila il parser in bin/parser
#   make clean      rimuove i file generati
#   make test       esegue gli scenari Robot (tests/banco.robot); la prima
#                   volta crea il venv .venv con robotframework, pyyaml, pyserial
#
# Percorso di libiec61850 (include/ e lib/): per default il link simbolico
# lib/libiec61850; si puo' cambiare con  make IEC61850_DIR=/altro/percorso

IEC61850_DIR ?= lib/libiec61850

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CPPFLAGS += -I$(IEC61850_DIR)/include
LDFLAGS  += -L$(IEC61850_DIR)/lib
LDLIBS   += -liec61850 -lcjson -lpthread

BIN := bin/parser

VENV := .venv

.PHONY: all clean test

all: $(BIN)

# parser e tshark usano un socket raw: si eseguono come root
test: $(BIN) $(VENV)/bin/robot
	sudo $(VENV)/bin/robot -d data/results tests/banco.robot

$(VENV)/bin/robot:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install robotframework pyyaml pyserial

bin/parser: source/parser/parser.c | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

bin:
	mkdir -p $@

clean:
	rm -f $(BIN)
	find . -name __pycache__ -type d -prune -exec rm -rf {} +
