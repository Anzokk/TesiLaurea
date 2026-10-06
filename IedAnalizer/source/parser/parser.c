/*
 * parser.c
 * Parser IEC 61850 per BU9020: in un'unica esecuzione
 *   - legge via MMS i data attribute e lo stato dei Report Control Block;
 *   - riceve in tempo reale i GOOSE presenti in rete durante le letture;
 * e salva tutto in un file JSON (le verifiche sono demandate ai test Robot).
 * Usa libiec61850 (protocolli) e cJSON (lettura della configurazione).
 *
 * Uso:
 *   sudo bin/parser [config.json]        (default: config/parser/parser.json)
 * La ricezione GOOSE usa un socket raw: servono root o CAP_NET_RAW.
 *
 * Configurazione:
 * {
 *   "host": "10.145.100.26",          IP dell'IED (obbligatorio)
 *   "port": 102,                      porta MMS (default 102)
 *   "interface": "enx503f56005b3e",   interfaccia per i GOOSE (obbligatoria)
 *   "goose_time_s": 5,                durata minima dell'ascolto GOOSE (default 5)
 *   "output": "data/results/run.json",
 *   "points": [
 *     {"ref":"BU9020/MMXU1.TotW.mag.f", "key":"potenza_attiva", "fc":"MX"}
 *   ],
 *   "report_blocks": [
 *     {"ref":"BU9020/LLN0.Urcb01", "key":"rcb_urcb01", "fc":"RP"}
 *   ],
 *   "goose": [
 *     {"ref":"BU9020CTRL/LLN0$GO$GcbSync", "key":"gcb_sync", "appid":"0001"}
 *   ]
 * }
 * Punti e RCB: "key" e "fc" opzionali (default: key = ref, fc = MX).
 * GOOSE: vengono ricevuti TUTTI i GOOSE; quelli elencati in "goose" (gocbRef
 * e, se indicato, APPID esadecimale) sono marcati come attesi e prendono la
 * chiave "key", gli altri compaiono come flussi inattesi.
 *
 * Exit code: 0 = completato, 1 = configurazione non valida, 2 = connessione
 * MMS fallita, 3 = output non scrivibile, 4 = ricezione GOOSE non avviata
 * (letture MMS comunque eseguite e salvate).
 *
 * Flusso generale:
 *   1. legge la configurazione;
 *   2. avvia la ricezione GOOSE (prima della connessione MMS);
 *   3. si connette all'IED, legge i data point e lo stato degli RCB;
 *   4. attende la fine dell'ascolto GOOSE e ferma la ricezione;
 *   5. scrive il JSON con valori MMS, flussi GOOSE (con statistiche) e frame.
 */

#define _GNU_SOURCE            /* open_memstream() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <cjson/cJSON.h>       /* lettura della configurazione */
#include "iec61850_client.h"   /* API client di libiec61850 (IedConnection, MmsValue, ...) */
#include "goose_receiver.h"    /* ricezione GOOSE (GooseReceiver) */
#include "goose_subscriber.h"  /* decodifica di un flusso GOOSE (GooseSubscriber) */

#define DEFAULT_CONFIG     "config/parser/parser.json"
#define MAX_GOOSE_STREAMS  64      /* flussi distinti registrati (attesi + inattesi) */
#define MAX_GOOSE_FRAMES   100000  /* frame registrati al massimo; gli altri sono contati */

/* Parametri di esecuzione (primo livello della configurazione) */
typedef struct {
    const char* host;
    int port;
    const char* iface;
    double gooseTimeS;
    char output[256];
} RunCfg;

/* ------------------------------------------------------------------ */
/* Configurazione                                                      */
/* ------------------------------------------------------------------ */

/* Stringa non vuota del campo k di o, oppure def */
static const char* jstr(const cJSON* o, const char* k, const char* def)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : def;
}

/* Valore numerico del campo k di o, oppure def */
static double jnum(const cJSON* o, const char* k, double def)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

/* Array k di o (NULL se assente o non e' un array) */
static const cJSON* jarr(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsArray(v) ? v : NULL;
}

/* Campi di un punto o di un report block della configurazione.
 * ref, key, fc: riferimento, chiave nell'output (default ref), FC (default MX).
 * Se l'elemento non e' valido (manca "ref" o FC sconosciuto) ritorna false e
 * scrive il motivo in problem: l'elemento non viene letto ma compare comunque
 * nell'output con "config_error", sotto la chiave "key" o "senza_ref_<n>". */
