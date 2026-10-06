# Client MMS – `ParserMMSV2`

Sorgente: `source/mms/ParserMMSV2.c` · Eseguibile: `bin/ParserMMSV2` (`make`)

Si collega all'IED via MMS (libiec61850), legge i data attribute elencati in un
file di configurazione e salva valori, tipi e tempi di risposta in un file JSON.
Facoltativamente riceve anche i messaggi **GOOSE**, in tempo reale o da un file
di cattura, con il decoder di libiec61850.

```sh
bin/ParserMMSV2 <host|-> <port> <config.json> <output.json> [opzioni GOOSE]
```

| Opzione | Effetto |
|---|---|
| `--goose <interfaccia>` | riceve i GOOSE in tempo reale: l'ascolto parte prima della connessione MMS e prosegue durante le letture (richiede `sudo` o `CAP_NET_RAW`) |
| `--goose-time <s>` | durata minima dell'ascolto in tempo reale (default 5 s) |
| `--goose-pcap <file>` | decodifica i GOOSE di una cattura `.pcap`/`.pcapng` (offline, senza privilegi) |
| host `-` | nessuna connessione MMS: solo GOOSE |

Esempi:

```sh
# letture MMS + GOOSE ricevuti nello stesso intervallo (10 s)
sudo bin/ParserMMSV2 10.145.100.26 102 config/mms/demo_old.json data/results/run.json \
     --goose enx503f56005b3e --goose-time 10

# solo analisi GOOSE di una cattura esistente
bin/ParserMMSV2 - 0 config/mms/demo_old.json data/results/goose.json \
     --goose-pcap data/captures/baseline.pcapng
```

| Exit code | Significato |
|---|---|
| 0 | letture completate (eventuali errori sui singoli punti sono nel JSON) |
| 1 | argomenti mancanti o configurazione non valida |
| 2 | connessione all'IED fallita |
| 3 | impossibile creare il file di output |

## Configurazione

Esempi in `config/mms/`.

```json
{
  "points": [
    {"ref": "BU9020/XCBR1.Pos.stVal",   "key": "pos_interruttore", "fc": "ST"},
    {"ref": "BU9020/MMXU1.TotW.mag.f",  "key": "potenza_attiva",   "fc": "MX"}
  ],
  "report_blocks": [
    {"ref": "BU9020/LLN0.Urcb01", "key": "rcb_urcb01", "fc": "RP"}
  ],
  "goose": [
    {"ref": "BU9020CTRL/LLN0$GO$GcbSync", "key": "gcb_sync", "appid": "0x0001"}
  ]
}
```

- `ref`: riferimento IEC 61850 `LD/LN.DO.DA` (obbligatorio);
- `key`: nome nel JSON di output (default: `ref`);
- `fc`: Functional Constraint – `ST MX SP CF DC SG SE CO RP BR` (default: `MX`).

Per i Report Control Block il FC si indica solo in `fc`; la forma
`LLN0.RP.Urcb01` è comunque accettata (il segmento `.RP.`/`.BR.` viene rimosso).

Sezione `goose` (facoltativa):

- `ref`: gocbRef come compare nei frame, `LD/LN$GO$nome` (obbligatorio);
- `key`: nome del flusso nel JSON di output (default: `ref`);
- `appid`: APPID esadecimale, con o senza `0x` (facoltativo; se indicato,
  i frame con APPID diverso vengono scartati).

Se la sezione manca o è vuota, con le opzioni GOOSE si registrano **tutti** i
GOOSE presenti (modo *observer*). Con `scl_extract.py --genera-config` la
sezione viene compilata dal file SCL.

## Output

```json
{
  "host": "10.0.0.10",
  "port": 102,
  "timestamp_wall": 1791207965,
  "connect_latency_ms": 0.642,
  "values": {
    "potenza_attiva":   {"value": 1250.5, "type": "float"},
    "pos_interruttore": {"value": 2, "type": "dbpos", "label": "on"},
    "punto_inesistente":{"value": null, "access_error": 10}
  },
  "report_blocks": {
    "rcb_urcb01": {"RptEna": false}
  },
  "total_read_time_ms": 0.426
}
```

