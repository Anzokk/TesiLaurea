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
│   ├── mms/              client MMS in C (ParserMMSV2.c)
│   ├── goose/            analizzatore GOOSE in Python (ParserGooseV2.py)
│   ├── scl/              estrattore del modello SCL (scl_extract.py)
│   └── legacy/           versioni precedenti, conservate per riferimento
├── config/
│   ├── mms/              punti da leggere via MMS (MMSExpected*.json)
│   ├── goose/            criteri di verifica GOOSE (GooseExpected.json)
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
| Client MMS (C) | gcc, [libiec61850](https://github.com/mz-automation/libiec61850) compilata e installata |
| Analizzatore GOOSE | Python ≥ 3.8, `tshark` (Wireshark), `pyshark` (`pip install pyshark`) |
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
make          # compila bin/ParserMMSV2
make check    # controlla la sintassi degli script Python
make clean    # rimuove i file generati
```

## Utilizzo rapido

```sh
# 1. Lettura dati MMS dall'IED
bin/ParserMMSV2 <ip-ied> 102 config/mms/MMSExpectedV2.json data/results/mms.json

# 2. Cattura del traffico GOOSE (60 s)
sudo tshark -i <interfaccia> -a duration:60 -w data/captures/scenario.pcapng

# 3. Analisi GOOSE rispetto ai criteri attesi
python3 source/goose/ParserGooseV2.py data/captures/scenario.pcapng \
        config/goose/GooseExpected.json data/results/goose_report.json

# 4. Estrazione del modello dal file SCL
python3 source/scl/scl_extract.py config/scl/demo_old.icd -o data/results/scl
```

Gli exit code permettono l'uso in script e pipeline: `ParserGooseV2.py` termina
con 0 se tutti i check passano, 1 altrimenti.

## Documentazione

- [docs/mms_client.md](docs/mms_client.md) – client MMS: configurazione e formato dell'output
- [docs/goose_analyzer.md](docs/goose_analyzer.md) – analizzatore GOOSE: check eseguiti e criteri
- [docs/acquisizione.md](docs/acquisizione.md) – procedura di cattura del traffico
