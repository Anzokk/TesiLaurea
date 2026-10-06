# Analizzatore GOOSE – `ParserGooseV2.py`

Sorgente: `source/goose/ParserGooseV2.py`

Analizza una cattura di rete (`.pcapng`) e verifica che i messaggi GOOSE
pubblicati dall'IED rispettino i criteri di uno scenario di test.

```sh
python3 source/goose/ParserGooseV2.py <cattura.pcapng> <expected.json> <report.json>
```

Exit code: `0` tutti i check superati, `1` almeno un check fallito.

## Criteri (`expected.json`)

Esempio in `config/goose/`. Tutte le chiavi sono opzionali: un check viene
eseguito solo se la relativa chiave è presente.

| Chiave | Significato |
|---|---|
| `scenario` | nome dello scenario, riportato nel report |
| `expected_gocb_ref` | GoCB sotto test; i check di sequenza considerano solo i suoi frame |
| `expected_dataset`, `expected_go_id` | datSet e goID attesi |
| `expected_conf_rev` | confRev atteso |
| `expected_stnum_changes` | numero di cambi di stato (eventi) attesi |
| `expected_entries` | numero di valori nel payload di ogni frame |
| `max_retransmission_interval_ms` | limite fisso sull'intervallo tra frame consecutivi |
| `event_time_epoch` | istante dell'evento (epoch, s) |
| `clock_offset_ms` | correzione dell'orologio dell'evento rispetto a quello della cattura |
| `max_event_to_goose_latency_ms` | latenza massima evento → primo GOOSE con il nuovo stato |
| `forbid_unexpected_frames` | nessun frame di GoCB diversi da quello atteso |

## Check eseguiti

| Check | Superato se |
|---|---|
| `frames_present` | la cattura contiene almeno un frame GOOSE |
| `frames_decoded` | tutti i campi obbligatori dell'header sono stati decodificati |
| `gocb_ref_match` | il GoCB atteso compare nella cattura |
| `dataset_match`, `go_id_match` | tutti i frame del GoCB riportano i valori attesi |
| `conf_rev` / `conf_rev_stable` | confRev uguale a quello atteso / costante |
| `stnum_changes` | il numero di cambi di stNum è quello atteso |
| `sqnum_reset` | a ogni cambio di stNum, sqNum riparte da 0 o 1 |
| `ttl_respected` | ogni frame arriva entro il timeAllowedtoLive del precedente |
| `retransmission_interval` | l'intervallo massimo non supera il limite fisso |
| `event_to_goose_latency` | il primo frame con il nuovo stNum arriva entro il limite |
| `no_unexpected_frames` | non ci sono frame di altri GoCB |
| `payload_entries` | ogni frame contiene il numero di valori atteso |

Per ogni check fallito il report elenca i frame coinvolti (massimo 10).

## Note

- La latenza è significativa solo se `event_time_epoch` e la cattura usano
  orologi sincronizzati; se l'evento cade fuori dalla finestra della cattura
  il check lo segnala esplicitamente.
- Il limite `max_retransmission_interval_ms` va scelto in base all'heartbeat
  configurato sull'IED: nella cattura `data/captures/baseline.pcapng`
  l'heartbeat è ~2 s (massimo osservato 2185 ms), TTL 4000 ms.
- tshark 4.2.2 marca come "malformati" i GOOSE con dati annidati (bug del
  dissector, `recursion_depth <= 100`) pur decodificando correttamente
  l'header: il numero è riportato in `malformed_frames` come avviso.
