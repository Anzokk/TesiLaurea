#!/usr/bin/env python3
"""
ParserGooseV2.py
Analizzatore automatico dei flussi GOOSE per la suite di test BU9020.

Uso:
    python3 source/goose/ParserGooseV2.py <input.pcapng> <expected.json> <report.json>

Dove expected.json definisce i criteri di verifica dello scenario
(tutte le chiavi sono opzionali; un check viene eseguito solo se la sua
chiave e' presente):
{
  "scenario": "sync_check_ok",
  "event_time_epoch": 1712345678.123,
  "clock_offset_ms": 0,
  "expected_gocb_ref": "BU9020/LLN0$GO$GcbSync",
  "expected_dataset":  "BU9020/LLN0$SyncDS",
  "expected_go_id":    "SYNC_STATUS",
  "expected_conf_rev": 1,
  "expected_stnum_changes": 1,
  "max_retransmission_interval_ms": 1100,
  "max_event_to_goose_latency_ms": 50,
  "forbid_unexpected_frames": true,
  "expected_entries": 4
}

- event_time_epoch: istante dell'evento, nella base tempi della cattura;
- clock_offset_ms: correzione da sommare a event_time_epoch se l'evento e'
  registrato con un orologio diverso da quello della macchina di cattura
  (offset = orologio_cattura - orologio_evento).

Il report.json prodotto è pronto per il capitolo di validazione sperimentale.
Exit code: 0 = tutti i check passati, 1 = almeno un check fallito.
"""

# Flusso generale:
#   1. pyshark (tshark) legge il pcapng e restituisce solo i pacchetti GOOSE;
#   2. ogni pacchetto diventa un GooseFrame con i campi dell'header GOOSE;
#   3. i frame vengono raggruppati per GoCB e ordinati nel tempo; si calcolano
#      cambi di stNum, reset di sqNum e intervalli tra frame consecutivi;
#   4. si eseguono i check definiti in expected.json;
#   5. si scrive report.json e si stampa un riepilogo PASS/FAIL.
#
# Richiami GOOSE (IEC 61850-8-1):
#   - stNum: incrementato a ogni cambio di stato del dataset (evento);
#   - sqNum: incrementato a ogni ritrasmissione dello stesso stato, riparte
#     quando cambia stNum (da 0 in Ed.1, da 0 o 1 a seconda dell'implementazione
#     in Ed.2: si accettano entrambi);
#   - dopo un evento i frame vengono ritrasmessi a intervalli crescenti fino
#     al tempo di heartbeat; timeAllowedtoLive (TTL) e' il tempo entro cui il
#     ricevitore deve vedere il frame successivo.

import sys
import json
from collections import defaultdict
from dataclasses import dataclass, field, asdict

import pyshark   # wrapper Python di tshark: richiede tshark installato


# Valori massimi di sqNum accettati come "reset" dopo un cambio di stNum
SQNUM_RESET_VALUES = (0, 1)

# Campi GOOSE obbligatori: se mancano in un frame la decodifica e' incompleta
REQUIRED_FIELDS = ("gocbRef", "goID", "datSet", "stNum", "sqNum",
                   "confRev", "timeAllowedtoLive", "numDatSetEntries")

# Quanti frame "problematici" riportare al massimo nel report per ogni check
MAX_LISTED = 10


# ---------- Struttura dati ----------

@dataclass
class GooseFrame:
    """Un singolo frame GOOSE estratto dalla cattura."""
    frame_no: int         # numero del frame nel pcap
    time_epoch: float     # istante di cattura (secondi da epoch)
    src_mac: str          # MAC del publisher
    dst_mac: str          # MAC multicast di destinazione
    appid: str            # APPID GOOSE (es. 0x0001)
    gocb_ref: str         # riferimento del GOOSE Control Block
    go_id: str            # identificativo GOOSE (goID)
    dat_set: str          # riferimento del dataset pubblicato
    st_num: int           # state number
    sq_num: int           # sequence number
    conf_rev: int         # revisione di configurazione
    ttl: int              # timeAllowedtoLive (ms)
    t_goose: str          # timestamp interno GOOSE (ultimo cambio di stato)
    payload_entries: int  # numDatSetEntries: numero di valori nel payload
    raw_len: int          # lunghezza del frame in byte
    missing_fields: list = field(default_factory=list)  # campi non decodificati
    malformed: bool = False  # tshark ha segnalato il pacchetto come malformato


