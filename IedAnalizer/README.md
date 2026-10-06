# Banco di prova IEC 61850 – BU9020

Strumenti per la verifica automatica di un IED IEC 61850 (BU9020):
lettura dei dati via **MMS**, analisi dei flussi **GOOSE** catturati in rete ed
estrazione del modello dati dal file **SCL**.

## Struttura del progetto

```
IED/
├── bin/                  eseguibili generati da `make` (non modificare a mano)
├── lib/
│   ├── libiec61850/      link a libiec61850 installata (dipendenza esterna)
│   └── python/           moduli Python condivisi (bench_io.py)
├── source/
│   ├── parser/           parser MMS + GOOSE in C (parser.c)
│   ├── scl/              estrattore del modello SCL (scl_extract.py)
│   └── legacy/           versioni precedenti, conservate per riferimento
├── config/
│   ├── parser/           configurazioni del parser: IED, interfaccia, punti MMS, GoCB
│   ├── legacy/           configurazioni dei vecchi parser
│   └── scl/              file SCL dell'IED (.icd)
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
| Estrattore SCL | Python ≥ 3.8 (solo libreria standard) |

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
make check    # controlla la sintassi degli script Python
make clean    # rimuove i file generati
```

## Utilizzo rapido

```sh
# 1. Dal file SCL: modello dell'IED e configurazione del parser
python3 source/scl/scl_extract.py config/scl/<ied>.icd -o data/results/scl \
        --genera-config config
#    (poi compilare "interface" in config/parser/<ied>.json)

# 2. Letture MMS + GOOSE ricevuti nello stesso intervallo -> JSON
sudo bin/parser                              # usa config/parser/parser.json
sudo bin/parser config/parser/<ied>.json
```

I comandi vanno lanciati dalla cartella `IedAnalizer`. Il parser non esegue
verifiche: produce i dati grezzi in JSON, che vengono poi controllati dai test
Robot Framework. Gli exit code (vedi [docs/parser.md](docs/parser.md))
permettono di usarlo in script e pipeline.

## Documentazione

- [docs/libiec61850.md](docs/libiec61850.md) – la libreria libiec61850: architettura, API client, report, comandi, GOOSE
- [docs/parser.md](docs/parser.md) – parser MMS + GOOSE: configurazione e formato dell'output
- [docs/scl_extract.md](docs/scl_extract.md) – estrattore SCL e generazione delle configurazioni
- [docs/acquisizione.md](docs/acquisizione.md) – procedura di cattura del traffico
