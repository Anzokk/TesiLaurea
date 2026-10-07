# Banco di prova IEC 61850 – BU9020

Strumenti per la verifica automatica di un IED IEC 61850 (BU9020):
lettura dei dati via **MMS** e analisi dei flussi **GOOSE** catturati in rete.

## Struttura del progetto

```
IED/
├── bin/                  eseguibili generati da `make` (non modificare a mano)
├── lib/
│   ├── libiec61850/      link a libiec61850 installata (dipendenza esterna)
│   └── python/           moduli Python condivisi (bench_io.py)
├── source/
│   ├── parser/           parser MMS + GOOSE in C (parser.c)
│   └── legacy/           versioni precedenti, conservate per riferimento
├── config/
│   ├── parser/           configurazioni del parser: IED, interfaccia, punti MMS, GoCB
│   ├── legacy/           configurazioni dei vecchi parser
│   └── scl/              file SCL dell'IED (.icd), da consultare a mano
├── data/
│   ├── captures/         catture di rete (.pcapng)
│   ├── processed/        dati estratti dalle catture (.csv)
│   └── results/          report prodotti dai test
├── docs/                 documentazione
└── Makefile
```

## Requisiti

| Componente | Requisiti |
|---|---|
| Parser MMS + GOOSE (C) | gcc, [libiec61850](https://github.com/mz-automation/libiec61850) compilata e installata, cJSON (`sudo apt install libcjson-dev`) |
| Cattura del traffico | `tshark` / `dumpcap` (Wireshark) |

Il link `lib/libiec61850` punta all'installazione locale della libreria
(`~/libiec61850/.install`). Su un'altra macchina va ricreato, oppure si indica
il percorso a `make`:

```sh
ln -s /percorso/libiec61850/install lib/libiec61850
# oppure
make IEC61850_DIR=/percorso/libiec61850/install
```

## Compilazione

```sh
make          # compila bin/parser
make clean    # rimuove i file generati
```

## Utilizzo rapido

```sh
# Letture MMS + GOOSE ricevuti nello stesso intervallo -> JSON
# (configurazione scritta a mano dai dati del file .icd)
sudo bin/parser config/parser/parser.json > data/results/run.json
```

I comandi vanno lanciati dalla cartella `IedAnalizer`. Il parser non esegue
verifiche: stampa i dati grezzi in JSON su stdout, che vengono poi controllati
dalla suite di test in Python. Exit code 0 = completato, 1 = errore (motivo su
stderr).

## Test

```sh
make test      # esegue gli scenari (tests/scenari/*.yaml)
```

La prima volta crea il venv `.venv` con robotframework, pyyaml e pyserial.
Report di Robot e dati di ogni scenario in `data/results/`.

## Documentazione

- [docs/libiec61850.md](docs/libiec61850.md) – la libreria libiec61850: architettura, API client, report, comandi, GOOSE
- [docs/parser.md](docs/parser.md) – parser MMS + GOOSE: configurazione e formato dell'output
- [docs/acquisizione.md](docs/acquisizione.md) – procedura di cattura del traffico