@dataclass
class CheckResult:
    """Esito di una singola verifica."""
    name: str
    passed: bool
    detail: str = ""         # descrizione leggibile
    value: object = None     # valore misurato
    expected: object = None  # valore atteso / limite


@dataclass
class ScenarioReport:
    """Contenuto del report.json finale."""
    scenario: str
    pcap: str
    total_frames: int
    analyzed_gocb: str = None     # GoCB su cui sono calcolate le statistiche
    analyzed_frames: int = 0
    gocb_refs_seen: list = field(default_factory=list)
    go_ids_seen: list = field(default_factory=list)
    malformed_frames: int = 0
    incomplete_frames: int = 0
    stnum_transitions: int = 0
    sqnum_resets: int = 0
    retransmission_intervals_ms: list = field(default_factory=list)
    event_to_goose_latency_ms: float = None
    event_frame_no: int = None
    checks: list = field(default_factory=list)
    overall_pass: bool = False


# ---------- Utility ----------

def safe_int(v, default=0):
    """int(v) (anche esadecimale "0x..."), oppure default se non numerico."""
    try:
        return int(v, 0) if isinstance(v, str) else int(v)
    except (TypeError, ValueError):
        return default


def safe_float(v, default=0.0):
    """float(v), oppure default se v e' None o non numerico."""
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def group_by_gocb(frames):
    """Raggruppa i frame per gocbRef, ciascun gruppo ordinato nel tempo.
    Ogni GoCB ha la sua sequenza stNum/sqNum indipendente."""
    groups = defaultdict(list)
    for f in frames:
        groups[f.gocb_ref].append(f)
    for flist in groups.values():
        flist.sort(key=lambda x: (x.time_epoch, x.frame_no))
    return groups


def consecutive_pairs(frames):
    """Coppie (precedente, successivo) di frame consecutivi dello stesso GoCB."""
    for flist in group_by_gocb(frames).values():
        for i in range(1, len(flist)):
            yield flist[i - 1], flist[i]


def frames_of(frames, gocb):
    """Frame del solo GoCB indicato; tutti i frame se gocb non e' definito."""
    if not gocb:
        return frames
    return [f for f in frames if f.gocb_ref == gocb]


# ---------- Estrazione frame GOOSE ----------

def extract_goose_frames(pcap_file):
    """Legge il pcap e restituisce la lista di GooseFrame (in ordine di cattura)."""
    print(f"[analyzer] parsing {pcap_file}...")
    # display_filter='goose': tshark restituisce solo i pacchetti GOOSE.
    # keep_packets=False: i pacchetti non restano in memoria dopo l'iterazione.
    cap = pyshark.FileCapture(pcap_file, display_filter='goose', keep_packets=False)
    frames = []
    try:
        for pkt in cap:
            if not hasattr(pkt, 'goose'):
                continue
            g = pkt.goose
            # I nomi degli attributi sono quelli dei campi Wireshark senza il
            # prefisso "goose." (es. goose.stNum -> g.stNum).
            missing = [name for name in REQUIRED_FIELDS
                       if getattr(g, name, None) in (None, '')]
            # tshark aggiunge un layer "_ws.malformed" ai pacchetti che non
            # riesce a decodificare completamente
            malformed = any('malformed' in l.layer_name.lower() for l in pkt.layers)
            f = GooseFrame(
                frame_no=safe_int(getattr(pkt.frame_info, 'number', 0)),
                time_epoch=safe_float(getattr(pkt.frame_info, 'time_epoch', 0)),
                src_mac=getattr(pkt.eth, 'src', ''),
                dst_mac=getattr(pkt.eth, 'dst', ''),
                appid=getattr(g, 'appid', ''),
                gocb_ref=getattr(g, 'gocbRef', ''),
                go_id=getattr(g, 'goID', ''),
                dat_set=getattr(g, 'datSet', ''),
                st_num=safe_int(getattr(g, 'stNum', 0)),
                sq_num=safe_int(getattr(g, 'sqNum', 0)),
                conf_rev=safe_int(getattr(g, 'confRev', 0)),
                ttl=safe_int(getattr(g, 'timeAllowedtoLive', 0)),
                t_goose=getattr(g, 't', ''),
                payload_entries=safe_int(getattr(g, 'numDatSetEntries', 0)),
                raw_len=safe_int(getattr(pkt.frame_info, 'len', 0)),
                missing_fields=missing,
                malformed=malformed,
            )
            frames.append(f)
    finally:
        cap.close()   # chiude il processo tshark sottostante
    print(f"[analyzer] {len(frames)} frame GOOSE trovati")

    incomplete = [f for f in frames if f.missing_fields]
    if incomplete:
        print(f"[analyzer] ATTENZIONE: {len(incomplete)} frame con campi non "
              f"decodificati (es. frame {incomplete[0].frame_no}: "
              f"{', '.join(incomplete[0].missing_fields)})")
    malformed = [f for f in frames if f.malformed]
    if malformed:
        print(f"[analyzer] ATTENZIONE: {len(malformed)} frame segnalati come "
              f"malformati da tshark (es. frame {malformed[0].frame_no})")
    return frames


