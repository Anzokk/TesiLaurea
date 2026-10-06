# Acquisizione del traffico GOOSE

Cattura di 60 s sull'interfaccia collegata alla rete di stazione
(comando originale in `comandi_acquisizione.txt`):

```sh
sudo tshark -i enx503f56005b3e -a duration:60 -w - > data/captures/baseline_cassetto.pcapng
```

- `-i`: interfaccia di rete (elenco con `tshark -D`);
- `-a duration:60`: termina dopo 60 secondi;
- `-w -`: scrive la cattura su standard output, rediretta nel file.

Per limitare la cattura al solo traffico GOOSE si può aggiungere il filtro
di cattura `-f "ether proto 0x88b8"`.

Le catture vanno salvate in `data/captures/` con un nome che identifichi lo
scenario (es. `sync_check_ok_2026-10-05.pcapng`) e analizzate con
`source/goose/ParserGooseV2.py` (vedi [goose_analyzer.md](goose_analyzer.md)).
