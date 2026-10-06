#!/usr/bin/env python3
"""
scl_extract.py - estrae da un file SCL (.icd / .cid / .scd) la tabella
"scenario -> riferimenti" per la validazione dei flussi IEC 61850 e, a
richiesta, la configurazione pronta per bin/parser (MMS + GOOSE).

Uso:
  python3 scl_extract.py BU9020.icd                 # tutti gli IED
  python3 scl_extract.py BU9020.icd --ied NOME_IED  # un solo IED
  python3 scl_extract.py BU9020.icd --tutto         # anche DO non classificati
  python3 scl_extract.py BU9020.icd --tutti-fc      # anche attributi CF/DC/EX
  python3 scl_extract.py BU9020.icd --genera-config config
                                     # scrive config/parser/<ied>.json
                                     # (configurazione per bin/parser)
  python3 scl_extract.py BU9020.icd --scenari mio_scenari.json
                                     # classificazione scenari personalizzata

Output (cartella --out, default ./out):
  tabella_scenari.csv / .md   una riga per (DO, dataset, GSEControl)
  goose_inventario.csv        tutti i GSEControl con MAC/APPID/VLAN/timing
  report_inventario.csv       tutti i ReportControl, con le istanze da usare nel client
  modello.json                tutto il modello estratto (per i tuoi script)

Solo libreria standard Python 3.8+.
"""
import argparse
import csv
import json
import os
import re
import xml.etree.ElementTree as ET

# --------------------------------------------------------------------------
# CONFIGURAZIONE SCENARI - valori di default, usati se non c'e' il file
# config/scl/scenari.json (o quello passato con --scenari).
# Un DO finisce in uno scenario se la sua lnClass e' nella lista OPPURE una
# keyword compare in (LD, LN, desc, DO, desc DO, dataset).
# --------------------------------------------------------------------------
DEFAULT_SCENARIOS = {
    "sincronismo": {
        "ln_classes": ["RSYN", "CSYN"],
        "keywords": ["sync", "sincr", "synchk", "syncchk"],
    },
    "richiusura": {
        "ln_classes": ["RREC", "XCBR", "CSWI", "XSWI"],
        "keywords": ["reclos", "richius", "autorec"],
    },
    "regolazione_tensione": {
        "ln_classes": ["ATCC", "YLTC", "PTUV", "PTOV"],
        "keywords": ["tapchg", "tappos", "regol", "undervolt", "overvolt",
                     "sottotens", "sovratens"],
    },
    "io_digitali": {
        "ln_classes": ["GGIO"],
        "keywords": ["digital", "ingress", "uscit"],
    },
}
DEFAULT_SCENARI_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "config", "scl", "scenari.json")

# Functional Constraint "utili" per la validazione (stato, misure, comandi,
# setpoint). Con --tutti-fc si includono anche CF, DC, EX, ...
KEY_FCS = {"ST", "MX", "CO", "SP", "SV", "SG", "SE"}
FCDA_KEYS = ("ldInst", "prefix", "lnClass", "lnInst", "doName", "daName", "fc")

# Generazione config MMS: FC letti e attributi esclusi (qualita', timestamp e
# attributi di servizio dei comandi, che raddoppierebbero le letture)
MMS_CONFIG_FCS = {"ST", "MX"}
MMS_CONFIG_SKIP = {"q", "t", "stSeld", "opRcvd", "opOk", "tOpOk", "ctlNum"}
MMS_CONFIG_DC_DOS = {"NamPlt", "PhyNam"}   # targhe dati: lette anche in DC

# Avvisi raccolti durante l'analisi (set: ogni avviso compare una volta sola)
WARNINGS = set()


# ------------------------------- utilita' ---------------------------------
def strip_ns(root):
    """Rimuove i namespace XML, cosi' si puo' cercare 'IED' e non '{ns}IED'."""
    for el in root.iter():
        if isinstance(el.tag, str) and "}" in el.tag:
            el.tag = el.tag.split("}", 1)[1]
    return root