static bool config_item(const cJSON* o, int idx, const char** ref, const char** key,
                        FunctionalConstraint* fc, char* keybuf, char* problem, size_t plen)
{
    *ref = jstr(o, "ref", NULL);
    *key = jstr(o, "key", *ref);
    if (!*key) {
        snprintf(keybuf, 32, "senza_ref_%d", idx);
        *key = keybuf;
    }
    /* FunctionalConstraint_fromString guarda solo i primi due caratteri:
     * per questo si controlla anche la lunghezza */
    const char* f = jstr(o, "fc", "MX");
    *fc = strlen(f) == 2 ? FunctionalConstraint_fromString(f) : IEC61850_FC_NONE;

    problem[0] = '\0';
    if (!*ref) snprintf(problem, plen, "manca \"ref\"");
    else if (*fc == IEC61850_FC_NONE) snprintf(problem, plen, "FC sconosciuto \"%s\"", f);
    if (problem[0]) fprintf(stderr, "[parser] %s: %s, non letto\n", *key, problem);
    return !problem[0];
}

/* Legge e analizza il file di configurazione; compila run.
 * Ritorna l'albero JSON (da liberare con cJSON_Delete) o NULL se non valido. */
static cJSON* load_config(const char* filename, RunCfg* run)
{
    FILE* fp = fopen(filename, "rb");
    if (!fp) { perror(filename); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = sz >= 0 ? malloc((size_t)sz + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "config: lettura di %s fallita\n", filename);
        fclose(fp); free(buf); return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        const char* at = cJSON_GetErrorPtr();
        fprintf(stderr, "config: JSON non valido%s%.40s\n", at ? " vicino a: " : "", at ? at : "");
        cJSON_Delete(root); free(buf); return NULL;
    }
    free(buf);

    run->host = jstr(root, "host", NULL);
    run->iface = jstr(root, "interface", NULL);
    run->port = (int)jnum(root, "port", 102);
    run->gooseTimeS = jnum(root, "goose_time_s", 5.0);
    snprintf(run->output, sizeof(run->output), "%s", jstr(root, "output", ""));
    if (!run->output[0])
        snprintf(run->output, sizeof(run->output), "data/results/parser_%ld.json", (long)time(NULL));

    if (!run->host) fprintf(stderr, "config: manca \"host\"\n");
    if (!run->iface) fprintf(stderr, "config: manca \"interface\" (interfaccia per i GOOSE)\n");
    if (!run->host || !run->iface) { cJSON_Delete(root); return NULL; }
    return root;
}

/* ------------------------------------------------------------------ */
/* Valori MMS in JSON                                                  */
/* ------------------------------------------------------------------ */

/* Scrive s come stringa JSON (tra virgolette, con escape dei caratteri speciali) */
static void fprint_json_string(FILE* fp, const char* s) {
    fputc('"', fp);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp);  break;
            case '\r': fputs("\\r", fp);  break;
            case '\t': fputs("\\t", fp);  break;
            default:
                if (c < 0x20) fprintf(fp, "\\u%04x", c);
                else fputc(c, fp);
        }
    }
    fputc('"', fp);
}

/* Nome leggibile di un valore Dbpos (posizione doppia, es. interruttore) */
static const char* dbpos_name(Dbpos d) {
    switch (d) {
        case DBPOS_INTERMEDIATE_STATE: return "intermediate";
        case DBPOS_OFF:                return "off";
        case DBPOS_ON:                 return "on";
        case DBPOS_BAD_STATE:          return "bad";
    }
    return "unknown";
}

/* Scrive un valore MMS come oggetto JSON con tipo e valore, per esempio
 *   {"type": "float", "value": 230.5}
 *   {"type": "dbpos", "value": 2, "label": "on", "bits": "10"}
 *   {"type": "bitstring", "value": 0, "size": 13, "bits": "0000000000000"}
 *   {"type": "struct", "value": [ ...valori tipizzati... ]}
 * Usato sia per le letture MMS sia per i dataset GOOSE (ricorsivo su
 * strutture e array). Bit string: "bits" ha il bit 0 a sinistra, "value" e'
 * l'intero corrispondente (bit 0 = piu' significativo). */
