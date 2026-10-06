# Parser IEC 61850 – `bin/parser`

Sorgente: `source/parser/parser.c` · Eseguibile: `bin/parser` (`make`)

In un'unica esecuzione, tramite libiec61850 (protocolli) e cJSON (lettura
della configurazione, pacchetto `libcjson-dev`):

- **MMS**: legge i data attribute elencati e lo stato dei Report Control Block;
- **GOOSE**: riceve in tempo reale tutti i GOOSE presenti in rete durante le
  letture, con statistiche per flusso e ogni frame decodificato.

Il risultato è un file JSON con i **dati grezzi**: le verifiche vengono fatte
dai test Robot Framework. Sostituisce `ParserMMSV2.c` e `ParserGooseV2.py`,
conservati in `source/legacy/`.

```sh
cd IedAnalizer
sudo bin/parser                            # configurazione config/parser/parser.json
sudo bin/parser config/parser/<ied>.json   # altra configurazione
```

`sudo` (o `sudo setcap cap_net_raw+ep bin/parser`, da ripetere dopo ogni
`make`) serve per la ricezione GOOSE, che usa un socket raw.

| Exit code | Significato |
|---|---|
| 0 | completato |
| 1 | configurazione non valida (file assente, JSON malformato, mancano `host` o `interface`) |
| 2 | connessione MMS all'IED fallita (nessun file di output) |
| 3 | impossibile creare il file di output |
| 4 | ricezione GOOSE non avviata (permessi o interfaccia): le letture MMS sono comunque salvate e `goose.error` spiega il motivo |

## Esecuzione

1. legge la configurazione;
2. avvia la ricezione GOOSE sull'interfaccia indicata;
3. si connette all'IED via MMS, legge i data point e lo stato degli RCB;
4. continua ad ascoltare i GOOSE fino a `goose_time_s` secondi dall'avvio
   (anche oltre la fine delle letture), poi ferma la ricezione;
5. scrive il JSON.

## Configurazione

Si genera dal file SCL con `scl_extract.py --genera-config config` (vedi
[scl_extract.md](scl_extract.md)); `interface` va compilato a mano.

```json
{
  "host": "10.145.100.26",
  "port": 102,
  "interface": "enx503f56005b3e",
  "goose_time_s": 5,
  "output": "data/results/parser.json",
  "points": [
    {"ref": "XSCT1M01SMNPBMUCM1/LBMULPHD1.PhyHealth.stVal", "key": "salute_fisica", "fc": "ST"}
  ],
  "report_blocks": [
    {"ref": "XSCT1M01SMNPBMUCM1/LLN0.op_urcb01", "key": "rcb_op", "fc": "RP"}
  ],
  "goose": [
    {"ref": "XSCT1M01SMNPBMUCM1/LLN0$GO$GseBCU_Op_gocb", "key": "goose_op", "appid": "0E65"}
  ]
}
```

| Chiave | Significato |
|---|---|
| `host` | IP dell'IED (obbligatorio) |
| `port` | porta MMS (default 102) |
| `interface` | interfaccia di rete per i GOOSE (obbligatoria; elenco con `ip link`) |
| `goose_time_s` | durata minima dell'ascolto GOOSE in secondi (default 5) |
| `output` | file JSON prodotto (default `data/results/parser_<epoch>.json`) |
| `points` | data attribute: `ref` = `LD/LN.DO.DA` (obbligatorio), `key` = nome nell'output (default `ref`), `fc` = `ST MX SP CF DC SG SE CO RP BR` (default `MX`) |
| `report_blocks` | RCB di cui leggere `RptEna`; la forma `LLN0.RP.Urcb01` è accettata |
| `goose` | GoCB attesi: `ref` = gocbRef come nei frame (`LD/LN$GO$nome`), `key`, `appid` esadecimale con o senza `0x` (facoltativo) |

Vengono **ricevuti tutti i GOOSE**. Quelli elencati in `goose` (gocbRef e,
se indicato, APPID) risultano `"expected": true` con la chiave indicata; gli
altri compaiono come flussi inattesi, con il gocbRef come chiave. Un GoCB
atteso mai ricevuto compare comunque, con `"frames": 0`.

## Output