def txt(el):
    return (el.text or "").strip() if el is not None else ""


def vals(el):
    """Contenuto dei <Val> di un elemento (piu' Val = setting group)."""
    v = [txt(x) for x in el.findall("Val")] if el is not None else []
    return v[0] if len(v) == 1 else ",".join(v)


def timing(el):
    """MinTime/MaxTime: valore + multiplier + unit (es. '4 ms')."""
    if el is None:
        return ""
    return f'{txt(el)} {el.get("multiplier", "")}{el.get("unit", "s")}'


MULTIPLIERS = {"": 1.0, "m": 1e-3, "u": 1e-6, "n": 1e-9, "k": 1e3}


def timing_ms(el):
    """MinTime/MaxTime convertito in millisecondi (None se assente/non valido)."""
    if el is None:
        return None
    try:
        return float(txt(el)) * MULTIPLIERS.get(el.get("multiplier", ""), 1.0) * 1000.0
    except ValueError:
        return None


def load_scenarios(path):
    """Classificazione scenari da file JSON; default interni se il file manca."""
    if path and os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        src = path
    else:
        data, src = DEFAULT_SCENARIOS, "default interni"
    scen = {k: {"ln_classes": set(v.get("ln_classes", [])),
                "keywords": [w.lower() for w in v.get("keywords", [])]}
            for k, v in data.items() if not k.startswith("_")}
    return scen, src


# ------------------------- DataTypeTemplates ------------------------------
def parse_templates(root):
    t = root.find("DataTypeTemplates")
    lnt, dot, dat = {}, {}, {}
    if t is None:
        WARNINGS.add("il file non contiene <DataTypeTemplates>")
        return lnt, dot, dat
    for n in t.findall("LNodeType"):
        lnt[n.get("id")] = {
            "lnClass": n.get("lnClass"),
            "dos": [{"name": d.get("name"), "type": d.get("type"),
                     "desc": d.get("desc", "")} for d in n.findall("DO")],
        }
    for d in t.findall("DOType"):
        dot[d.get("id")] = {
            "cdc": d.get("cdc"),
            "das": [{"name": a.get("name"), "fc": a.get("fc"),
                     "bType": a.get("bType"), "type": a.get("type"),
                     "count": a.get("count"), "val": vals(a),
                     "trg": ",".join(k for k in ("dchg", "qchg", "dupd")
                                     if a.get(k) == "true")}
                    for a in d.findall("DA")],
            "sdos": [{"name": s.get("name"), "type": s.get("type"),
                      "count": s.get("count")}
                     for s in d.findall("SDO")],
        }
    for d in t.findall("DAType"):
        dat[d.get("id")] = [{"name": b.get("name"), "bType": b.get("bType"),
                             "type": b.get("type"), "count": b.get("count"),
                             "val": vals(b)} for b in d.findall("BDA")]
    return lnt, dot, dat


def is_array(count):
    return bool(count) and count != "0"


def expand_da(path, bType, typ, fc, trg, dat, count=None, default="", depth=0):
    """Espande ricorsivamente i DA strutturati fino alle foglie.
    Un array (attributo count) diventa una sola foglia "nome[count]", marcata
    con array=count: non si legge come valore singolo."""
    if is_array(count):
        return [{"da": ".".join(path) + f"[{count}]", "fc": fc, "bType": bType,
                 "trg": trg, "array": count, "default": ""}]
    if bType == "Struct":
        if typ not in dat:
            WARNINGS.add(f"DAType '{typ}' non definito (usato da {'.'.join(path)})")
        elif depth < 8:
            out = []
            for b in dat[typ]:
                out += expand_da(path + [b["name"]], b["bType"], b["type"],
                                 fc, trg, dat, b["count"], b["val"], depth + 1)
            return out
    return [{"da": ".".join(path), "fc": fc, "bType": bType, "trg": trg,
             "array": None, "default": default}]