static void fprint_mms_value(FILE* fp, MmsValue* v)
{
    if (!v) { fputs("{\"type\": null, \"value\": null}", fp); return; }
    MmsType t = MmsValue_getType(v);
    switch (t) {
        case MMS_STRUCTURE:
        case MMS_ARRAY: {
            int n = (int)MmsValue_getArraySize(v);
            fprintf(fp, "{\"type\": \"%s\", \"value\": [", t == MMS_STRUCTURE ? "struct" : "array");
            for (int i = 0; i < n; i++) {
                if (i) fputs(", ", fp);
                fprint_mms_value(fp, MmsValue_getElement(v, i));
            }
            fputs("]}", fp);
            break;
        }
        case MMS_BOOLEAN:
            fprintf(fp, "{\"type\": \"bool\", \"value\": %s}", MmsValue_getBoolean(v) ? "true" : "false");
            break;
        case MMS_INTEGER:
            /* Include gli enumerati (es. Mod, Beh, Health) */
            fprintf(fp, "{\"type\": \"int\", \"value\": %lld}", (long long)MmsValue_toInt64(v));
            break;
        case MMS_UNSIGNED:
            fprintf(fp, "{\"type\": \"uint\", \"value\": %lu}", (unsigned long)MmsValue_toUint32(v));
            break;
        case MMS_FLOAT: {
            double d = MmsValue_toDouble(v);
            if (isfinite(d)) fprintf(fp, "{\"type\": \"float\", \"value\": %.9g}", d);
            else fputs("{\"type\": \"float\", \"value\": null}", fp);   /* NaN/Inf non sono JSON */
            break;
        }
        case MMS_BIT_STRING: {
            int n = MmsValue_getBitStringSize(v);
            if (n == 2) {
                /* 2 bit = Dbpos (es. XCBR.Pos.stVal): 0 intermedio, 1 off, 2 on, 3 bad.
                 * Attenzione: anche Tcmd (es. ATCC.TapChg) e' di 2 bit: 1 lower, 2 higher */
                Dbpos d = Dbpos_fromMmsValue(v);
                fprintf(fp, "{\"type\": \"dbpos\", \"value\": %d, \"label\": \"%s\", ",
                        (int)d, dbpos_name(d));
            } else {
                fprintf(fp, "{\"type\": \"bitstring\", \"value\": %u, \"size\": %d, ",
                        MmsValue_getBitStringAsIntegerBigEndian(v), n);
            }
            fputs("\"bits\": \"", fp);
            for (int i = 0; i < n; i++)
                fputc(MmsValue_getBitStringBit(v, i) ? '1' : '0', fp);
            fputs("\"}", fp);
            break;
        }
        case MMS_OCTET_STRING: {
            uint8_t* b = MmsValue_getOctetStringBuffer(v);
            fputs("{\"type\": \"octets\", \"value\": \"", fp);
            for (int i = 0; i < MmsValue_getOctetStringSize(v); i++)
                fprintf(fp, "%02x", b[i]);
            fputs("\"}", fp);
            break;
        }
        case MMS_VISIBLE_STRING:
        case MMS_STRING:
            fputs("{\"type\": \"string\", \"value\": ", fp);
            fprint_json_string(fp, MmsValue_toString(v));
            fputc('}', fp);
            break;
        case MMS_UTC_TIME:
            /* Timestamp IEC 61850 in millisecondi da epoch */
            fprintf(fp, "{\"type\": \"utc_time\", \"value\": %llu}",
                    (unsigned long long)MmsValue_getUtcTimeInMs(v));
            break;
        case MMS_BINARY_TIME:
            fprintf(fp, "{\"type\": \"binary_time\", \"value\": %llu}",
                    (unsigned long long)MmsValue_getBinaryTimeAsUtcMs(v));
            break;
        case MMS_DATA_ACCESS_ERROR:
            /* L'IED ha rifiutato l'accesso all'oggetto (es. 10 = inesistente, 3 = negato) */
            fprintf(fp, "{\"type\": null, \"value\": null, \"access_error\": %d}",
                    (int)MmsValue_getDataAccessError(v));
            break;
        default:
            fprintf(fp, "{\"type\": \"unsupported_%d\", \"value\": null}", t);
            break;
    }
}

/* Legge un oggetto via MMS e scrive "chiave": {valore tipizzato}, oppure
 * {"value": null, "error": <codice IedClientError>} se la richiesta fallisce. */
static void read_point_json(IedConnection con, const char* ref, const char* key,
                            FunctionalConstraint fc, FILE* fp)
{
    IedClientError err;
    MmsValue* value = IedConnection_readObject(con, &err, ref, fc);
    fputs("    ", fp);
    fprint_json_string(fp, key);
    fputs(": ", fp);
    if (value == NULL || err != IED_ERROR_OK)
        fprintf(fp, "{\"type\": null, \"value\": null, \"error\": %d}", err);
    else
        fprint_mms_value(fp, value);
    if (value) MmsValue_delete(value);   /* il valore e' allocato dalla libreria */
}