# ---------- Analisi sequenza ----------

def analyze_sequence(frames):
    """Calcola transizioni stNum, reset sqNum, intervalli di ritrasmissione.
    Passare i soli frame del GoCB di interesse per statistiche non inquinate
    da altri publisher."""
    stnum_transitions = 0
    sqnum_resets = 0
    intervals_ms = []

    for prev, f in consecutive_pairs(frames):
        if f.st_num != prev.st_num:
            # Cambio di stato: sqNum deve ripartire
            stnum_transitions += 1
            if f.sq_num in SQNUM_RESET_VALUES:
                sqnum_resets += 1
        # Tempo trascorso dal frame precedente dello stesso GoCB
        intervals_ms.append((f.time_epoch - prev.time_epoch) * 1000.0)

    return stnum_transitions, sqnum_resets, intervals_ms


def find_event_frame(frames, event_epoch, expected_gocb):
    """Trova il primo frame GOOSE che riporta il nuovo stato dopo l'evento.

    Il frame dell'evento e' il primo frame del GoCB, a partire da event_epoch,
    con stNum diverso da quello in vigore prima dell'evento (le ritrasmissioni
    del vecchio stato vengono ignorate). Se non ci sono frame prima
    dell'evento si usa il primo frame successivo con sqNum azzerato.
    Ritorna (latenza_ms, frame) oppure (None, None)."""
    if event_epoch is None:
        return None, None
    flist = sorted(frames_of(frames, expected_gocb),
                   key=lambda x: (x.time_epoch, x.frame_no))
    before = [f for f in flist if f.time_epoch < event_epoch]
    after = [f for f in flist if f.time_epoch >= event_epoch]

    if before:
        old_st = before[-1].st_num
        candidates = [f for f in after if f.st_num != old_st]
    else:
        candidates = [f for f in after if f.sq_num in SQNUM_RESET_VALUES]
    if not candidates:
        return None, None
    ev = candidates[0]
    return (ev.time_epoch - event_epoch) * 1000.0, ev


def event_epoch_from(expected):
    """event_time_epoch corretto con l'eventuale clock_offset_ms."""
    t = expected.get("event_time_epoch")
    if t is None:
        return None
    return t + expected.get("clock_offset_ms", 0) / 1000.0


# ---------- Verifiche (check) ----------

