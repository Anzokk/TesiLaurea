#!/usr/bin/env python3
"""
goose_parse.py - decodifica i frame GOOSE (IEC 61850-8-1) da un .pcap/.pcapng
senza dipendere dal dissettore di tshark (aggirando il bug "recursion_depth").

Uso da riga di comando:
  python3 goose_parse.py cattura.pcapng                    # riassunto per flusso
  python3 goose_parse.py cattura.pcapng -o frame.csv       # + CSV, un frame per riga
  python3 goose_parse.py cattura.pcapng --json frame.json  # + JSON con dati annidati
  python3 goose_parse.py cattura.pcapng --analizza         # + controlli di coerenza
  python3 goose_parse.py cattura.pcapng --analizza --stimolo-epoch 1791191950.250
  python3 goose_parse.py cattura.pcapng --appid 0x0e65 --gocbref GseBCU_Op

Filtri: --appid, --gocbref (sottostringa), --dst (MAC destinazione).

Codici di uscita (utili in una pipeline di test / Robot Framework):
  0  tutto ok
  1  con --analizza: trovate anomalie di livello ERRORE
  2  con --stimolo-epoch: nessun cambio di stNum dopo lo stimolo
  3  nessun frame GOOSE trovato (dopo i filtri)

Da Python:
  from goose_parse import parse_file, analizza
  frames = parse_file("cattura.pcapng")
  risultato = analizza(frames, stimolo_epoch=1791191950.25)

Richiede: pip install scapy   (usato solo per leggere il file pcap/pcapng;
la decodifica Ethernet/VLAN/GOOSE/BER e' fatta qui a mano).
"""
import argparse
import csv
import json
import statistics
import struct
import sys
from datetime import datetime, timedelta, timezone

GOOSE_ETHERTYPE = 0x88B8
VLAN_ETHERTYPES = (0x8100, 0x88A8, 0x9100)
MAX_DEPTH = 16
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
U32_MAX = 0xFFFFFFFF

# tag dei campi del goosePdu (IEC 61850-8-1)
PDU_FIELDS = {
    0x80: "gocbRef", 0x81: "timeAllowedToLive", 0x82: "datSet", 0x83: "goID",
    0x84: "t", 0x85: "stNum", 0x86: "sqNum", 0x87: "simulation",
    0x88: "confRev", 0x89: "ndsCom", 0x8A: "numDatSetEntries",
}
VALIDITY = {"00": "good", "01": "invalid", "10": "reserved", "11": "questionable"}

CSV_FIELDS = [
    "frame", "time_epoch", "time_utc", "dt_prev_s", "src", "dst", "vlan_id",
    "vlan_prio", "appid", "reserved1", "reserved2", "gocbRef", "datSet", "goID",
    "timeAllowedToLive", "t_utc", "t_flags", "stNum", "sqNum", "simulation",
    "confRev", "ndsCom", "numDatSetEntries", "n_items", "data_flat", "parse_error",
]


class BerError(Exception):
    pass


# ------------------------------- BER -------------------------------------
def read_tlv(b, i, end):
    """Legge un TLV BER in b[i:end]. Ritorna (tag, inizio_valore, fine_valore)."""
    if i + 2 > end:
        raise BerError("TLV troncato")
    tag = b[i]
    i += 1
    if tag & 0x1F == 0x1F:
        raise BerError("tag multi-byte non supportato")
    ln = b[i]
    i += 1
    if ln == 0x80:
        raise BerError("lunghezza indefinita non ammessa in GOOSE")
    if ln & 0x80:
        n = ln & 0x7F
        if n == 0 or n > 4 or i + n > end:
            raise BerError("lunghezza BER non valida")
        ln = int.from_bytes(b[i:i + n], "big")
        i += n
    if i + ln > end:
        raise BerError(f"valore oltre la fine del PDU (tag 0x{tag:02x})")
    return tag, i, i + ln