/* ------------------------------------------------------------------ */
/* GOOSE                                                               */
/* ------------------------------------------------------------------ */

/* Un flusso GOOSE (un GoCB) e le sue informazioni fisse */
typedef struct {
    char key[160];              /* chiave nel JSON: key da config, o gocbRef */
    char gocbRef[130];
    char goId[130];
    char dataSet[130];
    int appid;                  /* -1 = non noto / qualsiasi */
    int vlanId;                 /* -1 = frame senza tag VLAN */
    char srcMac[18], dstMac[18];
    bool expected;              /* GoCB elencato in configurazione */
    bool seen;                  /* almeno un frame ricevuto */
} GooseStream;

/* Un frame GOOSE ricevuto */
typedef struct {
    int stream;                 /* indice in GooseCapture.streams */
    long n;                     /* progressivo dei GOOSE ricevuti */
    double rx_ms;               /* istante di ricezione, ms da epoch */
    uint32_t stNum, sqNum, ttl, confRev;
    uint64_t t_ms;              /* timestamp "t" del GOOSE: ultimo cambio di stato */
    int entries;                /* numero di valori nel dataset */
    bool valid, test, ndsCom;
    char* values;               /* dataset in JSON (malloc) */
} GooseFrameRec;

/* Stato della ricezione: scritto solo dal thread del GooseReceiver e letto
 * dal main solo dopo GooseReceiver_stop(), che attende la fine del thread:
 * non serve un mutex. */
typedef struct {
    GooseStream streams[MAX_GOOSE_STREAMS];
    int nStreams;
    GooseFrameRec* frames;
    int nFrames;
    long received;              /* GOOSE ricevuti in totale */
    long dropped;               /* frame non registrati (limiti superati) */
} GooseCapture;

static double now_epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void mac_to_str(const uint8_t* m, char* out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Flusso a cui appartiene un frame: prima i GoCB attesi (gocbRef uguale e
 * APPID uguale, se indicato), poi i flussi inattesi gia' visti (gocbRef e
 * APPID uguali); altrimenti ne crea uno nuovo. -1 se non c'e' piu' spazio. */
static int goose_stream_for(GooseCapture* cap, const char* ref, int appid)
{
    for (int s = 0; s < cap->nStreams; s++) {
        GooseStream* st = &cap->streams[s];
        if (strcmp(st->gocbRef, ref) != 0) continue;
        if (st->expected ? (st->appid < 0 || st->appid == appid) : st->appid == appid)
            return s;
    }
    if (cap->nStreams == MAX_GOOSE_STREAMS) return -1;
    int s = cap->nStreams++;
    GooseStream* st = &cap->streams[s];
    /* Chiave unica: il gocbRef, con l'APPID se lo stesso gocbRef e' gia' usato */
    bool dup = false;
    for (int i = 0; i < s; i++)
        if (strcmp(cap->streams[i].key, ref) == 0) dup = true;
    if (dup) snprintf(st->key, sizeof(st->key), "%s (appid 0x%04x)", ref, appid);
    else snprintf(st->key, sizeof(st->key), "%s", ref);
    snprintf(st->gocbRef, sizeof(st->gocbRef), "%s", ref);
    st->appid = appid;
    st->expected = false;
    return s;
}

/* Callback chiamata da libiec61850 per ogni frame GOOSE ricevuto (il
 * subscriber e' in modo observer: riceve tutti i GOOSE). Gira sul thread
 * del GooseReceiver: deve essere breve. I valori del dataset vanno usati
 * solo qui (la libreria li riusa al frame successivo), per questo vengono
 * subito convertiti in testo JSON. */