def do_attributes(type_id, dot, dat, prefix=(), depth=0):
    """Tutte le foglie (DA) di un DO, con FC, tipo, trigger (dchg/qchg/dupd)
    e valore di default definito nei template."""
    d = dot.get(type_id)
    if not d:
        WARNINGS.add(f"DOType '{type_id}' non definito")
        return []
    if depth > 6:
        return []
    out = []
    for a in d["das"]:
        out += expand_da(list(prefix) + [a["name"]], a["bType"], a["type"],
                         a["fc"], a["trg"], dat, a["count"], a["val"])
    for s in d["sdos"]:
        if is_array(s["count"]):
            out.append({"da": ".".join(list(prefix) + [s["name"]]) + f'[{s["count"]}]',
                        "fc": "", "bType": "SDO", "trg": "", "array": s["count"],
                        "default": ""})
            continue
        out += do_attributes(s["type"], dot, dat,
                             tuple(prefix) + (s["name"],), depth + 1)
    return out


def instance_values(doi, prefix=()):
    """Valori (<Val>) assegnati nell'istanza: DOI -> SDI -> DAI, a ogni livello."""
    out = {}
    for el in doi:
        name = el.get("name", "")
        if el.get("ix"):                       # elemento di array
            name += f'({el.get("ix")})'
        if el.tag == "SDI":
            out.update(instance_values(el, prefix + (name,)))
        elif el.tag == "DAI":
            v = vals(el)
            if v != "":
                out[".".join(prefix + (name,))] = v
    return out


# ----------------------------- Communication ------------------------------
def parse_comm(root):
    ap_map, gse_map = {}, {}
    c = root.find("Communication")
    if c is None:
        return ap_map, gse_map
    for sn in c.findall("SubNetwork"):
        for cap in sn.findall("ConnectedAP"):
            ied, apn = cap.get("iedName"), cap.get("apName")
            ap_map[(ied, apn)] = {
                "subnetwork": sn.get("name"), "sn_type": sn.get("type"),
                "addr": {p.get("type"): txt(p) for p in cap.findall("Address/P")},
            }
            for g in cap.findall("GSE"):
                a = {p.get("type"): txt(p) for p in g.findall("Address/P")}
                gse_map[(ied, g.get("ldInst"), g.get("cbName"))] = {
                    "ap": apn,
                    "mac": a.get("MAC-Address", ""),
                    "appid": a.get("APPID", ""),
                    "vlan_id": a.get("VLAN-ID", ""),
                    "vlan_prio": a.get("VLAN-PRIORITY", ""),
                    "min_time": timing(g.find("MinTime")),
                    "max_time": timing(g.find("MaxTime")),
                    "min_time_ms": timing_ms(g.find("MinTime")),
                    "max_time_ms": timing_ms(g.find("MaxTime")),
                }
    return ap_map, gse_map


# --------------------------------- IED ------------------------------------
def rcb_instances(r, ld_full, ln_name):
    """Istanze reali di un ReportControl, nel formato del client MMS.
    Se indexed (default "true") e RptEnabled max=N, l'IED espone le istanze
    <nome>01 ... <nome>NN; con indexed="false" una sola istanza <nome>."""
    en = r.find("RptEnabled")
    try:
        n = int(en.get("max", "1")) if en is not None else 1
    except ValueError:
        n = 1
    name = r.get("name")
    names = ([f"{name}{i:02d}" for i in range(1, n + 1)]
             if r.get("indexed", "true") != "false" else [name])
    return [f"{ld_full}/{ln_name}.{x}" for x in names]