def run_checks(frames, expected):
    """Esegue i check abilitati in expected.json e ne restituisce gli esiti.
    I check di sequenza (stNum, sqNum, intervalli, TTL, latenza) sono
    calcolati sul solo GoCB atteso, se definito."""
    checks = []

    def add(name, passed, detail="", value=None, exp=None):
        checks.append(CheckResult(name, passed, detail, value, exp))

    exp_gocb = expected.get("expected_gocb_ref")
    target = frames_of(frames, exp_gocb)   # frame del GoCB sotto test
    scope = f"GoCB '{exp_gocb}'" if exp_gocb else "tutti i GoCB"

    # 1. Presenza frame GOOSE
    add("frames_present", len(frames) > 0,
        f"trovati {len(frames)} frame", len(frames), ">0")

    # 2. Decodifica completa: tutti i campi obbligatori presenti, altrimenti
    #    gli altri check lavorano su valori di default.
    #    I frame "malformati" sono solo segnalati (malformed_frames nel report):
    #    tshark 4.2.x marca cosi' tutti i GOOSE con allData annidato per un suo
    #    bug ("recursion_depth <= 100"), pur decodificando correttamente l'header.
    bad = [f.frame_no for f in target if f.missing_fields]
    add("frames_decoded", len(bad) == 0,
        f"{len(bad)} frame con campi obbligatori mancanti ({scope})",
        bad[:MAX_LISTED], [])

    # 3. gocbRef atteso (deve comparire almeno una volta)
    if exp_gocb:
        seen = sorted({f.gocb_ref for f in frames})
        add("gocb_ref_match", exp_gocb in seen,
            f"gocbRef atteso '{exp_gocb}'", seen, exp_gocb)

    # 4. datSet atteso: tutti i frame del GoCB devono riportarlo
    exp_ds = expected.get("expected_dataset")
    if exp_ds:
        seen = sorted({f.dat_set for f in target})
        add("dataset_match", seen == [exp_ds],
            f"datSet atteso '{exp_ds}' ({scope})", seen, exp_ds)

    # 5. goID atteso
    exp_go = expected.get("expected_go_id")
    if exp_go:
        seen = sorted({f.go_id for f in target})
        add("go_id_match", seen == [exp_go],
            f"goID atteso '{exp_go}' ({scope})", seen, exp_go)

    # 6. confRev: costante per GoCB ed eventualmente uguale a quello atteso
    exp_cr = expected.get("expected_conf_rev")
    revs_by_gocb = {g: sorted({f.conf_rev for f in fl})
                    for g, fl in group_by_gocb(target).items()}
    changed = {g: r for g, r in revs_by_gocb.items() if len(r) > 1}
    if exp_cr is not None:
        seen = sorted({f.conf_rev for f in target})
        add("conf_rev", seen == [exp_cr] and not changed,
            f"confRev atteso {exp_cr} ({scope})", seen, exp_cr)
    elif target:
        add("conf_rev_stable", not changed,
            f"confRev costante per GoCB ({scope})", changed or None, "costante")

    # 7. numero transizioni stNum
    stnum_trans, sq_resets, intervals = analyze_sequence(target)
    exp_stnum = expected.get("expected_stnum_changes")
    if exp_stnum is not None:
        add("stnum_changes", stnum_trans == exp_stnum,
            f"transizioni stNum ({scope})", stnum_trans, exp_stnum)

    # 8. reset di sqNum a ogni cambio di stNum
    if stnum_trans > 0:
        no_reset = [f.frame_no for prev, f in consecutive_pairs(target)
                    if f.st_num != prev.st_num and f.sq_num not in SQNUM_RESET_VALUES]
        add("sqnum_reset", len(no_reset) == 0,
            f"{sq_resets}/{stnum_trans} cambi di stNum con sqNum azzerato ({scope})",
            no_reset[:MAX_LISTED], list(SQNUM_RESET_VALUES))

    # 9. TTL rispettato: ogni frame deve arrivare entro il timeAllowedtoLive
    #    dichiarato dal frame precedente dello stesso GoCB
    late = [{"frame": f.frame_no,
             "interval_ms": round((f.time_epoch - prev.time_epoch) * 1000.0, 2),
             "ttl_ms": prev.ttl}
            for prev, f in consecutive_pairs(target)
            if prev.ttl > 0 and (f.time_epoch - prev.time_epoch) * 1000.0 > prev.ttl]
    if len(target) >= 2:
        add("ttl_respected", len(late) == 0,
            f"{len(late)} frame arrivati oltre il TTL del frame precedente ({scope})",
            late[:MAX_LISTED], "nessuno")

    # 10. intervallo massimo di ritrasmissione (limite fisso opzionale;
    #     deve essere coerente con l'heartbeat configurato sull'IED)
    max_ms = expected.get("max_retransmission_interval_ms")
    if max_ms is not None and intervals:
        actual = max(intervals)
        add("retransmission_interval",
            actual <= max_ms,
            f"max intervallo {actual:.1f} ms (limite {max_ms} ms, {scope})",
            round(actual, 2), max_ms)

    # 11. latenza evento → GOOSE (primo frame con il nuovo stNum)
    exp_lat = expected.get("max_event_to_goose_latency_ms")
    event_t = event_epoch_from(expected)
    if exp_lat is not None and event_t is not None:
        times = [f.time_epoch for f in target]
        if times and not (min(times) <= event_t <= max(times)):
            # Evento fuori dalla finestra di cattura: quasi sempre orologi
            # non sincronizzati o event_time_epoch sbagliato
            add("event_to_goose_latency", False,
                "evento fuori dalla finestra temporale della cattura: "
                "verificare la sincronizzazione degli orologi / clock_offset_ms",
                None, f"<={exp_lat}")
        else:
            lat, ev = find_event_frame(frames, event_t, exp_gocb)
            if lat is None:
                add("event_to_goose_latency", False,
                    "nessun cambio di stNum dopo l'evento", None, f"<={exp_lat}")
            else:
                add("event_to_goose_latency", lat <= exp_lat,
                    f"latenza {lat:.2f} ms (frame {ev.frame_no}, stNum {ev.st_num})",
                    round(lat, 2), f"<={exp_lat}")

    # 12. assenza frame inattesi: ogni frame deve appartenere al GoCB atteso
    if expected.get("forbid_unexpected_frames"):
        if not exp_gocb:
            add("no_unexpected_frames", False,
                "forbid_unexpected_frames richiede expected_gocb_ref", None, "nessuno")
        else:
            unexpected = [{"frame": f.frame_no, "gocb_ref": f.gocb_ref}
                          for f in frames if f.gocb_ref != exp_gocb]
            add("no_unexpected_frames", len(unexpected) == 0,
                f"{len(unexpected)} frame inattesi",
                unexpected[:MAX_LISTED], "nessuno")

    # 13. numero entries del dataset (payload coerente)
    exp_entries = expected.get("expected_entries")
    if exp_entries is not None and target:
        bad = [{"frame": f.frame_no, "entries": f.payload_entries}
               for f in target if f.payload_entries != exp_entries]
        add("payload_entries", len(bad) == 0,
            f"{len(bad)} frame con numero di voci diverso da {exp_entries} ({scope})",
            bad[:MAX_LISTED], exp_entries)

    return checks


