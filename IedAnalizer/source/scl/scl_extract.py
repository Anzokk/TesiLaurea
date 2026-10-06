#!/usr/bin/env python3
"""
scl_extract.py - estrae da un file SCL (.icd / .cid / .scd) la tabella
"scenario -> riferimenti" per la validazione dei flussi IEC 61850.

Uso:
  python3 scl_extract.py BU9020.cid                 # tutti gli IED
  python3 scl_extract.py BU9020.cid --ied NOME_IED  # un solo IED
  python3 scl_extract.py BU9020.cid --tutto         # anche DO non classificati
  python3 scl_extract.py BU9020.cid --tutti-fc      # anche attributi CF/DC/EX

Output (cartella --out, default ./out):
  tabella_scenari.csv / .md   una riga per (DO, dataset, GSEControl)
  goose_inventario.csv        tutti i GSEControl con MAC/APPID/VLAN/timing
  report_inventario.csv       tutti i ReportControl
  modello.json                tutto il modello estratto (per i tuoi script)

Solo libreria standard Python 3.8+.
"""
import argparse
import csv
import json
import os
import xml.etree.ElementTree as ET

# --------------------------------------------------------------------------
# CONFIGURAZIONE SCENARI - se la BU9020 usa LN custom o nomi diversi,
# modifica qui. Un DO finisce in uno scenario se la sua lnClass e' nella
# lista OPPURE una keyword compare in (LD, LN, desc, DO, desc DO, dataset).
# --------------------------------------------------------------------------
SCENARIOS = {
    "sincronismo": {
        "ln_classes": {"RSYN", "CSYN"},
        "keywords": ["sync", "sincr", "synchk", "syncchk"],
    },
    "richiusura": {
        "ln_classes": {"RREC", "XCBR", "CSWI", "XSWI"},
        "keywords": ["reclos", "richius", "autorec"],
    },
    "regolazione_tensione": {
        "ln_classes": {"ATCC", "YLTC", "PTUV", "PTOV"},
        "keywords": ["tapchg", "tappos", "regol", "undervolt", "overvolt",
                     "sottotens", "sovratens"],
    },
    "io_digitali": {
        "ln_classes": {"GGIO"},
        "keywords": ["digital", "ingress", "uscit"],
    },
}
# Functional Constraint "utili" per la validazione (stato, misure, comandi,
# setpoint). Con --tutti-fc si includono anche CF, DC, EX, ...
KEY_FCS = {"ST", "MX", "CO", "SP", "SV", "SG", "SE"}
FCDA_KEYS = ("ldInst", "prefix", "lnClass", "lnInst", "doName", "daName", "fc")


# ------------------------------- utilita' ---------------------------------
def strip_ns(root):
    """Rimuove i namespace XML, cosi' si puo' cercare 'IED' e non '{ns}IED'."""
    for el in root.iter():
        if isinstance(el.tag, str) and "}" in el.tag:
            el.tag = el.tag.split("}", 1)[1]
    return root


def txt(el):
    return (el.text or "").strip() if el is not None else ""


def timing(el):
    """MinTime/MaxTime: valore + multiplier + unit (es. '4 ms')."""
    if el is None:
        return ""
    return f'{txt(el)} {el.get("multiplier", "")}{el.get("unit", "s")}'


# ------------------------- DataTypeTemplates ------------------------------
def parse_templates(root):
    t = root.find("DataTypeTemplates")
    lnt, dot, dat = {}, {}, {}
    if t is None:
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
                     "trg": ",".join(k for k in ("dchg", "qchg", "dupd")
                                     if a.get(k) == "true")}
                    for a in d.findall("DA")],
            "sdos": [{"name": s.get("name"), "type": s.get("type")}
                     for s in d.findall("SDO")],
        }
    for d in t.findall("DAType"):
        dat[d.get("id")] = [{"name": b.get("name"), "bType": b.get("bType"),
                             "type": b.get("type")} for b in d.findall("BDA")]
    return lnt, dot, dat


def expand_da(path, bType, typ, fc, trg, dat, depth=0):
    """Espande ricorsivamente i DA strutturati fino alle foglie."""
    if bType == "Struct" and typ in dat and depth < 8:
        out = []
        for b in dat[typ]:
            out += expand_da(path + [b["name"]], b["bType"], b["type"],
                             fc, trg, dat, depth + 1)
        return out
    return [{"da": ".".join(path), "fc": fc, "bType": bType, "trg": trg}]


def do_attributes(type_id, dot, dat, prefix=(), depth=0):
    """Tutte le foglie (DA) di un DO, con FC, tipo e trigger (dchg/qchg/dupd)."""
    d = dot.get(type_id)
    if not d or depth > 6:
        return []
    out = []
    for a in d["das"]:
        out += expand_da(list(prefix) + [a["name"]], a["bType"], a["type"],
                         a["fc"], a["trg"], dat)
    for s in d["sdos"]:
        out += do_attributes(s["type"], dot, dat,
                             tuple(prefix) + (s["name"],), depth + 1)
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
                }
    return ap_map, gse_map