def parse_ieds(root, tmpl, comm, only=None):
    lnt, dot, dat = tmpl
    ap_map, gse_map = comm
    M = {"ieds": [], "lns": [], "datasets": [], "gse": [], "rcb": [],
         "access_points": {f"{k[0]}/{k[1]}": v for k, v in ap_map.items()}}
    for ied in root.findall("IED"):
        iname = ied.get("name")
        if only and iname != only:
            continue
        M["ieds"].append({"name": iname, "manufacturer": ied.get("manufacturer", ""),
                          "type": ied.get("type", ""),
                          "configVersion": ied.get("configVersion", ""),
                          "desc": ied.get("desc", "")})
        for ap in ied.findall("AccessPoint"):
            for ld in ap.findall("Server/LDevice"):
                ld_inst = ld.get("inst")
                # ldName (ed.2) se presente, altrimenti IEDname + inst
                ld_full = ld.get("ldName") or (iname + ld_inst)
                for el in ld.findall("LN0") + ld.findall("LN"):
                    cls = el.get("lnClass")
                    inst = el.get("inst") or ""
                    pre = el.get("prefix") or ""
                    name = pre + cls + inst
                    ln = {"ied": iname, "ap": ap.get("name"), "ld_inst": ld_inst,
                          "ld": ld_full, "name": name, "lnClass": cls,
                          "prefix": pre, "inst": inst, "lnType": el.get("lnType"),
                          "desc": el.get("desc", ""), "dos": []}
                    if ln["lnType"] not in lnt:
                        WARNINGS.add(f"LNodeType '{ln['lnType']}' non definito: "
                                     f"{ld_full}/{name} senza DO")
                    doi = {d.get("name"): d for d in el.findall("DOI")}
                    for d in lnt.get(ln["lnType"], {}).get("dos", []):
                        dd = doi.get(d["name"])
                        # Valori: default dei template, sovrascritti da quelli
                        # assegnati nell'istanza (DOI/SDI/DAI)
                        values = {a["da"]: a["default"]
                                  for a in do_attributes(d["type"], dot, dat)
                                  if a["default"] != ""}
                        if dd is not None:
                            values.update(instance_values(dd))
                        desc = (dd.get("desc") if dd is not None else "") or d["desc"]
                        ln["dos"].append({
                            "name": d["name"], "type": d["type"],
                            "cdc": dot.get(d["type"], {}).get("cdc", ""),
                            "desc": desc or "",
                            "ctlModel": values.get("ctlModel", ""),
                            "values": values,
                        })
                    M["lns"].append(ln)

                    for ds in el.findall("DataSet"):
                        M["datasets"].append({
                            "ied": iname, "ld_inst": ld_inst, "ld": ld_full,
                            "ln": name, "name": ds.get("name"),
                            "desc": ds.get("desc", ""),
                            "ref": f'{ld_full}/{name}${ds.get("name")}',
                            "fcdas": [{k: f.get(k) or "" for k in FCDA_KEYS}
                                      for f in ds.findall("FCDA")],
                        })
                    for g in el.findall("GSEControl"):
                        gname = g.get("name")
                        ds_name = g.get("datSet", "")
                        M["gse"].append({
                            "ied": iname, "ld_inst": ld_inst, "ld": ld_full,
                            "ln": name, "name": gname, "desc": g.get("desc", ""),
                            "type": g.get("type", "GOOSE"),
                            "goID": g.get("appID", ""),
                            "datSet": ds_name,
                            "datSet_ref": f"{ld_full}/{name}${ds_name}" if ds_name else "",
                            "confRev": g.get("confRev", ""),
                            "fixedOffs": g.get("fixedOffs", ""),
                            "securityEnabled": g.get("securityEnabled", ""),
                            "gocbRef": f"{ld_full}/{name}$GO${gname}",
                            "subscribers": [txt(i) for i in g.findall("IEDName")],
                            "comm": gse_map.get((iname, ld_inst, gname), {}),
                        })
                    for r in el.findall("ReportControl"):
                        trg, opt, en = r.find("TrgOps"), r.find("OptFields"), r.find("RptEnabled")
                        buffered = r.get("buffered", "false")
                        fc = "BR" if buffered == "true" else "RP"
                        M["rcb"].append({
                            "ied": iname, "ld_inst": ld_inst, "ld": ld_full,
                            "ln": name, "name": r.get("name"),
                            "rptID": r.get("rptID", ""), "datSet": r.get("datSet", ""),
                            "confRev": r.get("confRev", ""), "buffered": buffered,
                            "bufTime": r.get("bufTime", ""), "intgPd": r.get("intgPd", ""),
                            "indexed": r.get("indexed", "true"),
                            "trgops": ",".join(k for k in ("dchg", "qchg", "dupd", "period", "gi")
                                               if trg is not None and trg.get(k) == "true"),
                            "optfields": ",".join(k for k, v in opt.attrib.items() if v == "true")
                                         if opt is not None else "",
                            "rpt_max": en.get("max", "") if en is not None else "",
                            "clients": [c.get("iedName", "") for c in en.findall("ClientLN")]
                                       if en is not None else [],
                            # riferimento MMS del blocco (come compare nei frame)
                            "ref": f'{ld_full}/{name}${fc}${r.get("name")}',
                            # istanze da usare nel client (bin/parser), con FC
                            "fc": fc,
                            "istanze": rcb_instances(r, ld_full, name),
                        })
    return M


