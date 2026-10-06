# Client MMS – `ParserMMSV2`

Sorgente: `source/mms/ParserMMSV2.c` · Eseguibile: `bin/ParserMMSV2` (`make`)

Si collega all'IED via MMS (libiec61850), legge i data attribute elencati in un
file di configurazione e salva valori, tipi e tempi di risposta in un file JSON.

```sh
bin/ParserMMSV2 <host> <port> <config.json> <output.json>
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
  ]
}
```

- `ref`: riferimento IEC 61850 `LD/LN.DO.DA` (obbligatorio);
- `key`: nome nel JSON di output (default: `ref`);
- `fc`: Functional Constraint – `ST MX SP CF DC SG SE CO RP BR` (default: `MX`).

Per i Report Control Block il FC si indica solo in `fc`; la forma
`LLN0.RP.Urcb01` è comunque accettata (il segmento `.RP.`/`.BR.` viene rimosso).

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