def utc_iso(sec, frac24):
    dt = EPOCH + timedelta(seconds=sec, microseconds=int(frac24 / 2 ** 24 * 1e6))
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


def decode_utctime(v):
    """UtcTime IEC 61850: 4 byte secondi, 3 byte frazione, 1 byte qualita' tempo."""
    if len(v) != 8:
        return {"type": "utc-time", "value": v.hex(), "note": "lunghezza anomala"}
    q = v[7]
    flags = []
    if q & 0x80:
        flags.append("LeapSecondsKnown")
    if q & 0x40:
        flags.append("ClockFailure")
    if q & 0x20:
        flags.append("ClockNotSynchronized")
    flags.append(f"acc={q & 0x1F}")
    return {"type": "utc-time",
            "value": utc_iso(int.from_bytes(v[:4], "big"), int.from_bytes(v[4:7], "big")),
            "note": ",".join(flags)}


def decode_bits(v):
    if not v:
        return ""
    pad = v[0]
    bits = "".join(f"{x:08b}" for x in v[1:])
    return bits[:len(bits) - pad] if 0 < pad < 8 else bits


def decode_data(b, start, end, depth=0):
    """Decodifica ricorsivamente allData (CHOICE Data). Ritorna lista di dict."""
    if depth > MAX_DEPTH:
        raise BerError("annidamento eccessivo in allData")
    out, i = [], start
    while i < end:
        tag, vs, ve = read_tlv(b, i, end)
        v = b[vs:ve]
        i = ve
        if tag in (0xA1, 0xA2):
            out.append({"type": "array" if tag == 0xA1 else "struct",
                        "value": decode_data(b, vs, ve, depth + 1)})
        elif tag == 0x83:
            out.append({"type": "boolean", "value": bool(v[0]) if v else None})
        elif tag in (0x84, 0x8E):
            bits = decode_bits(v)
            d = {"type": "bit-string" if tag == 0x84 else "boolean-array", "value": bits}
            if tag == 0x84 and len(bits) == 13:      # Quality
                d["note"] = "validity=" + VALIDITY[bits[:2]]
            out.append(d)
        elif tag == 0x85:
            out.append({"type": "integer", "value": int.from_bytes(v, "big", signed=True)})
        elif tag == 0x86:
            out.append({"type": "unsigned", "value": int.from_bytes(v, "big")})
        elif tag == 0x87:
            if len(v) == 5 and v[0] == 8:
                out.append({"type": "float32", "value": struct.unpack(">f", v[1:])[0]})
            elif len(v) == 9 and v[0] == 11:
                out.append({"type": "float64", "value": struct.unpack(">d", v[1:])[0]})
            else:
                out.append({"type": "float", "value": v.hex(), "note": "formato non riconosciuto"})
        elif tag == 0x8A:
            out.append({"type": "visible-string", "value": v.decode("ascii", "replace")})
        elif tag == 0x90:
            out.append({"type": "mms-string", "value": v.decode("utf-8", "replace")})
        elif tag == 0x91:
            out.append(decode_utctime(v))
        elif tag == 0x8D:
            out.append({"type": "bcd", "value": int.from_bytes(v, "big")})
        else:
            names = {0x88: "real", 0x89: "octet-string", 0x8C: "binary-time", 0x8F: "objId"}
            out.append({"type": names.get(tag, f"tag-0x{tag:02x}"), "value": v.hex()})
    return out


def flat(items):
    """Rappresentazione testuale compatta di allData (per il CSV)."""
    parts = []
    for it in items:
        t, val = it["type"], it["value"]
        if t in ("struct", "array"):
            parts.append(f"{t}[{flat(val)}]")
        else:
            s = f"{t}={val}"
            if it.get("note"):
                s += f"({it['note']})"
            parts.append(s)
    return " | ".join(parts)