static void goose_listener(GooseSubscriber sub, void* param)
{
    GooseCapture* cap = (GooseCapture*)param;
    double rx = now_epoch_ms();
    cap->received++;
    const char* ref = GooseSubscriber_getGoCbRef(sub);
    int appid = GooseSubscriber_getAppId(sub);

    int s = goose_stream_for(cap, ref ? ref : "", appid);
    if (s < 0) { cap->dropped++; return; }

    GooseStream* st = &cap->streams[s];
    if (!st->seen) {
        /* Informazioni fisse del flusso, prese dal primo frame */
        uint8_t mac[6];
        const char* id = GooseSubscriber_getGoId(sub);
        const char* ds = GooseSubscriber_getDataSet(sub);
        snprintf(st->goId, sizeof(st->goId), "%s", id ? id : "");
        snprintf(st->dataSet, sizeof(st->dataSet), "%s", ds ? ds : "");
        st->appid = appid;
        st->vlanId = GooseSubscriber_isVlanSet(sub) ? GooseSubscriber_getVlanId(sub) : -1;
        GooseSubscriber_getSrcMac(sub, mac);
        mac_to_str(mac, st->srcMac);
        GooseSubscriber_getDstMac(sub, mac);
        mac_to_str(mac, st->dstMac);
        st->seen = true;
    }

    if (cap->nFrames >= MAX_GOOSE_FRAMES) { cap->dropped++; return; }
    GooseFrameRec* f = &cap->frames[cap->nFrames++];
    MmsValue* values = GooseSubscriber_getDataSetValues(sub);
    bool list = values && (MmsValue_getType(values) == MMS_ARRAY ||
                           MmsValue_getType(values) == MMS_STRUCTURE);
    f->stream = s;
    f->n = cap->received;
    f->rx_ms = rx;
    f->stNum = GooseSubscriber_getStNum(sub);
    f->sqNum = GooseSubscriber_getSqNum(sub);
    f->ttl = GooseSubscriber_getTimeAllowedToLive(sub);
    f->confRev = GooseSubscriber_getConfRev(sub);
    f->t_ms = GooseSubscriber_getTimestamp(sub);
    f->valid = GooseSubscriber_isValid(sub);
    f->test = GooseSubscriber_isTest(sub);
    f->ndsCom = GooseSubscriber_needsCommission(sub);
    f->entries = list ? (int)MmsValue_getArraySize(values) : 0;

    /* Dataset -> lista JSON di valori tipizzati, in memoria */
    size_t len = 0;
    f->values = NULL;
    FILE* mem = open_memstream(&f->values, &len);
    if (mem) {
        fputc('[', mem);
        for (int i = 0; i < f->entries; i++) {
            if (i) fputs(", ", mem);
            fprint_mms_value(mem, MmsValue_getElement(values, i));
        }
        fputc(']', mem);
        fclose(mem);
    }
}

/* Crea il ricevitore con un subscriber in modo observer (tutti i GOOSE) e
 * prepara un flusso per ogni GoCB della sezione "goose" della configurazione. */
static GooseReceiver goose_setup(GooseCapture* cap, const cJSON* gooses, const char* iface)
{
    const cJSON* g;
    cJSON_ArrayForEach(g, gooses) {
        const char* ref = jstr(g, "ref", NULL);
        if (!ref) { fprintf(stderr, "[parser] GoCB senza \"ref\", ignorato\n"); continue; }
        if (cap->nStreams == MAX_GOOSE_STREAMS) break;
        GooseStream* st = &cap->streams[cap->nStreams++];
        const char* appid = jstr(g, "appid", NULL);
        snprintf(st->key, sizeof(st->key), "%s", jstr(g, "key", ref));
        snprintf(st->gocbRef, sizeof(st->gocbRef), "%s", ref);
        /* APPID sempre esadecimale, con o senza "0x" (come nell'SCL) */
        st->appid = appid ? (int)strtol(appid, NULL, 16) : -1;
        st->vlanId = -1;
        st->expected = true;
    }

    GooseReceiver rx = GooseReceiver_create();
    GooseReceiver_setInterfaceId(rx, iface);
    static char any[] = "";
    GooseSubscriber s = GooseSubscriber_create(any, NULL);
    GooseSubscriber_setObserver(s);
    GooseSubscriber_setListener(s, goose_listener, cap);
    GooseReceiver_addSubscriber(rx, s);
    return rx;
}

/* Statistiche di un flusso, calcolate nell'ordine di ricezione:
 *   stnumChanges   cambi di stNum (eventi)
 *   sqnumResets    cambi di stNum con sqNum ripartito (0 in Ed.1, 0 o 1 in Ed.2)
 *   maxIntervalMs  intervallo massimo tra due frame consecutivi (<0 = meno di 2 frame)
 *   ttlViolations  frame arrivati oltre il TTL dichiarato dal precedente */
typedef struct {
    int frames, stnumChanges, sqnumResets, ttlViolations, invalid;
    double maxIntervalMs, firstMs, lastMs;
} GooseStats;

