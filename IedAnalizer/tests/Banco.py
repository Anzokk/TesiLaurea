"""Libreria Robot Framework del banco di prova BU9020.

Keyword: Esegui Scenario <file.yaml>

Per ogni passo dello scenario:
  1. avvia bin/parser (ricezione GOOSE + letture MMS);
  2. dopo AVVIO_S secondi invia gli stimoli all'ESP32 (istante t0 del passo);
  3. attende la fine del parser (attesa_s secondi dopo lo stimolo).
Le letture MMS di un parser avvengono prima del suo stimolo: lo stato MMS
dopo il passo i e' quindi quello letto dal parser del passo i+1 (dopo
l'ultimo passo c'e' una lettura finale senza stimolo).
Per tutta la durata gira tshark; alla fine si confrontano parser e cattura
con i valori attesi. Risultati in data/results/<scenario>/.

Formato dello scenario (YAML):
  nome: testo
  gocbRef: LD/LLN0$GO$nome         GoCB da ricevere col parser
  punti: {LD/LN.DO.DA: FC}         punti MMS da leggere
  rete: {campo_tshark: valore}     uguale in ogni frame GOOSE della cattura
  passi:
    - nome: testo
      stimolo: [comando, ...]      righe inviate all'ESP32
      attesa_s: 3                  secondi di osservazione dopo lo stimolo
      atteso:
        mms: {LD/LN.DO.DA: valore | {min: x, max: y}}
        stati_goose: n             nuovi stNum dopo lo stimolo
        latenza_max_ms: x          stimolo -> primo GOOSE con nuovo stNum
        sequenza: {i: [v, ...]}    valori del membro i del dataset dopo lo
                                   stimolo (ripetizioni consecutive tolte)
Sempre verificati sulla cattura: sequenza stNum/sqNum, intervallo tra due
frame entro il TTL del precedente, frame del parser presenti nella cattura.
"""
import json
import math
import subprocess
import time
from pathlib import Path

import serial
import yaml
from robot.api import logger

AVVIO_S = 2     # tempo per avviare il GOOSE e fare le letture MMS nel parser


def norm(v):
    """Confronto tra valori YAML e testo di tshark (0x0e65 = 3685, False = false)."""
    s = str(v).strip().lower()
    try:
        return int(s, 0)
    except ValueError:
        return s