# ------------------------------ tabella scenari ---------------------------
def fcda_matches(f, ds, ln, do):
    return (ds["ied"] == ln["ied"]
            and (f["ldInst"] or ds["ld_inst"]) == ln["ld_inst"]
            and f["prefix"] == ln["prefix"]
            and f["lnClass"] == ln["lnClass"]
            and f["lnInst"] == ln["inst"]
            and f["doName"].split(".")[0] == do["name"])


def classify(ln, do, scenarios, ds_name=""):
    text = " ".join([ln["ld"], ln["name"], ln["desc"], do["name"],
                     do["desc"], ds_name]).lower()
    out = [s for s, c in scenarios.items()
           if ln["lnClass"] in c["ln_classes"] or any(k in text for k in c["keywords"])]
    return out or ["-"]


def tshark_filter(g):
    parts = ["goose"]
    mac = g["comm"].get("mac", "")
    if mac:
        parts.append("eth.dst == " + mac.replace("-", ":").lower())
    parts.append(f'goose.gocbRef == "{g["gocbRef"]}"')
    return " && ".join(parts)


ROW_FIELDS = [
    "scenario", "ied", "ld", "ln", "lnClass", "ln_desc", "do", "cdc", "do_desc",
    "ctlModel", "rif_mms_do", "attributi_mms", "valori_iniziali",
    "dataset_ref", "fcda_nel_dataset",
    "gse_name", "gocbRef", "goID", "confRev", "mac", "appid", "vlan_id",
    "vlan_prio", "min_time", "max_time", "tshark_filter", "report_control",
]