static GooseStats goose_stats(const GooseCapture* cap, int s)
{
    GooseStats st = {0};
    st.maxIntervalMs = -1;
    const GooseFrameRec* prev = NULL;
    for (int i = 0; i < cap->nFrames; i++) {
        const GooseFrameRec* f = &cap->frames[i];
        if (f->stream != s) continue;
        if (!f->valid) st.invalid++;
        if (prev) {
            double dt = f->rx_ms - prev->rx_ms;
            if (dt > st.maxIntervalMs) st.maxIntervalMs = dt;
            if (prev->ttl > 0 && dt > prev->ttl) st.ttlViolations++;
            if (f->stNum != prev->stNum) {
                st.stnumChanges++;
                if (f->sqNum <= 1) st.sqnumResets++;
            }
        } else {
            st.firstMs = f->rx_ms;
        }
        st.lastMs = f->rx_ms;
        prev = f;
        st.frames++;
    }
    return st;
}

/* Scrive la sezione "goose" del JSON: flussi con statistiche e frame. */
static void write_goose_json(FILE* fp, const GooseCapture* cap, const char* iface,
                             double listen_ms, const char* error)
{
    fprintf(fp, "  \"goose\": {\n");
    fputs("    \"interface\": ", fp);
    fprint_json_string(fp, iface);
    fputs(",\n", fp);
    if (listen_ms >= 0) fprintf(fp, "    \"listen_time_ms\": %.3f,\n", listen_ms);
    else fputs("    \"listen_time_ms\": null,\n", fp);
    fputs("    \"error\": ", fp);
    if (error && error[0]) fprint_json_string(fp, error); else fputs("null", fp);
    fputs(",\n", fp);
    fprintf(fp, "    \"frames_total\": %d,\n", cap->nFrames);
    fprintf(fp, "    \"frames_dropped\": %ld,\n", cap->dropped);

    fprintf(fp, "    \"streams\": {\n");
    for (int s = 0; s < cap->nStreams; s++) {
        const GooseStream* st = &cap->streams[s];
        GooseStats gs = goose_stats(cap, s);
        fputs("      ", fp);
        fprint_json_string(fp, st->key);
        fprintf(fp, ": {\"expected\": %s, \"gocb_ref\": ", st->expected ? "true" : "false");
        fprint_json_string(fp, st->gocbRef);
        fputs(", \"go_id\": ", fp);
        fprint_json_string(fp, st->goId);
        fputs(", \"dat_set\": ", fp);
        fprint_json_string(fp, st->dataSet);
        if (st->appid >= 0) fprintf(fp, ", \"appid\": \"0x%04x\"", st->appid);
        else fputs(", \"appid\": null", fp);
        if (st->vlanId >= 0) fprintf(fp, ", \"vlan_id\": %d", st->vlanId);
        else fputs(", \"vlan_id\": null", fp);
        if (st->seen) fprintf(fp, ", \"src_mac\": \"%s\", \"dst_mac\": \"%s\"", st->srcMac, st->dstMac);
        fprintf(fp, ", \"frames\": %d", gs.frames);
        if (gs.frames) {
            fprintf(fp, ", \"first_rx_ms\": %.3f, \"last_rx_ms\": %.3f", gs.firstMs, gs.lastMs);
            fprintf(fp, ", \"stnum_changes\": %d, \"sqnum_resets\": %d",
                    gs.stnumChanges, gs.sqnumResets);
            if (gs.maxIntervalMs >= 0) fprintf(fp, ", \"max_interval_ms\": %.3f", gs.maxIntervalMs);
            fprintf(fp, ", \"ttl_violations\": %d, \"invalid_frames\": %d",
                    gs.ttlViolations, gs.invalid);
        }
        fprintf(fp, "}%s\n", s == cap->nStreams - 1 ? "" : ",");
    }
    fprintf(fp, "    },\n");

    fprintf(fp, "    \"frames\": [\n");
    for (int i = 0; i < cap->nFrames; i++) {
        const GooseFrameRec* f = &cap->frames[i];
        fputs("      {\"stream\": ", fp);
        fprint_json_string(fp, cap->streams[f->stream].key);
        fprintf(fp, ", \"n\": %ld, \"rx_ms\": %.3f, \"st_num\": %u, \"sq_num\": %u"
                    ", \"ttl_ms\": %u, \"t_ms\": %llu, \"conf_rev\": %u, \"entries\": %d"
                    ", \"valid\": %s, \"test\": %s, \"nds_com\": %s, \"values\": %s}%s\n",
                f->n, f->rx_ms, f->stNum, f->sqNum, f->ttl, (unsigned long long)f->t_ms,
                f->confRev, f->entries, f->valid ? "true" : "false",
                f->test ? "true" : "false", f->ndsCom ? "true" : "false",
                f->values ? f->values : "null", i == cap->nFrames - 1 ? "" : ",");
    }
    fprintf(fp, "    ]\n");
    fprintf(fp, "  }");
}