def parse_apdu(pl, f):
    """Riempie il dict f con header GOOSE e goosePdu. Solleva BerError se malformato."""
    if len(pl) < 8:
        raise BerError("header GOOSE troncato")
    f["appid"] = int.from_bytes(pl[0:2], "big")
    length = int.from_bytes(pl[2:4], "big")
    r1, r2 = int.from_bytes(pl[4:6], "big"), int.from_bytes(pl[6:8], "big")
    f["reserved1"], f["reserved2"] = f"0x{r1:04x}", f"0x{r2:04x}"
    end = min(length, len(pl))          # esclude il padding Ethernet
    if length < 8:
        raise BerError("campo Length GOOSE < 8")
    tag, vs, ve = read_tlv(pl, 8, end)
    if tag != 0x61:
        raise BerError(f"atteso tag goosePdu 0x61, trovato 0x{tag:02x}")
    i = vs
    while i < ve:
        tag, a, b = read_tlv(pl, i, ve)
        i = b
        v = pl[a:b]
        name = PDU_FIELDS.get(tag)
        if name in ("gocbRef", "datSet", "goID"):
            f[name] = v.decode("ascii", "replace")
        elif name in ("timeAllowedToLive", "stNum", "sqNum", "confRev", "numDatSetEntries"):
            f[name] = int.from_bytes(v, "big")
        elif name in ("simulation", "ndsCom"):
            f[name] = bool(v[0]) if v else None
        elif name == "t":
            d = decode_utctime(v)
            f["t_utc"], f["t_flags"] = d["value"], d.get("note", "")
        elif tag == 0xAB:
            f["data"] = decode_data(pl, a, b)
            f["n_items"] = len(f["data"])
            f["data_flat"] = flat(f["data"])
        else:
            f.setdefault("extra_tags", []).append(f"0x{tag:02x}")


# ------------------------------ lettura ----------------------------------
def mac_str(b):
    return ":".join(f"{x:02x}" for x in b)