class Banco:
    def __init__(self, config, seriale, baud=115200):
        self.cfg = json.loads(Path(config).read_text())
        self.seriale, self.baud = seriale, int(baud)

    def esegui_scenario(self, file):
        sc = yaml.safe_load(Path(file).read_text())
        out = Path("data/results") / Path(file).stem
        out.mkdir(parents=True, exist_ok=True)
        pcap = out / "cattura.pcapng"
        cattura = subprocess.Popen(["tshark", "-q", "-i", self.cfg["interface"], "-w", pcap],
                                   stderr=subprocess.DEVNULL)
        try:            # tshark va fermato anche se lo scenario si interrompe
            time.sleep(3)   # almeno una ritrasmissione GOOSE prima del primo stimolo
            esp = None
            if any(p.get("stimolo") for p in sc["passi"]):
                esp = serial.Serial(self.seriale, self.baud)
            runs, t0 = [], []
            for i, passo in enumerate(sc["passi"] + [{"nome": "lettura finale"}]):
                cfg = dict(self.cfg, gocbRef=sc["gocbRef"], points=sc["punti"],
                           goose_time_s=math.ceil(AVVIO_S + passo.get("attesa_s", 0)))
                (out / f"config{i}.json").write_text(json.dumps(cfg, indent=2))
                parser = subprocess.Popen(["bin/parser", out / f"config{i}.json"],
                                          stdout=subprocess.PIPE, text=True)
                time.sleep(AVVIO_S)
                t0.append(time.time())
                if passo.get("stimolo"):
                    esp.write("".join(c + "\n" for c in passo["stimolo"]).encode())
                    logger.info(f"{passo['nome']}: stimolo {passo['stimolo']}")
                stdout, _ = parser.communicate()
                if parser.returncode:
                    raise RuntimeError(f"parser fallito al passo '{passo['nome']}'")
                (out / f"passo{i}.json").write_text(stdout)
                runs.append(json.loads(stdout))
        finally:
            cattura.terminate()
            cattura.wait()
        errori = self._verifica(sc, runs, t0, pcap)
        if errori:
            raise AssertionError("\n".join(errori))

    def _verifica(self, sc, runs, t0, pcap):
        rete = sc.get("rete", {})
        campi = ["frame.time_epoch", "goose.gocbRef", "goose.stNum", "goose.sqNum",
                 "goose.timeAllowedtoLive", "goose.goID", "goose.datSet", *rete]
        righe = subprocess.run(["tshark", "-r", pcap, "-Y", "goose", "-T", "fields",
                                *[a for c in campi for a in ("-e", c)]],
                               capture_output=True, text=True, check=True).stdout.splitlines()
        frame = [dict(zip(campi, r.split("\t"))) for r in righe]
        errori = sorted({f"rete: {c} = {f[c]} invece di {v}"
                         for f in frame for c, v in rete.items() if norm(f[c]) != norm(v)})

        # GOOSE del GoCB nella cattura
        g = [{"t": float(f["frame.time_epoch"]), "st": int(f["goose.stNum"]),
              "sq": int(f["goose.sqNum"]), "ttl": int(f["goose.timeAllowedtoLive"]),
              "goID": f["goose.goID"], "datSet": f["goose.datSet"]}
             for f in frame if f["goose.gocbRef"] == sc["gocbRef"]]
        if not g:
            return errori + [f"nessun GOOSE di {sc['gocbRef']} nella cattura"]
        for a, b in zip(g, g[1:]):
            if not (b["st"] == a["st"] and b["sq"] == a["sq"] + 1
                    or b["st"] == a["st"] + 1 and b["sq"] == 0):
                errori.append(f"sequenza: stNum/sqNum {a['st']}/{a['sq']} -> {b['st']}/{b['sq']}")
            if (b["t"] - a["t"]) * 1000 > a["ttl"]:
                errori.append(f"TTL: {(b['t'] - a['t']) * 1000:.0f} ms senza GOOSE "
                              f"dopo {a['st']}/{a['sq']} (TTL {a['ttl']} ms)")

        # Controllo incrociato parser / tshark
        rif = {(f["st"], f["sq"]): f for f in g}
        for r in runs:
            for f in r["goose"]:
                c = rif.get((f["stNum"], f["sqNum"]))
                if not c or (c["goID"], c["datSet"], c["ttl"]) != \
                        (f["goID"], f["datSet"], f["timeAllowedtoLive"]):
                    errori.append(f"parser/tshark: frame {f['stNum']}/{f['sqNum']} diverso o assente")

        # Valori attesi di ogni passo
        for i, passo in enumerate(sc["passi"]):
            nome, att = passo["nome"], passo.get("atteso", {})
            for ref, v in att.get("mms", {}).items():
                letto = runs[i + 1]["mms"].get(ref)
                if isinstance(v, dict):
                    ok = isinstance(letto, (int, float)) and v["min"] <= letto <= v["max"]
                else:
                    ok = letto == v
                if not ok:
                    errori.append(f"{nome}: {ref} = {letto} invece di {v}")

            prima = [f for f in g if f["t"] < t0[i]]
            if not prima:
                errori.append(f"{nome}: nessun GOOSE prima dello stimolo")
                continue
            st0 = prima[-1]["st"]
            nuovi = [f for f in g if t0[i] <= f["t"] < t0[i + 1] and f["st"] > st0]
            if nuovi:
                latenza = (nuovi[0]["t"] - t0[i]) * 1000
                logger.info(f"{nome}: latenza stimolo -> GOOSE {latenza:.1f} ms")
            if "stati_goose" in att and len({f["st"] for f in nuovi}) != att["stati_goose"]:
                errori.append(f"{nome}: {len({f['st'] for f in nuovi})} nuovi stNum "
                              f"invece di {att['stati_goose']}")
            if "latenza_max_ms" in att and (not nuovi or latenza > att["latenza_max_ms"]):
                errori.append(f"{nome}: latenza " + (f"{latenza:.1f} ms" if nuovi else "assente")
                              + f" (max {att['latenza_max_ms']} ms)")
            dati = [f["allData"] for r in runs[i:i + 2] for f in r["goose"]
                    if f["stNum"] > st0 and f["rxMs"] < t0[i + 1] * 1000]
            for m, valori in att.get("sequenza", {}).items():
                visti = []
                for d in dati:
                    if not visti or visti[-1] != d[m]:
                        visti.append(d[m])
                if visti != valori:
                    errori.append(f"{nome}: membro {m} del dataset {visti} invece di {valori}")
        return errori