def build_rows(M, tmpl, scenarios, all_fc=False):
    _, dot, dat = tmpl
    rows = []
    for ln in M["lns"]:
        for do in ln["dos"]:
            attrs = [a for a in do_attributes(do["type"], dot, dat)
                     if all_fc or a["fc"] in KEY_FCS or a["array"]]
            attr_refs = " ; ".join(
                f'{ln["ld"]}/{ln["name"]}.{do["name"]}.{a["da"]}[{a["fc"]}'
                f'{"," + a["trg"] if a["trg"] else ""}'
                f'{",array" if a["array"] else ""}]' for a in attrs)
            base = {
                "ied": ln["ied"], "ld": ln["ld"], "ln": ln["name"],
                "lnClass": ln["lnClass"], "ln_desc": ln["desc"], "do": do["name"],
                "cdc": do["cdc"], "do_desc": do["desc"], "ctlModel": do["ctlModel"],
                "rif_mms_do": f'{ln["ld"]}/{ln["name"]}.{do["name"]}',
                "attributi_mms": attr_refs,
                "valori_iniziali": " ; ".join(f"{k}={v}" for k, v in do["values"].items()),
            }
            by_ds = {}
            for ds in M["datasets"]:
                for f in ds["fcdas"]:
                    if fcda_matches(f, ds, ln, do):
                        by_ds.setdefault((ds["ld_inst"], ds["name"]), (ds, []))[1].append(
                            f'{f["doName"]}{"." + f["daName"] if f["daName"] else ""}[{f["fc"]}]')
            groups = list(by_ds.values()) or [(None, [])]
            for ds, fcdas in groups:
                scen = classify(ln, do, scenarios, ds["name"] if ds else "")
                row = dict.fromkeys(ROW_FIELDS, "")
                row.update(base)
                row["scenario"] = ",".join(scen)
                gses, rcbs = [], []
                if ds:
                    row["dataset_ref"] = ds["ref"]
                    row["fcda_nel_dataset"] = " ; ".join(fcdas)
                    gses = [g for g in M["gse"] if g["ied"] == ds["ied"]
                            and g["ld_inst"] == ds["ld_inst"] and g["datSet"] == ds["name"]]
                    rcbs = [r for r in M["rcb"] if r["ied"] == ds["ied"]
                            and r["ld_inst"] == ds["ld_inst"] and r["datSet"] == ds["name"]]
                    row["report_control"] = " ; ".join(
                        f'{r["istanze"][0]}[{r["fc"]}] ({len(r["istanze"])} istanze) '
                        f'rptID={r["rptID"]} '
                        f'{"buffered" if r["buffered"] == "true" else "unbuffered"} '
                        f'trg={r["trgops"]} intgPd={r["intgPd"]}' for r in rcbs if r["istanze"])
                for g in gses or [None]:
                    r2 = dict(row)
                    if g:
                        c = g["comm"]
                        r2.update(gse_name=g["name"], gocbRef=g["gocbRef"],
                                  goID=g["goID"], confRev=g["confRev"],
                                  mac=c.get("mac", ""), appid=c.get("appid", ""),
                                  vlan_id=c.get("vlan_id", ""),
                                  vlan_prio=c.get("vlan_prio", ""),
                                  min_time=c.get("min_time", ""),
                                  max_time=c.get("max_time", ""),
                                  tshark_filter=tshark_filter(g))
                    rows.append(r2)
    return rows


# -------------------------- generazione config ----------------------------
def config_key(*parts):
    """Chiave JSON leggibile: LBMULPHD1.PhyHealth.stVal -> lbmulphd1_phyhealth_stval"""
    return re.sub(r"[^A-Za-z0-9]+", "_", "_".join(parts)).strip("_").lower()


def goose_entry(g):
    """GoCB pubblicato dall'IED: gocbRef, chiave e APPID (da <Communication>)."""
    c = {"ref": g["gocbRef"], "key": config_key("goose", g["name"])}
    if g["comm"].get("appid"):
        c["appid"] = g["comm"]["appid"]          # esadecimale
    return c


def ied_ip(M, ied):
    """Indirizzo IP dell'IED da <Communication> (primo access point con IP)."""
    for k, ap in M["access_points"].items():
        if k.split("/")[0] == ied and ap["addr"].get("IP"):
            return ap["addr"]["IP"]
    return ""


def parser_config(M, tmpl, ied):
    """Configurazione per bin/parser: valori ST/MX di tutti i DO dell'IED
    (senza q/t e attributi di servizio dei comandi), targhe dati in DC,
    prima istanza di ogni Report Control Block, GoCB pubblicati."""
    _, dot, dat = tmpl
    points, skipped_arrays = [], 0
    for ln in M["lns"]:
        if ln["ied"] != ied:
            continue
        for do in ln["dos"]:
            for a in do_attributes(do["type"], dot, dat):
                if a["array"]:
                    skipped_arrays += 1
                    continue
                leaf = a["da"].split(".")
                wanted = ((a["fc"] in MMS_CONFIG_FCS and leaf[-1] not in MMS_CONFIG_SKIP
                           and leaf[0] != "origin")
                          or (a["fc"] == "DC" and do["name"] in MMS_CONFIG_DC_DOS))
                if wanted:
                    points.append({
                        "ref": f'{ln["ld"]}/{ln["name"]}.{do["name"]}.{a["da"]}',
                        "key": config_key(ln["name"], do["name"], a["da"]),
                        "fc": a["fc"],
                    })
    rbs = [{"ref": r["istanze"][0], "key": config_key("rcb", r["name"]), "fc": r["fc"]}
           for r in M["rcb"] if r["ied"] == ied and r["istanze"]]
    gooses = [goose_entry(g) for g in M["gse"] if g["ied"] == ied]
    return points, rbs, gooses, skipped_arrays