static double elapsed_ms(const struct timespec* a, const struct timespec* b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char** argv)
{
    const char* cfgfile = argc > 1 ? argv[1] : DEFAULT_CONFIG;

    /* --- Caricamento configurazione --- */
    RunCfg run;
    cJSON* cfg = load_config(cfgfile, &run);
    if (!cfg) {
        fprintf(stderr, "Errore lettura config %s\n", cfgfile);
        return 1;
    }
    const cJSON* points = jarr(cfg, "points");
    const cJSON* rbs = jarr(cfg, "report_blocks");
    const cJSON* gooses = jarr(cfg, "goose");
    fprintf(stderr, "[parser] %s:%d, %d data points, %d report blocks, %d GoCB attesi, "
            "GOOSE su %s per %.1f s\n", run.host, run.port, cJSON_GetArraySize(points),
            cJSON_GetArraySize(rbs), cJSON_GetArraySize(gooses), run.iface, run.gooseTimeS);

    /* --- Ricezione GOOSE: parte PRIMA della connessione MMS, cosi' i frame
     *     pubblicati durante le letture vengono registrati --- */
    static GooseCapture gcap;
    char goose_err[256] = "";
    struct timespec t_goose_start, t_goose_stop;

    gcap.frames = calloc(MAX_GOOSE_FRAMES, sizeof(GooseFrameRec));
    if (!gcap.frames) { fprintf(stderr, "memoria insufficiente\n"); cJSON_Delete(cfg); return 1; }
    GooseReceiver grx = goose_setup(&gcap, gooses, run.iface);
    GooseReceiver_start(grx);
    clock_gettime(CLOCK_MONOTONIC, &t_goose_start);
    if (!GooseReceiver_isRunning(grx)) {
        snprintf(goose_err, sizeof(goose_err),
                 "ricezione non avviata su %s: servono root o CAP_NET_RAW, "
                 "oppure l'interfaccia non esiste", run.iface);
        fprintf(stderr, "[goose] %s\n", goose_err);
    } else {
        fprintf(stderr, "[goose] in ascolto su %s\n", run.iface);
    }

    /* Timestamp di inizio per misura latenza */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* --- Connessione MMS all'IED --- */
    IedClientError err;
    IedConnection con = IedConnection_create();
    IedConnection_connect(con, &err, run.host, run.port);
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[parser] connessione fallita a %s:%d (err=%d)\n", run.host, run.port, err);
        IedConnection_destroy(con);
        GooseReceiver_destroy(grx);             /* ferma anche il thread GOOSE */
        free(gcap.frames);
        cJSON_Delete(cfg);
        return 2;
    }
    fprintf(stderr, "[parser] connesso a %s:%d\n", run.host, run.port);

    /* Timestamp a connessione stabilita: chiude la misura di latenza di connessione
     * e apre quella del tempo di lettura */
    struct timespec t_connected;
    clock_gettime(CLOCK_MONOTONIC, &t_connected);

    FILE* fp = fopen(run.output, "w");
    if (!fp) {
        perror(run.output);
        IedConnection_close(con);
        IedConnection_destroy(con);
        GooseReceiver_destroy(grx);
        free(gcap.frames);
        cJSON_Delete(cfg);
        return 3;
    }

    /* Corpo JSON: intestazione con metadati della sessione */
    fprintf(fp, "{\n");
    fputs("  \"host\": ", fp);
    fprint_json_string(fp, run.host);
    fputs(",\n", fp);
    fprintf(fp, "  \"port\": %d,\n", run.port);
    fprintf(fp, "  \"timestamp_wall\": %ld,\n", (long)time(NULL));   /* epoch in secondi */
    fprintf(fp, "  \"connect_latency_ms\": %.3f,\n", elapsed_ms(&t_start, &t_connected));

    /* Sezione "values": un elemento per ogni data point */
    const cJSON* item;
    const char *ref, *key;
    FunctionalConstraint fc;
    char keybuf[32], problem[160];
    int idx = 0;
    fprintf(fp, "  \"values\": {\n");
    cJSON_ArrayForEach(item, points) {
        if (idx) fputs(",\n", fp);
        if (config_item(item, idx, &ref, &key, &fc, keybuf, problem, sizeof(problem))) {
            read_point_json(con, ref, key, fc, fp);
        } else {
            fputs("    ", fp);
            fprint_json_string(fp, key);
            fputs(": {\"type\": null, \"value\": null, \"config_error\": ", fp);
            fprint_json_string(fp, problem);
            fputc('}', fp);
        }
        idx++;
    }
    fprintf(fp, "%s  },\n", idx ? "\n" : "");

    /* Report Control Blocks: verifica presenza e attributi base */
    idx = 0;
    fprintf(fp, "  \"report_blocks\": {\n");
    cJSON_ArrayForEach(item, rbs) {
        if (idx++) fputs(",\n", fp);
        bool ok = config_item(item, idx - 1, &ref, &key, &fc, keybuf, problem, sizeof(problem));
        fputs("    ", fp);
        fprint_json_string(fp, key);
        fputs(": ", fp);
        if (!ok) {
            fputs("{\"RptEna\": null, \"config_error\": ", fp);
            fprint_json_string(fp, problem);
            fputc('}', fp);
            continue;
        }
        /* Si legge solo l'attributo RptEna (report abilitato si'/no).
         * Il FC viene passato a parte, quindi un eventuale segmento ".RP." / ".BR."
         * nel riferimento (stile MMS, es. "LLN0.RP.Urcb01") va rimosso:
         * il riferimento corretto e' "LLN0.Urcb01". */
        char attr[300];
        snprintf(attr, sizeof(attr), "%s.RptEna", ref);
        char* seg = strstr(attr, ".RP.");
        if (!seg) seg = strstr(attr, ".BR.");
        if (seg) memmove(seg, seg + 3, strlen(seg + 3) + 1);
        MmsValue* v = IedConnection_readObject(con, &err, attr, fc);
        if (v && err == IED_ERROR_OK && MmsValue_getType(v) == MMS_BOOLEAN) {
            fprintf(fp, "{\"RptEna\": %s}", MmsValue_getBoolean(v) ? "true" : "false");
        } else if (v && err == IED_ERROR_OK && MmsValue_getType(v) == MMS_DATA_ACCESS_ERROR) {
            /* RCB non presente sull'IED (o non accessibile) */
            fprintf(fp, "{\"RptEna\": null, \"access_error\": %d}",
                    (int)MmsValue_getDataAccessError(v));
        } else {
            fprintf(fp, "{\"RptEna\": null, \"error\": %d}", err);
        }
        if (v) MmsValue_delete(v);
    }
    fprintf(fp, "%s  },\n", idx ? "\n" : "");

    /* Tempo totale impiegato per tutte le letture (dopo la connessione) */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    fprintf(fp, "  \"total_read_time_ms\": %.3f,\n", elapsed_ms(&t_connected, &t_end));

    /* --- Fine ricezione GOOSE: ascolto per almeno goose_time_s dall'avvio --- */
    double listen_ms = -1;
    if (GooseReceiver_isRunning(grx)) {
        struct timespec now, pause = {0, 20 * 1000000L};
        clock_gettime(CLOCK_MONOTONIC, &now);
        while (elapsed_ms(&t_goose_start, &now) < run.gooseTimeS * 1000.0) {
            nanosleep(&pause, NULL);
            clock_gettime(CLOCK_MONOTONIC, &now);
        }
        GooseReceiver_stop(grx);    /* attende la fine del thread di ricezione */
        clock_gettime(CLOCK_MONOTONIC, &t_goose_stop);
        listen_ms = elapsed_ms(&t_goose_start, &t_goose_stop);
    }
    fprintf(stderr, "[goose] %d frame GOOSE in %d flussi\n", gcap.nFrames, gcap.nStreams);
    write_goose_json(fp, &gcap, run.iface, listen_ms, goose_err);

    fprintf(fp, "\n}\n");
    fclose(fp);

    fprintf(stderr, "[parser] dati salvati in %s\n", run.output);

    /* --- Chiusura connessione e rilascio risorse --- */
    IedConnection_close(con);
    IedConnection_destroy(con);
    GooseReceiver_destroy(grx);                     /* libera anche il subscriber */
    for (int i = 0; i < gcap.nFrames; i++) free(gcap.frames[i].values);
    free(gcap.frames);
    cJSON_Delete(cfg);                              /* run.host/iface puntano qui */
    return goose_err[0] ? 4 : 0;
}