| `type` | Tipo MMS |
|---|---|
| `float` | FLOAT |
| `int` / `uint` | INTEGER (anche enumerati) / UNSIGNED |
| `bool` | BOOLEAN |
| `dbpos` | BIT STRING di 2 bit: 0 intermedio, 1 off, 2 on, 3 bad |
| `bitstring` | altre BIT STRING (es. quality), con numero di bit |
| `string` | VISIBLE STRING / MMS STRING |
| `utc_time` | UTC TIME, in ms da epoch |

Errori: `"error"` = codice `IedClientError` (errore di comunicazione);
`"access_error"` = codice `MmsDataAccessError` restituito dall'IED
(10 = oggetto inesistente).

## Output GOOSE

Con le opzioni GOOSE il JSON contiene anche la sezione `goose`:

```json
"goose": {
  "mode": "subscriber",
  "source": "pcap:data/captures/baseline.pcapng",
  "listen_time_ms": null,
  "error": null,
  "frames_total": 31,
  "frames_dropped": 0,
  "streams": {
    "goose_gsebcu_op_gocb": {
      "gocb_ref": "XSCT1M01SMNPBMUCM1/LLN0$GO$GseBCU_Op_gocb",
      "go_id": "XSCT1M01SMNPBMUCM1/LLN0.GseBCU_Op_gocb",
      "dat_set": "XSCT1M01SMNPBMUCM1/LLN0$ds_GseBCU_Op",
      "appid": "0x0e65", "vlan_id": 7,
      "src_mac": "20:bb:76:91:64:1a", "dst_mac": "01:0c:cd:01:3d:65",
      "frames": 31, "first_rx_ms": 1791207965055.122, "last_rx_ms": 1791208024951.3,
      "stnum_changes": 0, "sqnum_resets": 0, "max_interval_ms": 2185.53,
      "ttl_violations": 0, "invalid_frames": 0
    }
  },
  "frames": [
    {"stream": "goose_gsebcu_op_gocb", "rx_ms": 1791207965055.122, "st_num": 2, "sq_num": 756,
     "ttl_ms": 4000, "t_ms": 389866883963, "conf_rev": 1, "valid": true, "test": false,
     "nds_com": false, "values": [true, "0000000000000", 304120, false, "0000000000000", 1262304000000]}
  ]
}
```

| Campo | Significato |
|---|---|
| `mode` | `subscriber` (GoCB della configurazione) o `observer` (tutti) |
| `error` | es. ricezione non avviata per mancanza di privilegi; le letture MMS proseguono comunque |
| `frames_dropped` | frame non registrati per i limiti interni (100000 frame, 64 flussi) |
| `streams.<key>.frames` | `0` = GoCB atteso ma **mai ricevuto** (o APPID diverso) |
| `stnum_changes`, `sqnum_resets` | eventi e ripartenze di sqNum dopo ogni evento |
| `max_interval_ms`, `ttl_violations` | intervallo massimo tra frame e frame arrivati oltre il TTL del precedente |
| `rx_ms` | istante di ricezione (ms da epoch): orologio del PC in tempo reale, timestamp della cattura con `--goose-pcap` |
| `t_ms` | timestamp interno del GOOSE (ultimo cambio di stato, orologio dell'IED) |
| `values` | dataset decodificato: strutture come array, bit string come `"0101..."` (bit 0 a sinistra), tempi in ms |

Note:

- in tempo reale `rx_ms` è preso nella callback (spazio utente): è meno preciso
  del timestamp di tshark/dumpcap, preso dal kernel. Per misure di latenza
  fini resta preferibile la cattura con tshark; i due metodi danno gli stessi
  campi (verificato su `baseline.pcapng`: 31 frame identici);
- il decoder di libiec61850 legge correttamente il dataset (`allData`) anche
  dove tshark 4.2.2 segnala i frame come malformati.