def write_config(path, text, overwrite):
    """Scrive un file di configurazione senza sovrascrivere quelli esistenti
    (salvo --sovrascrivi). Ritorna True se il file e' stato scritto."""
    if os.path.exists(path) and not overwrite:
        print(f"  esiste gia', non sovrascritto: {path}")
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  scritto: {path}")
    return True


def parser_config_text(run, points, rbs, gooses, source):
    """JSON con un elemento per riga, nello stesso stile dei config scritti a mano."""
    line = lambda d: "    " + json.dumps(d, ensure_ascii=False)
    nota = ("generato da scl_extract.py da " + source +
            ". Compilare \"interface\" con l'interfaccia di rete collegata all'IED.")
    return ("{\n"
            f'  "_origine": {json.dumps(nota, ensure_ascii=False)},\n'
            + "".join(f'  {json.dumps(k)}: {json.dumps(v)},\n' for k, v in run.items()) +
            '  "points": [\n' + ",\n".join(line(p) for p in points) + "\n  ],\n"
            '  "report_blocks": [\n' + ",\n".join(line(r) for r in rbs) + "\n  ],\n"
            '  "goose": [\n' + ",\n".join(line(g) for g in gooses) + "\n  ]\n"
            "}\n")


def generate_configs(M, tmpl, outdir, source, overwrite):
    """Un file per IED: DIR/parser/<ied>.json"""
    src = os.path.basename(source)
    for ied in (i["name"] for i in M["ieds"]):
        points, rbs, gooses, skipped = parser_config(M, tmpl, ied)
        ip = ied_ip(M, ied)
        run = {"host": ip, "port": 102, "interface": "", "goose_time_s": 5,
               "output": f"data/results/{ied}.json"}
        write_config(os.path.join(outdir, "parser", f"{ied}.json"),
                     parser_config_text(run, points, rbs, gooses, src), overwrite)
        print(f"    host {ip or '(IP non presente nel file: compilare)'}, "
              f"{len(points)} punti, {len(rbs)} report block, {len(gooses)} GoCB GOOSE"
              + (f", {skipped} attributi array esclusi" if skipped else ""))