# --------------------------------- IED ------------------------------------
def parse_ieds(root, tmpl, comm, only=None):
    lnt, dot, _ = tmpl
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
                    doi = {d.get("name"): d for d in el.findall("DOI")}
                    for d in lnt.get(ln["lnType"], {}).get("dos", []):
                        dd = doi.get(d["name"])
                        dais = ({x.get("name"): txt(x.find("Val"))
                                 for x in dd.findall("DAI")} if dd is not None else {})
                        desc = (dd.get("desc") if dd is not None else "") or d["desc"]
                        ln["dos"].append({
                            "name": d["name"], "type": d["type"],
                            "cdc": dot.get(d["type"], {}).get("cdc", ""),
                            "desc": desc or "",
                            "ctlModel": dais.get("ctlModel", ""),
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
                        M["rcb"].append({
                            "ied": iname, "ld_inst": ld_inst, "ld": ld_full,
                            "ln": name, "name": r.get("name"),
                            "rptID": r.get("rptID", ""), "datSet": r.get("datSet", ""),
                            "confRev": r.get("confRev", ""), "buffered": buffered,
                            "bufTime": r.get("bufTime", ""), "intgPd": r.get("intgPd", ""),
                            "indexed": r.get("indexed", ""),
                            "trgops": ",".join(k for k in ("dchg", "qchg", "dupd", "period", "gi")
                                               if trg is not None and trg.get(k) == "true"),
                            "optfields": ",".join(k for k, v in opt.attrib.items() if v == "true")
                                         if opt is not None else "",
                            "rpt_max": en.get("max", "") if en is not None else "",
                            "clients": [c.get("iedName", "") for c in en.findall("ClientLN")]
                                       if en is not None else [],
                            "ref": f'{ld_full}/{name}${"BR" if buffered == "true" else "RP"}${r.get("name")}',
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


def classify(ln, do, ds_name=""):
    text = " ".join([ln["ld"], ln["name"], ln["desc"], do["name"],
                     do["desc"], ds_name]).lower()
    out = [s for s, c in SCENARIOS.items()
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
    "ctlModel", "rif_mms_do", "attributi_mms",
    "dataset_ref", "fcda_nel_dataset",
    "gse_name", "gocbRef", "goID", "confRev", "mac", "appid", "vlan_id",
    "vlan_prio", "min_time", "max_time", "tshark_filter", "report_control",
]


def build_rows(M, tmpl, all_fc=False):
    _, dot, dat = tmpl
    rows = []
    for ln in M["lns"]:
        for do in ln["dos"]:
            attrs = [a for a in do_attributes(do["type"], dot, dat)
                     if all_fc or a["fc"] in KEY_FCS]
            attr_refs = " ; ".join(
                f'{ln["ld"]}/{ln["name"]}.{do["name"]}.{a["da"]}[{a["fc"]}'
                f'{"," + a["trg"] if a["trg"] else ""}]' for a in attrs)
            base = {
                "ied": ln["ied"], "ld": ln["ld"], "ln": ln["name"],
                "lnClass": ln["lnClass"], "ln_desc": ln["desc"], "do": do["name"],
                "cdc": do["cdc"], "do_desc": do["desc"], "ctlModel": do["ctlModel"],
                "rif_mms_do": f'{ln["ld"]}/{ln["name"]}.{do["name"]}',
                "attributi_mms": attr_refs,
            }
            by_ds = {}
            for ds in M["datasets"]:
                for f in ds["fcdas"]:
                    if fcda_matches(f, ds, ln, do):
                        by_ds.setdefault((ds["ld_inst"], ds["name"]), (ds, []))[1].append(
                            f'{f["doName"]}{"." + f["daName"] if f["daName"] else ""}[{f["fc"]}]')
            groups = list(by_ds.values()) or [(None, [])]
            for ds, fcdas in groups:
                scen = classify(ln, do, ds["name"] if ds else "")
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
                        f'{r["ref"]} rptID={r["rptID"]} '
                        f'{"buffered" if r["buffered"] == "true" else "unbuffered"} '
                        f'trg={r["trgops"]} intgPd={r["intgPd"]}' for r in rcbs)
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
    a = ap.parse_args()

    root = strip_ns(ET.parse(a.scl).getroot())
    tmpl = parse_templates(root)
    comm = parse_comm(root)
    M = parse_ieds(root, tmpl, comm, a.ied)
    if not M["ieds"]:
        raise SystemExit("Nessun IED trovato (controlla --ied e il file).")

    rows = build_rows(M, tmpl, a.tutti_fc)
    if not a.tutto:
        rows = [r for r in rows if r["scenario"] != "-"]
    rows.sort(key=lambda r: (r["scenario"], r["ln"], r["do"], r["gse_name"]))

    os.makedirs(a.out, exist_ok=True)
    write_csv(os.path.join(a.out, "tabella_scenari.csv"), rows, ROW_FIELDS)
    write_md(os.path.join(a.out, "tabella_scenari.md"), rows)
    gr = goose_rows(M)
    if gr:
        write_csv(os.path.join(a.out, "goose_inventario.csv"), gr, list(gr[0].keys()))
    if M["rcb"]:
        rr = [dict(r, clients=",".join(r["clients"])) for r in M["rcb"]]
        write_csv(os.path.join(a.out, "report_inventario.csv"), rr, list(rr[0].keys()))
    with open(os.path.join(a.out, "modello.json"), "w", encoding="utf-8") as f:
        json.dump(M, f, indent=2, ensure_ascii=False)

    print(f"IED: {', '.join(i['name'] for i in M['ieds'])}")
    print(f"LN: {len(M['lns'])}  DataSet: {len(M['datasets'])}  "
          f"GSEControl: {len(M['gse'])}  ReportControl: {len(M['rcb'])}")
    print(f"Righe in tabella_scenari: {len(rows)}  -> {a.out}/")
    for g in M["gse"]:
        if not g["comm"]:
            print(f"ATTENZIONE: GSEControl {g['gocbRef']} senza MAC/APPID in "
                  f"<Communication> (ICD generico? usa il CID).")


if __name__ == "__main__":
    main()