def parse_file(path, appid=None, gocbref=None, dst=None):
    """Legge un pcap/pcapng e ritorna la lista dei frame GOOSE decodificati."""
    try:
        from scapy.utils import PcapReader
        from scapy.layers.l2 import Ether  # noqa: F401  (registra il link type Ethernet)
    except ImportError:
        raise SystemExit("Serve scapy:  pip install scapy")
    dst = dst.lower().replace("-", ":") if dst else None
    frames, n, prev_by_stream = [], 0, {}
    with PcapReader(path) as rd:
        for pkt in rd:
            n += 1
            raw = getattr(pkt, "original", None) or bytes(pkt)
            if len(raw) < 14:
                continue
            et, off, vlans = int.from_bytes(raw[12:14], "big"), 14, []
            while et in VLAN_ETHERTYPES and off + 4 <= len(raw):
                tci = int.from_bytes(raw[off:off + 2], "big")
                vlans.append((tci & 0x0FFF, tci >> 13))
                et = int.from_bytes(raw[off + 2:off + 4], "big")
                off += 4
            if et != GOOSE_ETHERTYPE:
                continue
            f = dict.fromkeys(CSV_FIELDS)
            f.update(frame=n, time=float(pkt.time), time_epoch=f"{float(pkt.time):.6f}",
                     src=mac_str(raw[6:12]), dst=mac_str(raw[0:6]))
            f["time_utc"] = (EPOCH + timedelta(seconds=float(pkt.time))).strftime(
                "%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
            if vlans:
                f["vlan_id"], f["vlan_prio"] = vlans[0]
            try:
                parse_apdu(raw[off:], f)
            except BerError as e:
                f["parse_error"] = str(e)
            if appid is not None and f["appid"] != appid:
                continue
            if gocbref and gocbref not in (f["gocbRef"] or ""):
                continue
            if dst and f["dst"] != dst:
                continue
            f["data_key"] = json.dumps(f.get("data"), sort_keys=True)
            key = (f["src"], f["dst"], f["appid"], f["gocbRef"])
            p = prev_by_stream.get(key)
            f["dt_prev_s"] = f"{f['time'] - p['time']:.6f}" if p else ""
            prev_by_stream[key] = f
            frames.append(f)
    return frames


# ------------------------------ analisi ----------------------------------
def analizza(frames, stimolo_epoch=None):
    """Controlli di coerenza per flusso. Ritorna dict con riassunto, anomalie, eventi."""
    streams = {}
    for f in frames:
        streams.setdefault((f["src"], f["dst"], f["appid"], f["gocbRef"]), []).append(f)
    result = {"flussi": [], "errori": 0, "stimolo_senza_evento": False}
    for (src, dst, appid, gocb), fr in streams.items():
        issues, events, dts = [], [], []
        for prev, f in zip(fr, fr[1:]):
            dt = f["time"] - prev["time"]
            dts.append(dt)
            where = f"frame {f['frame']}"
            if f["parse_error"]:
                issues.append(("ERRORE", where, f"frame malformato: {f['parse_error']}"))
                continue
            if prev["parse_error"]:
                continue
            req = ("stNum", "sqNum", "confRev", "timeAllowedToLive")
            if any(f[k] is None or prev[k] is None for k in req):
                issues.append(("ERRORE", where, "campi obbligatori mancanti nel goosePdu"))
                continue
            if prev["timeAllowedToLive"] and dt > prev["timeAllowedToLive"] / 1000:
                issues.append(("ERRORE", where,
                               f"intervallo {dt:.3f}s > timeAllowedToLive {prev['timeAllowedToLive']} ms"))
            if f["confRev"] != prev["confRev"]:
                issues.append(("ERRORE", where, f"confRev cambiata {prev['confRev']} -> {f['confRev']}"))
            if f["stNum"] == prev["stNum"]:
                ok_sq = f["sqNum"] == prev["sqNum"] + 1 or (
                    prev["sqNum"] == U32_MAX and f["sqNum"] in (0, 1))
                if not ok_sq:
                    issues.append(("ERRORE", where,
                                   f"sqNum {prev['sqNum']} -> {f['sqNum']} (atteso +1)"))
                if f["data_key"] != prev["data_key"]:
                    issues.append(("ERRORE", where, "dati cambiati senza incremento di stNum"))
            else:
                exp = prev["stNum"] + 1 if prev["stNum"] < U32_MAX else 1
                if f["stNum"] != exp:
                    issues.append(("ERRORE", where, f"stNum {prev['stNum']} -> {f['stNum']} (atteso {exp})"))
                if f["sqNum"] != 0:
                    issues.append(("AVVISO", where, f"sqNum={f['sqNum']} dopo cambio stNum (atteso 0: da verificare sul dispositivo)"))
                if f["data_key"] == prev["data_key"]:
                    issues.append(("AVVISO", where, "stNum incrementato ma dati identici"))
                events.append(f)
        # burst di ritrasmissioni dopo ogni cambio di stato
        eventi = []
        for ev in events:
            idx = fr.index(ev)
            burst = [fr[k]["time"] - fr[k - 1]["time"]
                     for k in range(idx + 1, min(idx + 9, len(fr)))
                     if fr[k]["stNum"] == ev["stNum"]]
            eventi.append({"frame": ev["frame"], "stNum": ev["stNum"], "time": ev["time"],
                           "ritrasmissioni_s": [round(x, 4) for x in burst]})
        res = {
            "src": src, "dst": dst, "appid": appid, "gocbRef": gocb,
            "vlan": (fr[0]["vlan_id"], fr[0]["vlan_prio"]),
            "n_frame": len(fr), "durata_s": round(fr[-1]["time"] - fr[0]["time"], 3),
            "stNum": (fr[0]["stNum"], fr[-1]["stNum"]),
            "sqNum": (fr[0]["sqNum"], fr[-1]["sqNum"]),
            "ttl_ms": sorted({f["timeAllowedToLive"] for f in fr if f["timeAllowedToLive"]}),
            "intervallo_s": (round(min(dts), 3), round(statistics.median(dts), 3), round(max(dts), 3)) if dts else None,
            "anomalie": issues, "eventi": eventi,
        }
        res["errori"] = sum(1 for s, _, _ in issues if s == "ERRORE")
        result["errori"] += res["errori"]
        if stimolo_epoch is not None:
            after = [e for e in events if e["time"] >= stimolo_epoch]
            if after:
                res["latenza_stimolo_s"] = round(after[0]["time"] - stimolo_epoch, 6)
                res["latenza_frame"] = after[0]["frame"]
            else:
                res["latenza_stimolo_s"] = None
                result["stimolo_senza_evento"] = True
        result["flussi"].append(res)
    return result


def stampa(result, stimolo=False):
    for r in result["flussi"]:
        print(f"\nFlusso {r['src']} -> {r['dst']}  APPID 0x{r['appid']:04x}  VLAN {r['vlan'][0]} prio {r['vlan'][1]}")
        print(f"  gocbRef: {r['gocbRef']}")
        print(f"  frame: {r['n_frame']} in {r['durata_s']} s   stNum {r['stNum'][0]}->{r['stNum'][1]}   "
              f"sqNum {r['sqNum'][0]}->{r['sqNum'][1]}   TTL {r['ttl_ms']} ms")
        if r["intervallo_s"]:
            mn, md, mx = r["intervallo_s"]
            print(f"  intervallo tra frame (min/mediana/max): {mn} / {md} / {mx} s")
        for e in r["eventi"]:
            print(f"  cambio di stato: stNum={e['stNum']} al frame {e['frame']}, "
                  f"ritrasmissioni (s): {e['ritrasmissioni_s']}")
        if stimolo:
            lat = r.get("latenza_stimolo_s")
            print("  latenza stimolo -> primo frame con nuovo stNum: "
                  + (f"{lat*1000:.1f} ms (frame {r['latenza_frame']})" if lat is not None
                     else "NESSUN cambio di stNum dopo lo stimolo"))
        if r["anomalie"]:
            print(f"  anomalie ({len(r['anomalie'])}):")
            for sev, where, msg in r["anomalie"][:20]:
                print(f"    [{sev}] {where}: {msg}")
            if len(r["anomalie"]) > 20:
                print(f"    ... altre {len(r['anomalie']) - 20}")
        else:
            print("  anomalie: nessuna")


# -------------------------------- main -----------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("-o", "--csv", help="scrive un CSV (un frame per riga)")
    ap.add_argument("--json", help="scrive un JSON con anche i dati annidati")
    ap.add_argument("--analizza", action="store_true", help="controlli di coerenza")
    ap.add_argument("--stimolo-epoch", type=float,
                    help="istante dello stimolo (epoch, stessa macchina della cattura)")
    ap.add_argument("--appid", type=lambda x: int(x, 0))
    ap.add_argument("--gocbref")
    ap.add_argument("--dst")
    a = ap.parse_args()

    frames = parse_file(a.pcap, a.appid, a.gocbref, a.dst)
    if not frames:
        print("Nessun frame GOOSE trovato.", file=sys.stderr)
        return 3
    bad = sum(1 for f in frames if f["parse_error"])
    print(f"Frame GOOSE: {len(frames)}" + (f"  (di cui malformati: {bad})" if bad else ""))

    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8-sig") as fh:
            w = csv.DictWriter(fh, fieldnames=CSV_FIELDS, delimiter=";", extrasaction="ignore")
            w.writeheader()
            w.writerows(frames)
        print(f"CSV: {a.csv}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump([{k: v for k, v in f.items() if k not in ("data_key", "time")}
                       for f in frames], fh, indent=2, ensure_ascii=False)
        print(f"JSON: {a.json}")

    res = analizza(frames, a.stimolo_epoch)
    stampa(res, a.stimolo_epoch is not None)
    if a.stimolo_epoch is not None and res["stimolo_senza_evento"]:
        return 2
    if a.analizza and res["errori"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
