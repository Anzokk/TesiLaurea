# Estrattore SCL – `scl_extract.py`

Sorgente: `source/scl/scl_extract.py` · Solo libreria standard Python ≥ 3.8

Legge un file SCL (`.icd`, `.cid`, `.scd`) e ricava il modello dati dell'IED:
nodi logici, dati, dataset, GOOSE e Report Control Block. Può generare
direttamente la configurazione per `bin/parser` (MMS + GOOSE).

```sh
python3 source/scl/scl_extract.py config/scl/<file>.icd -o data/results/scl \
        --genera-config config
```

| Opzione | Effetto |
|---|---|
| `-o DIR` | cartella dei file prodotti (default `./out`) |
| `--ied NOME` | considera un solo IED |
| `--tutto` | include nella tabella anche i DO non assegnati a uno scenario |
| `--tutti-fc` | include anche gli attributi CF, DC, EX… |
| `--scenari FILE` | classificazione scenari (default `config/scl/scenari.json`) |
| `--genera-config DIR` | scrive `DIR/parser/<ied>.json` |
| `--sovrascrivi` | con `--genera-config`, sovrascrive i file già presenti |

## File prodotti

| File | Contenuto |
|---|---|
| `tabella_scenari.csv` / `.md` | una riga per DO e dataset: scenario, riferimenti MMS, valori iniziali, GOOSE e report associati |
| `goose_inventario.csv` | GSEControl con gocbRef, datSet, goID, MAC, APPID, VLAN, tempi, filtro tshark |
| `report_inventario.csv` | Report Control Block con le istanze reali da usare nel client (`rif_client`) |
| `modello.json` | modello completo, per altri script |

I CSV usano `;` e codifica UTF-8 con BOM: si aprono direttamente in Excel.

## Configurazioni generate

Un file per IED, `config/parser/<ied>.json` (formato in [parser.md](parser.md)):

- **parametri di esecuzione**: `host` (IP dell'IED letto da `<Communication>`),
  `port` 102, `goose_time_s` 5, `output`; **`interface` resta vuoto e va
  compilato** con l'interfaccia di rete collegata all'IED;

- **punti MMS**: tutti i valori ST e MX dei DO, esclusi qualità (`q`),
  timestamp (`t`) e attributi di servizio dei comandi; targhe dati
  (`NamPlt`, `PhyNam`) in DC. Gli attributi array vengono esclusi e contati;
- **report block**: la prima istanza di ogni RCB;
- **GoCB pubblicati**: gocbRef e APPID, così nell'output del parser i loro
  flussi risultano attesi (`"expected": true`) e con la chiave indicata.

I file esistenti non vengono sovrascritti senza `--sovrascrivi`: le
configurazioni modificate a mano restano al sicuro.

## Note

- **Report Control Block**: con `indexed` (default) e `RptEnabled max="N"`
  l'IED espone le istanze `<nome>01` … `<nome>NN`; il riferimento MMS del
  blocco (`LLN0$RP$nome`) non è leggibile direttamente dal client.
- **Valori iniziali**: si combinano i default dei template (`DA`/`BDA`) con i
  valori assegnati nell'istanza (`DOI`/`SDI`/`DAI`), che hanno la precedenza.
  Confrontarli con le letture MMS verifica la coerenza tra file e dispositivo.
- **Scenari**: un DO entra in uno scenario se la `lnClass` è elencata oppure
  una keyword compare nei nomi/descrizioni; gli LN personalizzati della BU9020
  si aggiungono in `config/scl/scenari.json`, senza toccare il codice.
- **Avvisi**: lo script segnala tipi non definiti nei template, GSEControl
  senza indirizzi in `<Communication>` e tabelle scenari vuote.