# -------------------------------- output ----------------------------------
def write_csv(path, rows, fields):
    # utf-8-sig + ';' : si apre direttamente in Excel italiano
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields, delimiter=";", extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def write_md(path, rows):
    cols = ["scenario", "ln", "do", "cdc", "dataset_ref", "gocbRef", "mac",
            "appid", "confRev", "rif_mms_do"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("| " + " | ".join(cols) + " |\n")
        f.write("|" + "---|" * len(cols) + "\n")
        for r in rows:
            f.write("| " + " | ".join(str(r[c]).replace("|", "/") for c in cols) + " |\n")


def goose_rows(M):
    out = []
    for g in M["gse"]:
        c = g["comm"]
        n_fcda = sum(len(d["fcdas"]) for d in M["datasets"]
                     if d["ied"] == g["ied"] and d["ld_inst"] == g["ld_inst"]
                     and d["name"] == g["datSet"])
        out.append({
            "ied": g["ied"], "ld": g["ld"], "gse_name": g["name"],
            "gocbRef": g["gocbRef"], "datSet_ref": g["datSet_ref"], "goID": g["goID"],
            "confRev": g["confRev"], "n_membri_dataset": n_fcda,
            "mac": c.get("mac", ""), "appid": c.get("appid", ""),
            "vlan_id": c.get("vlan_id", ""), "vlan_prio": c.get("vlan_prio", ""),
            "min_time": c.get("min_time", ""), "max_time": c.get("max_time", ""),
            "fixedOffs": g["fixedOffs"], "securityEnabled": g["securityEnabled"],
            "subscribers": ",".join(g["subscribers"]),
            "tshark_filter": tshark_filter(g),
        })
    return out


def rcb_rows(M):
    return [dict(r, clients=",".join(r["clients"]),
                 istanze=" ; ".join(r["istanze"]),
                 rif_client=r["istanze"][0] if r["istanze"] else "")
            for r in M["rcb"]]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scl", help="file .icd / .cid / .scd")
    ap.add_argument("--ied", help="nome IED da estrarre (default: tutti)")
    ap.add_argument("-o", "--out", default="out")
    ap.add_argument("--tutto", action="store_true",
                    help="includi anche i DO non assegnati a uno scenario")
    ap.add_argument("--tutti-fc", action="store_true",
                    help="includi attributi con FC CF/DC/EX/...")
    ap.add_argument("--scenari", default=DEFAULT_SCENARI_FILE,
                    help="file JSON con la classificazione degli scenari "
                         "(default: config/scl/scenari.json)")
    ap.add_argument("--genera-config", metavar="DIR",
                    help="scrive DIR/parser/<ied>.json (configurazione per bin/parser)")
    ap.add_argument("--sovrascrivi", action="store_true",
                    help="con --genera-config sovrascrive i file esistenti")
    a = ap.parse_args()

    scenarios, scen_src = load_scenarios(a.scenari)
    root = strip_ns(ET.parse(a.scl).getroot())
    tmpl = parse_templates(root)
    comm = parse_comm(root)
    M = parse_ieds(root, tmpl, comm, a.ied)
    if not M["ieds"]:
        raise SystemExit("Nessun IED trovato (controlla --ied e il file).")

    all_rows = build_rows(M, tmpl, scenarios, a.tutti_fc)
    rows = all_rows if a.tutto else [r for r in all_rows if r["scenario"] != "-"]
    rows.sort(key=lambda r: (r["scenario"], r["ln"], r["do"], r["gse_name"]))

    os.makedirs(a.out, exist_ok=True)
    write_csv(os.path.join(a.out, "tabella_scenari.csv"), rows, ROW_FIELDS)
    write_md(os.path.join(a.out, "tabella_scenari.md"), rows)
    gr = goose_rows(M)
    if gr:
        write_csv(os.path.join(a.out, "goose_inventario.csv"), gr, list(gr[0].keys()))
    if M["rcb"]:
        rr = rcb_rows(M)
        write_csv(os.path.join(a.out, "report_inventario.csv"), rr, list(rr[0].keys()))
    with open(os.path.join(a.out, "modello.json"), "w", encoding="utf-8") as f:
        json.dump(M, f, indent=2, ensure_ascii=False)

    print(f"IED: {', '.join(i['name'] for i in M['ieds'])}")
    print(f"LN: {len(M['lns'])}  DataSet: {len(M['datasets'])}  "
          f"GSEControl: {len(M['gse'])}  ReportControl: {len(M['rcb'])}")
    print(f"Scenari: {', '.join(scenarios)} ({scen_src})")
    print(f"Righe in tabella_scenari: {len(rows)}  -> {a.out}/")
    if not rows and all_rows:
        print(f"ATTENZIONE: nessun DO classificato negli scenari ({len(all_rows)} righe "
              f"scartate). Usa --tutto per averle tutte, oppure aggiungi lnClass/"
              f"keyword dell'IED nel file scenari.")
    for g in M["gse"]:
        if not g["comm"]:
            print(f"ATTENZIONE: GSEControl {g['gocbRef']} senza MAC/APPID in "
                  f"<Communication> (ICD generico? usa il CID).")
    for w in sorted(WARNINGS):
        print(f"ATTENZIONE: {w}")

    if a.genera_config:
        print(f"Generazione config in {a.genera_config}/")
        generate_configs(M, tmpl, a.genera_config, a.scl, a.sovrascrivi)


if __name__ == "__main__":
    main()