```json
{
  "host": "10.145.100.26",
  "port": 102,
  "timestamp_wall": 1791279043,
  "connect_latency_ms": 69.699,
  "values": {
    "salute_fisica": {"type": "int", "value": 2},
    "pos_interruttore": {"type": "dbpos", "value": 2, "label": "on", "bits": "10"},
    "qualita": {"type": "bitstring", "value": 0, "size": 13, "bits": "0000000000000"},
    "punto_inesistente": {"type": null, "value": null, "access_error": 10}
  },
  "report_blocks": {"rcb_op": {"RptEna": false}},
  "total_read_time_ms": 650.665,
  "goose": {
    "interface": "enx503f56005b3e",
    "listen_time_ms": 5003.2,
    "error": null,
    "frames_total": 3,
    "frames_dropped": 0,
    "streams": {
      "goose_op": {"expected": true,
        "gocb_ref": "XSCT1M01SMNPBMUCM1/LLN0$GO$GseBCU_Op_gocb",
        "go_id": "XSCT1M01SMNPBMUCM1/LLN0.GseBCU_Op_gocb",
        "dat_set": "XSCT1M01SMNPBMUCM1/LLN0$ds_GseBCU_Op",
        "appid": "0x0e65", "vlan_id": 7,
        "src_mac": "20:bb:76:91:64:1a", "dst_mac": "01:0c:cd:01:3d:65",
        "frames": 3, "first_rx_ms": 1791279040055.122, "last_rx_ms": 1791279044055.3,
        "stnum_changes": 0, "sqnum_resets": 0, "max_interval_ms": 2000.2,
        "ttl_violations": 0, "invalid_frames": 0}
    },
    "frames": [
      {"stream": "goose_op", "n": 1, "rx_ms": 1791279040055.122, "st_num": 2, "sq_num": 756,
       "ttl_ms": 4000, "t_ms": 389866883963, "conf_rev": 1, "entries": 6,
       "valid": true, "test": false, "nds_com": false,
       "values": [{"type": "bool", "value": true},
                  {"type": "bitstring", "value": 0, "size": 13, "bits": "0000000000000"},
                  {"type": "utc_time", "value": 304120}, "..."]}
    ]
  }
}
```

**Valori** (`values` MMS e `frames[].values` GOOSE): ogni valore è un oggetto
con `type` e `value`, nello stesso formato per MMS e GOOSE.

| `type` | Tipo MMS | Campi aggiuntivi |
|---|---|---|
| `float` | FLOAT (9 cifre significative) | |
| `int` / `uint` | INTEGER (anche enumerati) / UNSIGNED | |
| `bool` | BOOLEAN | |
| `dbpos` | BIT STRING di 2 bit: `value` 0 intermedio, 1 off, 2 on, 3 bad | `label`, `bits` |
| `bitstring` | altre BIT STRING (es. quality): `value` intero (bit 0 = più significativo) | `size`, `bits` (bit 0 a sinistra) |
| `string` | VISIBLE STRING / MMS STRING | |
| `octets` | OCTET STRING, in esadecimale | |
| `utc_time` / `binary_time` | tempi, in ms da epoch | |
| `struct` / `array` | strutture e array: `value` è la lista dei valori interni, tipizzati | |

Leggendo un DO intero (es. `GGIO1.AnIn1` con FC `MX`) si ottiene una `struct`
con tutti i suoi attributi (valore, qualità, timestamp) in una sola richiesta.

Attenzione: anche **Tcmd** (es. `ATCC.TapChg`, comando di salita/discesa tap)
è una bit string di 2 bit e compare come `dbpos`: in quel caso `value` 1 =
lower, 2 = higher e `label` va ignorata.

Errori MMS: `error` = codice `IedClientError` (comunicazione); `access_error`
= codice `MmsDataAccessError` restituito dall'IED (10 = oggetto inesistente,
3 = accesso negato); in entrambi i casi `type` e `value` sono `null`.

Errori di configurazione: un punto o report block senza `ref`, o con un FC
non valido, **non viene letto** e compare nell'output con `config_error`
(es. `{"type": null, "value": null, "config_error": "FC sconosciuto \"STX\""}`),
sotto la sua `key` o, se manca anche quella, sotto `senza_ref_<n>` (n =
posizione nell'elenco, da 0). Così anche i test Robot vedono l'errore.

**Flussi GOOSE** (`goose.streams`), statistiche calcolate nell'ordine di
ricezione:

| Campo | Significato |
|---|---|
| `expected` | GoCB elencato nella configurazione |
| `frames` | frame ricevuti (0 = GoCB atteso mai visto) |
| `stnum_changes` | cambi di stNum (eventi) |
| `sqnum_resets` | cambi di stNum in cui sqNum è ripartito da 0 o 1 |
| `max_interval_ms` | intervallo massimo tra due frame consecutivi |
| `ttl_violations` | frame arrivati oltre il TTL dichiarato dal precedente |
| `invalid_frames` | frame segnalati come non validi da libiec61850 |

**Frame GOOSE** (`goose.frames`): `n` progressivo di ricezione, `rx_ms`
istante di ricezione (orologio del PC), `t_ms` timestamp interno del GOOSE
(orologio dell'IED, ultimo cambio di stato), `entries` numero di valori,
`values` lista dei valori del dataset, tipizzati come sopra.

## Note

- `rx_ms` è preso nella callback del programma: per misure di latenza molto
  fini il timestamp del kernel di tshark/dumpcap è più preciso.
- I comandi usano percorsi relativi (configurazione di default, `output`):
  vanno lanciati dalla cartella `IedAnalizer`, o con percorsi assoluti.
- Limiti interni: 100000 frame e 64 flussi; quelli in eccesso sono contati
  in `frames_dropped`.