# ---------- Main ----------

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    pcap_file = sys.argv[1]
    expected_file = sys.argv[2]
    report_file = sys.argv[3]

    # Criteri di verifica dello scenario
    with open(expected_file, 'r') as f:
        expected = json.load(f)

    frames = extract_goose_frames(pcap_file)

    # Statistiche di sequenza (informative) sul GoCB atteso
    exp_gocb = expected.get("expected_gocb_ref")
    target = frames_of(frames, exp_gocb)
    stnum_trans, sq_resets, intervals = analyze_sequence(target)
    latency, ev = find_event_frame(frames, event_epoch_from(expected), exp_gocb)

    report = ScenarioReport(
        scenario=expected.get("scenario", "unknown"),
        pcap=pcap_file,
        total_frames=len(frames),
        analyzed_gocb=exp_gocb,
        analyzed_frames=len(target),
        gocb_refs_seen=sorted({f.gocb_ref for f in frames}),
        go_ids_seen=sorted({f.go_id for f in frames}),
        malformed_frames=sum(f.malformed for f in frames),
        incomplete_frames=sum(bool(f.missing_fields) for f in frames),
        stnum_transitions=stnum_trans,
        sqnum_resets=sq_resets,
        retransmission_intervals_ms=[round(x, 2) for x in intervals],
        event_to_goose_latency_ms=round(latency, 3) if latency is not None else None,
        event_frame_no=ev.frame_no if ev else None,
    )

    # Verifiche: l'esito globale e' PASS solo se tutti i check passano
    checks = run_checks(frames, expected)
    report.checks = [asdict(c) for c in checks]
    report.overall_pass = all(c.passed for c in checks)

    with open(report_file, 'w') as f:
        json.dump(asdict(report), f, indent=2)

    # Stampa riepilogo a console
    print("\n===== REPORT =====")
    for c in checks:
        status = "PASS" if c.passed else "FAIL"
        print(f"[{status}] {c.name}: {c.detail}")
    print(f"\nEsito globale: {'PASS' if report.overall_pass else 'FAIL'}")
    print(f"Report salvato in {report_file}")

    # Exit code utilizzabile da script/CI
    sys.exit(0 if report.overall_pass else 1)


if __name__ == "__main__":
    main()
