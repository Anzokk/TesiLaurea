# Parser IEC 61850 – `bin/parser`

Sorgente: `source/parser/parser.c` · Eseguibile: `bin/parser` (`make`)

Tramite libiec61850 (protocolli) e cJSON (pacchetto `libcjson-dev`), in
un'unica esecuzione:

- riceve i **GOOSE** di un GoCB dell'IED;
- nel frattempo legge via **MMS** i data attribute elencati;
- stampa tutto in JSON su **stdout**.

Il parser produce solo i dati grezzi: le verifiche (sequenze di stNum/sqNum,
TTL, intervalli, confronto con tshark e con il file `.icd`) sono fatte dalla
suite di test in Python.

```sh
cd IedAnalizer
sudo bin/parser config/parser/parser.json > data/results/run.json
```

`sudo` (o `sudo setcap cap_net_raw+ep bin/parser`, da ripetere dopo ogni
`make`) serve per la ricezione GOOSE, che usa un socket raw.

Exit code: **0** completato, **1** errore. Il motivo dell'errore è su stderr
(config non valida, GOOSE non avviato, connessione MMS fallita); in quel caso
su stdout non viene scritto nulla.

## Esecuzione

1. legge la configurazione;
2. avvia la ricezione GOOSE;
3. si connette all'IED via MMS e legge i punti, uno alla volta;
4. ascolta i GOOSE per altri `goose_time_s` secondi, poi ferma la ricezione;
5. stampa il JSON.

I GOOSE sono quindi registrati per tutta la durata delle letture più
`goose_time_s`.

## Configurazione

Si scrive a mano, con i riferimenti presi dal file SCL dell'IED
(`config/scl/*.icd`). Tutti i campi sono obbligatori.

```json
{
  "host": "10.145.100.26",
  "port": 102,
  "interface": "enx503f56005b3e",
  "goose_time_s": 5,
  "gocbRef": "XSCT1M01SMNPBMUCM1/LLN0$GO$GseBCU_Op_gocb",
  "points": {
    "XSCT1M01SMNPBMUCM1/LBMULPHD1.PhyHealth.stVal": "ST",
    "XSCT1M01SMNPBMUCM1/LLN0.op_urcb01.RptEna": "RP"
  }
}
```

| Chiave | Significato |
|---|---|
| `host`, `port` | indirizzo MMS dell'IED (porta di norma 102) |
| `interface` | interfaccia di rete per i GOOSE (elenco con `ip link`) |
| `goose_time_s` | secondi di ascolto GOOSE dopo le letture MMS (intero) |
| `gocbRef` | GoCB da ricevere, come nei frame (`LD/LN$GO$nome`); gli altri GOOSE sono scartati |
| `points` | riferimento `LD/LN.DO.DA` → FC (`ST MX SP CF DC SG SE CO RP BR`) |

Lo stato di un Report Control Block si legge come un normale punto:
`LD/LLN0.<rcb>.RptEna` con FC `RP` o `BR`.

## Output

```json
{
  "mms": {
    "XSCT1M01SMNPBMUCM1/LBMULPHD1.PhyHealth.stVal": 2,
    "XSCT1M01SMNPBMUCM1/LLN0.Health.q": "0000000000000",
    "XSCT1M01SMNPBMUCM1/LLN0.Xyz.stVal": {"accessError": 10},
    "XSCT1M01SMNPBMUCM1/LLN0.op_urcb01.RptEna": false
  },
  "goose": [
    {"rxMs": 1791279040055.122, "goID": "XSCT1M01SMNPBMUCM1/LLN0.GseBCU_Op_gocb",
     "datSet": "XSCT1M01SMNPBMUCM1/LLN0$ds_GseBCU_Op",
     "stNum": 2, "sqNum": 756, "timeAllowedtoLive": 4000, "t": 389866883963,
     "allData": [true, "0000000000000", 304120]}
  ]
}
```

**`mms`**: un valore per punto, sotto il suo riferimento.

**`goose`**: un elemento per frame, in ordine di ricezione. `rxMs` è
l'istante di ricezione (orologio del PC, ms da epoch), `t` il timestamp del
GOOSE (orologio dell'IED, ultimo cambio di stato), `allData` i valori del
dataset nell'ordine dei membri.

**Valori**, uguali per MMS e GOOSE:

| Tipo MMS | Valore JSON |
|---|---|
| BOOLEAN | `true` / `false` |
| INTEGER (anche enumerati: Mod, Beh, Health) | numero |
| FLOAT | numero (`null` se NaN o infinito) |
| UTC TIME | numero, ms da epoch |
| VISIBLE STRING / MMS STRING | stringa |
| BIT STRING (quality, Dbpos, Tcmd...) | stringa di bit, bit 0 a sinistra |
| STRUCTURE / ARRAY (es. un DO intero) | array dei valori interni |
| altri tipi | `null` |

Le bit string non vengono interpretate: Dbpos (`"01"` off, `"10"` on) e Tcmd
(`"01"` lower, `"10"` higher) sono entrambe di 2 bit e si distinguono solo
dal tipo del DA nel file `.icd`.

**Errori** (al posto del valore):

| Forma | Significato |
|---|---|
| `{"clientError": n}` | richiesta MMS fallita, `n` = `IedClientError` (5 IED irraggiungibile, 12 riferimento non valido, 20 timeout) |
| `{"accessError": n}` | l'IED ha rifiutato l'accesso, `n` = `MmsDataAccessError` (10 oggetto inesistente, 3 accesso negato) |

## Note

- `rxMs` è preso nella callback: per le latenze il timestamp del kernel di
  tshark/dumpcap è più preciso.
- MAC, APPID e VLAN non sono nell'output: si controllano sulla cattura tshark.
- L'output è costruito in memoria e stampato alla fine: adatto ad ascolti di
  secondi o minuti.
