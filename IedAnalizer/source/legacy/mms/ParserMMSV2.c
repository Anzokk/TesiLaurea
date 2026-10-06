/*
 * ParserMMSV2.c
 * Client MMS per BU9020 - componente di verifica automatica.
 *
 * Uso:
 *   bin/ParserMMSV2 <host> <port> <config.json> <output.json> [opzioni GOOSE]
 *
 * Opzioni GOOSE (facoltative):
 *   --goose <interfaccia>    riceve i GOOSE in tempo reale durante le letture MMS
 *                            (socket raw: servono root o CAP_NET_RAW)
 *   --goose-time <secondi>   durata minima dell'ascolto GOOSE (default 5)
 *   --goose-pcap <file>      decodifica i GOOSE di una cattura .pcap/.pcapng
 *                            (analisi offline, non servono privilegi)
 * Con <host> = "-" non viene aperta la connessione MMS (solo GOOSE).
 *
 * Il file config.json definisce i punti da leggere:
 * {
 *   "points": [
 *     {"ref":"BU9020/CSWI1.Pos.stVal",  "key":"pos_interruttore", "fc":"ST"},
 *     {"ref":"BU9020/MMXU1.TotW.mag.f", "key":"potenza_attiva",   "fc":"MX"},
 *     {"ref":"BU9020/RSYN1.Sync.stVal", "key":"consenso_sync",    "fc":"ST"},
 *     {"ref":"BU9020/ATCC1.TapPos.stVal","key":"posizione_tap",   "fc":"ST"}
 *   ],
 *   "report_blocks": [
 *     {"ref":"BU9020/LLN0.RP.Urcb01", "key":"rcb_urcb01", "fc":"RP"}
 *   ],
 *   "goose": [
 *     {"ref":"BU9020CTRL/LLN0$GO$GcbSync", "key":"gcb_sync", "appid":"0x0001"}
 *   ]
 * }
 * "key" e "fc" sono opzionali (default: key = ref, fc = MX).
 * "goose" e' opzionale: elenca i GoCB da sottoscrivere (gocbRef come compare
 * nei frame, appid esadecimale facoltativo). Se manca o e' vuoto, con le
 * opzioni GOOSE si ricevono TUTTI i GOOSE presenti in rete (modo observer).
 *
 * Flusso generale:
 *   1. legge config.json e costruisce la lista di data point e di report block;
 *   2. (se richiesto) avvia la ricezione GOOSE;
 *   3. si connette all'IED via MMS (libiec61850);
 *   4. legge ogni data point e ne scrive valore/tipo in output.json;
 *   5. per ogni Report Control Block legge l'attributo RptEna;
 *   6. annota nel JSON le latenze di connessione e di lettura;
 *   7. ferma la ricezione GOOSE e scrive frame e statistiche per ogni flusso.
 */

#define _GNU_SOURCE            /* open_memstream() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "iec61850_client.h"   /* API client di libiec61850 (IedConnection, MmsValue, ...) */
#include "goose_receiver.h"    /* ricezione GOOSE (GooseReceiver) */
#include "goose_subscriber.h"  /* decodifica di un flusso GOOSE (GooseSubscriber) */

#define MAX_POINTS 256  /* numero massimo di data point / report block caricabili */
#define MAX_GOOSE_SUBS     32      /* GoCB sottoscrivibili da configurazione */
#define MAX_GOOSE_STREAMS  64      /* flussi distinti registrati (modo observer) */
#define MAX_GOOSE_FRAMES   100000  /* frame registrati al massimo; gli altri sono contati */

/* Un data point da leggere: riferimento IEC 61850, nome nel JSON di output, FC */
typedef struct {
    char objectRef[256];        /* es. "BU9020/MMXU1.TotW.mag.f" */
    char jsonKey[128];          /* chiave usata nel JSON di output */
    FunctionalConstraint fc;    /* Functional Constraint (ST, MX, SP, ...) */
} DataPoint;

/* Un Report Control Block: stessa struttura di DataPoint, tenuta separata
 * perche' viene gestita in modo diverso (si legge solo RptEna) */
typedef struct {
    char objectRef[256];        /* es. "BU9020/LLN0.RP.Urcb01" */
    char jsonKey[128];
    FunctionalConstraint fc;    /* tipicamente RP (unbuffered) o BR (buffered) */
} ReportBlockRef;

/* Un GoCB da sottoscrivere (sezione "goose" della configurazione) */
typedef struct {
    char gocbRef[130];          /* es. "LD/LLN0$GO$gcb1" (max 129 caratteri) */
    char jsonKey[130];          /* default = gocbRef */
    int appid;                  /* APPID atteso, -1 = non filtrare */
} GooseSubRef;

/* Mappa stringa -> FunctionalConstraint.
 * Se la stringa e' assente si usa MX; se non e' riconosciuta si avvisa e si usa MX. */
static FunctionalConstraint parse_fc(const char* s) {
    if (!s || !s[0]) return IEC61850_FC_MX;
    if (strcmp(s, "MX") == 0) return IEC61850_FC_MX;   /* misure */
    if (strcmp(s, "ST") == 0) return IEC61850_FC_ST;   /* stati */
    if (strcmp(s, "CO") == 0) return IEC61850_FC_CO;   /* comandi */
    if (strcmp(s, "SP") == 0) return IEC61850_FC_SP;   /* setpoint */
    if (strcmp(s, "CF") == 0) return IEC61850_FC_CF;   /* configurazione */
    if (strcmp(s, "DC") == 0) return IEC61850_FC_DC;   /* descrizione */
    if (strcmp(s, "SG") == 0) return IEC61850_FC_SG;   /* setting group */
    if (strcmp(s, "SE") == 0) return IEC61850_FC_SE;   /* setting group editabile */
    if (strcmp(s, "RP") == 0) return IEC61850_FC_RP;   /* unbuffered report */
    if (strcmp(s, "BR") == 0) return IEC61850_FC_BR;   /* buffered report */
    fprintf(stderr, "[client] FC sconosciuto \"%s\", uso MX\n", s);
    return IEC61850_FC_MX;
}

/* ------------------------------------------------------------------ */
/* Parser JSON minimale                                                */
/* Lavora su intervalli [begin, end) del buffer, cosi' ogni ricerca    */
/* resta confinata nell'oggetto/array corrente.                        */
/* ------------------------------------------------------------------ */

/* Dato p che punta a una virgoletta di apertura, ritorna il puntatore alla
 * virgoletta di chiusura (saltando le sequenze di escape), o NULL. */
static const char* skip_string(const char* p, const char* end) {
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p;
    }
    return NULL;
}

/* Dato p che punta a '[' o '{', ritorna il puntatore alla parentesi di
 * chiusura corrispondente (tenendo conto di annidamenti e stringhe), o NULL. */
static const char* find_matching(const char* p, const char* end) {
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (!p) return NULL;
        } else if (*p == '[' || *p == '{') {
            depth++;
        } else if (*p == ']' || *p == '}') {
            if (--depth == 0) return p;
        }
    }
    return NULL;
}

/* Cerca la chiave "key" come chiave (non come valore) a livello superficiale
 * nell'intervallo [begin, end) e ritorna il puntatore al primo carattere non
 * spazio dopo i ':', o NULL se non trovata. */
static const char* find_key(const char* begin, const char* end, const char* key) {
    size_t klen = strlen(key);
    int depth = 0;
    for (const char* p = begin; p < end; p++) {
        if (*p == '[' || *p == '{') { depth++; continue; }
        if (*p == ']' || *p == '}') { depth--; continue; }
        if (*p != '"') continue;
        const char* q = skip_string(p, end);
        if (!q) return NULL;
        /* Confronta il contenuto della stringa con la chiave cercata */
        int match = (depth <= 1 && (size_t)(q - p - 1) == klen &&
                     memcmp(p + 1, key, klen) == 0);
        p = q;
        /* E' una chiave solo se e' seguita da ':' */
        const char* r = q + 1;
        while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) r++;
        if (r < end && *r == ':') {
            if (match) {
                r++;
                while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) r++;
                return r;
            }
            p = r;
        }
    }
    return NULL;
}

/* Estrae il valore stringa della chiave "key" nell'oggetto [begin, end).
 * Decodifica gli escape semplici (\" \\ \/ \n \t ...); tronca a outlen-1.
 * Ritorna 1 se trovato, 0 altrimenti (out resta stringa vuota). */
static int extract_json_string(const char* begin, const char* end,
                               const char* key, char* out, size_t outlen) {
    out[0] = '\0';
    const char* p = find_key(begin, end, key);
    if (!p || *p != '"') return 0;
    const char* q = skip_string(p, end);
    if (!q) return 0;
    size_t n = 0;
    for (p++; p < q && n < outlen - 1; p++) {
        char c = *p;
        if (c == '\\' && p + 1 < q) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default:  c = *p;   break;   /* \" \\ \/ (e \u copiato cosi' com'e') */
            }
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return 1;
}

/* Legge tutti gli oggetti dell'array associato a "section" (es. "points").
 * Per ogni oggetto estrae ref/key/fc e li scrive nei campi indicati.
 * Ritorna il numero di elementi caricati, o -1 se l'array e' malformato. */
static int load_section(const char* buf, const char* buf_end, const char* section,
                        char (*refs)[256], char (*keys)[128], FunctionalConstraint* fcs,
                        size_t stride, int max)
{
    /* L'array deve trovarsi al primo livello dell'oggetto radice */
    const char* root = strchr(buf, '{');
    if (!root) return -1;
    const char* root_end = find_matching(root, buf_end);
    if (!root_end) return -1;

    const char* arr = find_key(root, root_end, section);
    if (!arr) return 0;                         /* sezione assente: nessun elemento */
    if (*arr != '[') return -1;
    const char* arr_end = find_matching(arr, root_end);
    if (!arr_end) return -1;

    int n = 0;
    const char* p = arr + 1;
    while (p < arr_end && n < max) {
        /* Prossimo oggetto { ... } dell'array */
        while (p < arr_end && *p != '{') p++;
        if (p >= arr_end) break;
        const char* obj_end = find_matching(p, arr_end + 1);
        if (!obj_end) return -1;

        /* I campi sono raggiunti tramite stride perche' DataPoint e
         * ReportBlockRef sono due array di struct diversi */
        char* ref = (char*)refs + n * stride;
        char* key = (char*)keys + n * stride;
        FunctionalConstraint* fc = (FunctionalConstraint*)((char*)fcs + n * stride);

        if (!extract_json_string(p, obj_end + 1, "ref", ref, 256) || !ref[0]) {
            fprintf(stderr, "[client] elemento %d di \"%s\" senza \"ref\", ignorato\n", n, section);
        } else {
            char fcbuf[8];
            /* Se manca "key" si usa il riferimento stesso come chiave JSON */
            if (!extract_json_string(p, obj_end + 1, "key", key, 128) || !key[0])
                snprintf(key, 128, "%s", ref);
            extract_json_string(p, obj_end + 1, "fc", fcbuf, sizeof(fcbuf));
            *fc = parse_fc(fcbuf);
            n++;
        }
        p = obj_end + 1;
    }
    if (n == max)
        fprintf(stderr, "[client] \"%s\": raggiunto il limite di %d elementi\n", section, max);
    return n;
}

/* Legge la sezione "goose": per ogni oggetto ref (gocbRef), key e appid.
 * Ritorna il numero di GoCB caricati, o -1 se l'array e' malformato. */
static int load_goose_section(const char* buf, const char* buf_end,
                              GooseSubRef* subs, int max)
{
    const char* root = strchr(buf, '{');
    if (!root) return -1;
    const char* root_end = find_matching(root, buf_end);
    if (!root_end) return -1;

    const char* arr = find_key(root, root_end, "goose");
    if (!arr) return 0;
    if (*arr != '[') return -1;
    const char* arr_end = find_matching(arr, root_end);
    if (!arr_end) return -1;

    int n = 0;
    const char* p = arr + 1;
    while (p < arr_end && n < max) {
        while (p < arr_end && *p != '{') p++;
        if (p >= arr_end) break;
        const char* obj_end = find_matching(p, arr_end + 1);
        if (!obj_end) return -1;

        GooseSubRef* s = &subs[n];
        if (!extract_json_string(p, obj_end + 1, "ref", s->gocbRef, sizeof(s->gocbRef))
            || !s->gocbRef[0]) {
            fprintf(stderr, "[client] elemento %d di \"goose\" senza \"ref\", ignorato\n", n);
        } else {
            char appid[16];
            if (!extract_json_string(p, obj_end + 1, "key", s->jsonKey, sizeof(s->jsonKey))
                || !s->jsonKey[0])
                snprintf(s->jsonKey, sizeof(s->jsonKey), "%s", s->gocbRef);
            /* APPID sempre esadecimale, con o senza "0x" (come nell'SCL) */
            s->appid = extract_json_string(p, obj_end + 1, "appid", appid, sizeof(appid))
                       && appid[0] ? (int)strtol(appid, NULL, 16) : -1;
            n++;
        }
        p = obj_end + 1;
    }
    if (n == max)
        fprintf(stderr, "[client] \"goose\": raggiunto il limite di %d elementi\n", max);
    return n;
}

/* Carica config.json in memoria e popola points[], rbs[] e gsubs[]. Ritorna 1 se ok. */
static int load_config(const char* filename,
                       DataPoint* points, int* n_points,
                       ReportBlockRef* rbs, int* n_rbs,
                       GooseSubRef* gsubs, int* n_gsubs)
{
    *n_points = 0;
    *n_rbs = 0;
    *n_gsubs = 0;

    /* Lettura dell'intero file in un buffer terminato da '\0' */
    FILE* fp = fopen(filename, "rb");
    if (!fp) { perror("config"); return 0; }
    if (fseek(fp, 0, SEEK_END) != 0) { perror("config"); fclose(fp); return 0; }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { perror("config"); fclose(fp); return 0; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fprintf(stderr, "config: memoria insufficiente\n"); fclose(fp); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        fprintf(stderr, "config: letti %zu byte su %ld\n", rd, sz);
        free(buf);
        return 0;
    }
    buf[sz] = '\0';

    int np = load_section(buf, buf + sz, "points",
                          &points[0].objectRef, &points[0].jsonKey, &points[0].fc,
                          sizeof(DataPoint), MAX_POINTS);
    int nr = load_section(buf, buf + sz, "report_blocks",
                          &rbs[0].objectRef, &rbs[0].jsonKey, &rbs[0].fc,
                          sizeof(ReportBlockRef), MAX_POINTS);
    int ng = load_goose_section(buf, buf + sz, gsubs, MAX_GOOSE_SUBS);
    free(buf);

    if (np < 0 || nr < 0 || ng < 0) {
        fprintf(stderr, "config: JSON malformato\n");
        return 0;
    }
    *n_points = np;
    *n_rbs = nr;
    *n_gsubs = ng;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Output JSON                                                         */
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

/* Legge un oggetto e restituisce il valore come stringa JSON.
 * Scrive su fp una riga del tipo:
 *     "chiave": {"value": <valore>, "type": "<tipo>"}
 * oppure, in caso di errore di lettura:
 *     "chiave": {"value": null, "error": <codice IedClientError>}
 * is_last evita la virgola finale sull'ultimo elemento. */
static void read_point_json(IedConnection con, IedClientError* err,
                            const DataPoint* dp, FILE* fp, int is_last)
{
    /* Lettura MMS sincrona dell'oggetto con il suo Functional Constraint */
    MmsValue* value = IedConnection_readObject(con, err, dp->objectRef, dp->fc);
    fputs("    ", fp);
    fprint_json_string(fp, dp->jsonKey);
    fputs(": ", fp);

    if (value == NULL || *err != IED_ERROR_OK) {
        fprintf(fp, "{\"value\": null, \"error\": %d}", *err);
        if (value) MmsValue_delete(value);
    } else {
        /* Conversione del valore MMS in base al tipo restituito dall'IED */
        MmsType t = MmsValue_getType(value);
        switch (t) {
            case MMS_FLOAT:
                fprintf(fp, "{\"value\": %.6f, \"type\": \"float\"}", MmsValue_toFloat(value));
                break;
            case MMS_INTEGER:
                /* Include gli enumerati (es. AutoRecSt, Beh, Health) */
                fprintf(fp, "{\"value\": %lld, \"type\": \"int\"}",
                        (long long)MmsValue_toInt64(value));
                break;
            case MMS_UNSIGNED:
                fprintf(fp, "{\"value\": %lu, \"type\": \"uint\"}",
                        (unsigned long)MmsValue_toUint32(value));
                break;
            case MMS_BOOLEAN:
                fprintf(fp, "{\"value\": %s, \"type\": \"bool\"}",
                        MmsValue_getBoolean(value) ? "true" : "false");
                break;
            case MMS_BIT_STRING:
                if (MmsValue_getBitStringSize(value) == 2) {
                    /* Bit string di 2 bit = Dbpos (es. XCBR.Pos.stVal):
                     * 0 intermedio, 1 aperto/off, 2 chiuso/on, 3 bad */
                    Dbpos d = Dbpos_fromMmsValue(value);
                    fprintf(fp, "{\"value\": %d, \"type\": \"dbpos\", \"label\": \"%s\"}",
                            (int)d, dbpos_name(d));
                } else {
                    /* Altre bit string (es. Quality): valore intero, bit 0 = MSB */
                    fprintf(fp, "{\"value\": %u, \"type\": \"bitstring\", \"bits\": %d}",
                            MmsValue_getBitStringAsIntegerBigEndian(value),
                            MmsValue_getBitStringSize(value));
                }
                break;
            case MMS_VISIBLE_STRING:
            case MMS_STRING:
                fputs("{\"value\": ", fp);
                fprint_json_string(fp, MmsValue_toString(value));
                fputs(", \"type\": \"string\"}", fp);
                break;
            case MMS_UTC_TIME: {
                /* Timestamp IEC 61850 convertito in millisecondi da epoch */
                uint64_t ms = MmsValue_getUtcTimeInMs(value);
                fprintf(fp, "{\"value\": %llu, \"type\": \"utc_time\"}",
                        (unsigned long long)ms);
                break;
            }
            case MMS_DATA_ACCESS_ERROR:
                /* La richiesta e' andata a buon fine ma l'IED ha rifiutato l'accesso
                 * all'oggetto (es. 10 = oggetto inesistente) */
                fprintf(fp, "{\"value\": null, \"access_error\": %d}",
                        (int)MmsValue_getDataAccessError(value));
                break;
            default:
                /* Tipi non gestiti (es. STRUCTURE): si riporta il codice MmsType */
                fprintf(fp, "{\"value\": null, \"type\": \"unsupported_%d\"}", t);
                break;
        }
        MmsValue_delete(value);  /* il valore e' allocato dalla libreria: va liberato */
    }
    fprintf(fp, "%s\n", is_last ? "" : ",");
}

/* ------------------------------------------------------------------ */
/* GOOSE                                                               */
/* ------------------------------------------------------------------ */

/* Valore MMS generico in JSON (usato per i dataset GOOSE): tipi semplici come
 * valori JSON, strutture e array come array JSON, bit string come stringa di
 * '0'/'1' (bit 0 a sinistra, es. Dbpos "10" = on), tempi in ms da epoch. */
static void fprint_mms_json(FILE* fp, MmsValue* v)
{
    if (!v) { fputs("null", fp); return; }
    switch (MmsValue_getType(v)) {
        case MMS_STRUCTURE:
        case MMS_ARRAY: {
            int n = (int)MmsValue_getArraySize(v);
            fputc('[', fp);
            for (int i = 0; i < n; i++) {
                if (i) fputs(", ", fp);
                fprint_mms_json(fp, MmsValue_getElement(v, i));
            }
            fputc(']', fp);
            break;
        }
        case MMS_BOOLEAN:
            fputs(MmsValue_getBoolean(v) ? "true" : "false", fp);
            break;
        case MMS_INTEGER:
            fprintf(fp, "%lld", (long long)MmsValue_toInt64(v));
            break;
        case MMS_UNSIGNED:
            fprintf(fp, "%lu", (unsigned long)MmsValue_toUint32(v));
            break;
        case MMS_FLOAT: {
            double d = MmsValue_toDouble(v);
            if (isfinite(d)) fprintf(fp, "%.9g", d);
            else fputs("null", fp);              /* NaN/Inf non sono JSON valido */
            break;
        }
        case MMS_BIT_STRING:
            fputc('"', fp);
            for (int i = 0; i < MmsValue_getBitStringSize(v); i++)
                fputc(MmsValue_getBitStringBit(v, i) ? '1' : '0', fp);
            fputc('"', fp);
            break;
        case MMS_OCTET_STRING: {
            uint8_t* b = MmsValue_getOctetStringBuffer(v);
            fputc('"', fp);
            for (int i = 0; i < MmsValue_getOctetStringSize(v); i++)
                fprintf(fp, "%02x", b[i]);
            fputc('"', fp);
            break;
        }
        case MMS_VISIBLE_STRING:
        case MMS_STRING:
            fprint_json_string(fp, MmsValue_toString(v));
            break;
        case MMS_UTC_TIME:
            fprintf(fp, "%llu", (unsigned long long)MmsValue_getUtcTimeInMs(v));
            break;
        case MMS_BINARY_TIME:
            fprintf(fp, "%llu", (unsigned long long)MmsValue_getBinaryTimeAsUtcMs(v));
            break;
        default:
            fputs("null", fp);
            break;
    }
}

/* Un flusso GOOSE (un GoCB) e le sue informazioni fisse */
typedef struct {
    char key[130];              /* chiave nel JSON: key da config, o gocbRef */
    char gocbRef[130];
    char goId[130];
    char dataSet[130];
    int appid;                  /* -1 = non noto */
    int vlanId;                 /* -1 = frame senza tag VLAN */
    char srcMac[18], dstMac[18];
    bool seen;                  /* almeno un frame ricevuto */
} GooseStream;

/* Un frame GOOSE ricevuto */
typedef struct {
    int stream;                 /* indice in GooseCapture.streams */
    double rx_ms;               /* istante di ricezione, ms da epoch */
    uint32_t stNum, sqNum, ttl, confRev;
    uint64_t t_ms;              /* timestamp "t" del GOOSE: ultimo cambio di stato */
    bool valid, test, ndsCom;
    char* values;               /* allData in JSON (malloc) */
} GooseFrameRec;

/* Stato della ricezione. In tempo reale e' scritto solo dal thread del
 * GooseReceiver e letto dal main solo dopo GooseReceiver_stop(), che
 * attende la fine del thread: non serve un mutex. */
typedef struct {
    GooseStream streams[MAX_GOOSE_STREAMS];
    int nStreams;
    GooseFrameRec* frames;
    int nFrames;
    long dropped;               /* frame non registrati (limiti superati) */
    double replay_ms;           /* analisi pcap: istante del frame in corso; <0 = tempo reale */
} GooseCapture;

/* Parametro della callback: flusso associato al subscriber (-1 = observer) */
typedef struct {
    GooseCapture* cap;
    int stream;
} GooseListenerCtx;

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

/* Callback chiamata da libiec61850 per ogni frame GOOSE ricevuto.
 * In tempo reale gira sul thread del GooseReceiver: deve essere breve.
 * I valori del dataset vanno usati solo qui (la libreria li riusa al frame
 * successivo), per questo vengono subito convertiti in testo JSON. */
static void goose_listener(GooseSubscriber sub, void* param)
{
    GooseListenerCtx* ctx = (GooseListenerCtx*)param;
    GooseCapture* cap = ctx->cap;
    double rx = cap->replay_ms >= 0 ? cap->replay_ms : now_epoch_ms();
    const char* ref = GooseSubscriber_getGoCbRef(sub);
    if (!ref) ref = "";

    int s = ctx->stream;
    if (s < 0) {
        /* Observer: il flusso e' individuato dal gocbRef contenuto nel frame */
        for (s = 0; s < cap->nStreams; s++)
            if (strcmp(cap->streams[s].gocbRef, ref) == 0) break;
        if (s == cap->nStreams) {
            if (cap->nStreams == MAX_GOOSE_STREAMS) { cap->dropped++; return; }
            GooseStream* ns = &cap->streams[cap->nStreams++];
            snprintf(ns->key, sizeof(ns->key), "%s", ref);
            snprintf(ns->gocbRef, sizeof(ns->gocbRef), "%s", ref);
            ns->appid = -1;
        }
    }

    GooseStream* st = &cap->streams[s];
    if (!st->seen) {
        /* Informazioni fisse del flusso, prese dal primo frame */
        uint8_t mac[6];
        const char* id = GooseSubscriber_getGoId(sub);
        const char* ds = GooseSubscriber_getDataSet(sub);
        snprintf(st->goId, sizeof(st->goId), "%s", id ? id : "");
        snprintf(st->dataSet, sizeof(st->dataSet), "%s", ds ? ds : "");
        st->appid = GooseSubscriber_getAppId(sub);
        st->vlanId = GooseSubscriber_isVlanSet(sub) ? GooseSubscriber_getVlanId(sub) : -1;
        GooseSubscriber_getSrcMac(sub, mac);
        mac_to_str(mac, st->srcMac);
        GooseSubscriber_getDstMac(sub, mac);
        mac_to_str(mac, st->dstMac);
        st->seen = true;
    }

    if (cap->nFrames >= MAX_GOOSE_FRAMES) { cap->dropped++; return; }
    GooseFrameRec* f = &cap->frames[cap->nFrames++];
    f->stream = s;
    f->rx_ms = rx;
    f->stNum = GooseSubscriber_getStNum(sub);
    f->sqNum = GooseSubscriber_getSqNum(sub);
    f->ttl = GooseSubscriber_getTimeAllowedToLive(sub);
    f->confRev = GooseSubscriber_getConfRev(sub);
    f->t_ms = GooseSubscriber_getTimestamp(sub);
    f->valid = GooseSubscriber_isValid(sub);
    f->test = GooseSubscriber_isTest(sub);
    f->ndsCom = GooseSubscriber_needsCommission(sub);

    /* allData -> stringa JSON in memoria */
    size_t len = 0;
    f->values = NULL;
    FILE* mem = open_memstream(&f->values, &len);
    if (mem) {
        fprint_mms_json(mem, GooseSubscriber_getDataSetValues(sub));
        fclose(mem);
    }
}

/* Crea il ricevitore: un subscriber per ogni GoCB della configurazione, o un
 * solo subscriber in modo observer (tutti i GOOSE) se la lista e' vuota.
 * ctx deve restare valido finche' il ricevitore esiste. */
static GooseReceiver goose_setup(GooseCapture* cap, GooseSubRef* subs, int nsubs,
                                 GooseListenerCtx* ctx)
{
    GooseReceiver rx = GooseReceiver_create();
    if (nsubs == 0) {
        static char any[] = "";
        GooseSubscriber s = GooseSubscriber_create(any, NULL);
        GooseSubscriber_setObserver(s);
        ctx[0].cap = cap;
        ctx[0].stream = -1;
        GooseSubscriber_setListener(s, goose_listener, &ctx[0]);
        GooseReceiver_addSubscriber(rx, s);
        return rx;
    }
    for (int i = 0; i < nsubs; i++) {
        GooseStream* st = &cap->streams[i];
        snprintf(st->key, sizeof(st->key), "%s", subs[i].jsonKey);
        snprintf(st->gocbRef, sizeof(st->gocbRef), "%s", subs[i].gocbRef);
        st->appid = subs[i].appid;
        st->vlanId = -1;

        GooseSubscriber s = GooseSubscriber_create(subs[i].gocbRef, NULL);
        if (subs[i].appid >= 0)
            GooseSubscriber_setAppId(s, (uint16_t)subs[i].appid);
        ctx[i].cap = cap;
        ctx[i].stream = i;
        GooseSubscriber_setListener(s, goose_listener, &ctx[i]);
        GooseReceiver_addSubscriber(rx, s);
    }
    cap->nStreams = nsubs;
    return rx;
}

/* Lettura di interi little/big endian da un buffer */
static uint32_t rd32(const uint8_t* p, bool be)
{
    return be ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}

static uint16_t rd16(const uint8_t* p, bool be)
{
    return be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}

/* Analisi offline: legge una cattura .pcap o .pcapng e passa ogni frame
 * Ethernet al decoder GOOSE di libiec61850 (GooseReceiver_handleMessage),
 * usando come istante di ricezione il timestamp della cattura.
 * Ritorna il numero di pacchetti letti, -1 in caso di errore (msg in err). */
static long goose_replay_pcap(const char* path, GooseReceiver rx, GooseCapture* cap,
                              char* err, size_t errlen)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errlen, "impossibile aprire %s", path); return -1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* b = sz > 0 ? malloc((size_t)sz) : NULL;
    if (!b || fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        snprintf(err, errlen, "lettura di %s fallita", path);
        fclose(fp); free(b); return -1;
    }
    fclose(fp);

    long packets = 0, not_eth = 0;
    uint32_t magic = sz >= 4 ? rd32(b, false) : 0;

    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d ||
        (sz >= 4 && (rd32(b, true) == 0xa1b2c3d4 || rd32(b, true) == 0xa1b23c4d))) {
        /* --- pcap classico: header di 24 byte + record (16 byte + dati) --- */
        bool be = !(magic == 0xa1b2c3d4 || magic == 0xa1b23c4d);
        bool nano = rd32(b, be) == 0xa1b23c4d;
        if (sz < 24) { snprintf(err, errlen, "pcap troppo corto"); free(b); return -1; }
        if (rd32(b + 20, be) != 1) {
            snprintf(err, errlen, "link type %u non Ethernet", rd32(b + 20, be));
            free(b); return -1;
        }
        long off = 24;
        while (off + 16 <= sz) {
            uint32_t sec = rd32(b + off, be), frac = rd32(b + off + 4, be);
            uint32_t caplen = rd32(b + off + 8, be);
            if (off + 16 + (long)caplen > sz) break;
            cap->replay_ms = sec * 1000.0 + frac / (nano ? 1e6 : 1e3);
            GooseReceiver_handleMessage(rx, b + off + 16, (int)caplen);
            packets++;
            off += 16 + caplen;
        }
    } else if (magic == 0x0a0d0d0a) {
        /* --- pcapng: blocchi SHB (sezione), IDB (interfaccia), EPB (pacchetto) --- */
        double resol[32];              /* secondi per unita' di timestamp, per interfaccia */
        uint16_t linktype[32];
        int nif = 0;
        bool be = false;
        long off = 0;
        while (off + 12 <= sz) {
            uint32_t type = rd32(b + off, be);
            if (type == 0x0a0d0d0a) {
                /* Nuova sezione: ordine dei byte dal byte-order magic */
                be = rd32(b + off + 8, false) != 0x1a2b3c4d;
                nif = 0;
            }
            uint32_t blen = rd32(b + off + 4, be);
            if (blen < 12 || off + (long)blen > sz) break;
            const uint8_t* blk = b + off;

            if (type == 1 && nif < 32) {
                /* IDB: link type e risoluzione dei timestamp (if_tsresol, default 1e-6) */
                linktype[nif] = rd16(blk + 8, be);
                resol[nif] = 1e-6;
                long o = 16;
                while (o + 4 <= (long)blen - 4) {
                    uint16_t code = rd16(blk + o, be), olen = rd16(blk + o + 2, be);
                    if (code == 0) break;
                    if (code == 9 && olen >= 1) {
                        /* bit 7 = 0: 10^-r secondi (es. 6 = us, 9 = ns); 1: 2^-r */
                        uint8_t r = blk[o + 4];
                        double base = (r & 0x80) ? 2.0 : 10.0;
                        resol[nif] = 1.0;
                        for (int k = 0; k < (r & 0x7f); k++) resol[nif] /= base;
                    }
                    o += 4 + ((olen + 3) & ~3);
                }
                nif++;
            } else if (type == 6) {
                /* EPB: interfaccia, timestamp a 64 bit, lunghezza catturata, dati */
                uint32_t ifid = rd32(blk + 8, be);
                uint64_t ts = (uint64_t)rd32(blk + 12, be) << 32 | rd32(blk + 16, be);
                uint32_t caplen = rd32(blk + 20, be);
                if (ifid < (uint32_t)nif && 28 + caplen <= blen) {
                    if (linktype[ifid] == 1) {
                        cap->replay_ms = (double)ts * resol[ifid] * 1000.0;
                        GooseReceiver_handleMessage(rx, (uint8_t*)blk + 28, (int)caplen);
                    } else {
                        not_eth++;
                    }
                    packets++;
                }
            }
            off += blen;
        }
    } else {
        snprintf(err, errlen, "%s non e' un file pcap/pcapng", path);
        free(b);
        return -1;
    }
    free(b);
    if (not_eth)
        fprintf(stderr, "[goose] %ld pacchetti ignorati (link type non Ethernet)\n", not_eth);
    return packets;
}

/* Scrive la sezione "goose" del JSON: statistiche per flusso e frame.
 * Statistiche (per flusso, nell'ordine di ricezione):
 *   stnum_changes   cambi di stNum (eventi)
 *   sqnum_resets    cambi di stNum con sqNum ripartito da 0 o 1
 *   max_interval_ms intervallo massimo tra due frame consecutivi
 *   ttl_violations  frame arrivati oltre il TTL dichiarato dal precedente */
static void write_goose_json(FILE* fp, const GooseCapture* cap, const char* mode,
                             const char* source, double listen_ms, const char* error)
{
    fprintf(fp, "  \"goose\": {\n");
    fprintf(fp, "    \"mode\": \"%s\",\n", mode);
    fputs("    \"source\": ", fp);
    fprint_json_string(fp, source);
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
        int n = 0, stch = 0, sqres = 0, ttlv = 0, invalid = 0;
        double maxint = -1, first = 0, last = 0;
        const GooseFrameRec* prev = NULL;
        for (int i = 0; i < cap->nFrames; i++) {
            const GooseFrameRec* f = &cap->frames[i];
            if (f->stream != s) continue;
            if (!f->valid) invalid++;
            if (prev) {
                double dt = f->rx_ms - prev->rx_ms;
                if (dt > maxint) maxint = dt;
                if (prev->ttl > 0 && dt > prev->ttl) ttlv++;
                if (f->stNum != prev->stNum) {
                    stch++;
                    if (f->sqNum <= 1) sqres++;
                }
            } else {
                first = f->rx_ms;
            }
            last = f->rx_ms;
            prev = f;
            n++;
        }
        fputs("      ", fp);
        fprint_json_string(fp, st->key);
        fputs(": {\"gocb_ref\": ", fp);
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
        fprintf(fp, ", \"frames\": %d", n);
        if (n) {
            fprintf(fp, ", \"first_rx_ms\": %.3f, \"last_rx_ms\": %.3f", first, last);
            fprintf(fp, ", \"stnum_changes\": %d, \"sqnum_resets\": %d", stch, sqres);
            if (maxint >= 0) fprintf(fp, ", \"max_interval_ms\": %.3f", maxint);
            fprintf(fp, ", \"ttl_violations\": %d, \"invalid_frames\": %d", ttlv, invalid);
        }
        fprintf(fp, "}%s\n", s == cap->nStreams - 1 ? "" : ",");
    }
    fprintf(fp, "    },\n");

    fprintf(fp, "    \"frames\": [\n");
    for (int i = 0; i < cap->nFrames; i++) {
        const GooseFrameRec* f = &cap->frames[i];
        fputs("      {\"stream\": ", fp);
        fprint_json_string(fp, cap->streams[f->stream].key);
        fprintf(fp, ", \"rx_ms\": %.3f, \"st_num\": %u, \"sq_num\": %u, \"ttl_ms\": %u"
                    ", \"t_ms\": %llu, \"conf_rev\": %u, \"valid\": %s, \"test\": %s"
                    ", \"nds_com\": %s, \"values\": %s}%s\n",
                f->rx_ms, f->stNum, f->sqNum, f->ttl, (unsigned long long)f->t_ms,
                f->confRev, f->valid ? "true" : "false", f->test ? "true" : "false",
                f->ndsCom ? "true" : "false", f->values ? f->values : "null",
                i == cap->nFrames - 1 ? "" : ",");
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
    /* --- Argomenti da riga di comando --- */
    if (argc < 5) {
        fprintf(stderr,
            "Uso: %s <host|-> <port> <config.json> <output.json>\n"
            "        [--goose <interfaccia>] [--goose-time <secondi>] [--goose-pcap <file>]\n",
            argv[0]);
        return 1;
    }
    const char* host = argv[1];
    int port = atoi(argv[2]);       /* porta MMS, di norma 102 */
    const char* cfgfile = argv[3];
    const char* outfile = argv[4];
    bool use_mms = strcmp(host, "-") != 0;   /* "-" = solo GOOSE, nessuna connessione MMS */

    const char* goose_if = NULL;    /* ricezione in tempo reale */
    const char* goose_pcap = NULL;  /* analisi offline di una cattura */
    double goose_time_s = 5.0;      /* durata minima dell'ascolto in tempo reale */
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--goose") == 0 && i + 1 < argc)
            goose_if = argv[++i];
        else if (strcmp(argv[i], "--goose-time") == 0 && i + 1 < argc)
            goose_time_s = atof(argv[++i]);
        else if (strcmp(argv[i], "--goose-pcap") == 0 && i + 1 < argc)
            goose_pcap = argv[++i];
        else {
            fprintf(stderr, "Opzione non valida: %s\n", argv[i]);
            return 1;
        }
    }
    if (goose_if && goose_pcap) {
        fprintf(stderr, "--goose e --goose-pcap sono alternativi\n");
        return 1;
    }

    /* --- Caricamento configurazione --- */
    /* static: ~200 KB, meglio fuori dallo stack; azzerati all'avvio */
    static DataPoint points[MAX_POINTS];
    static ReportBlockRef rbs[MAX_POINTS];
    static GooseSubRef gsubs[MAX_GOOSE_SUBS];
    int n_points = 0, n_rbs = 0, n_gsubs = 0;

    if (!load_config(cfgfile, points, &n_points, rbs, &n_rbs, gsubs, &n_gsubs)) {
        fprintf(stderr, "Errore lettura config %s\n", cfgfile);
        return 1;
    }
    fprintf(stderr, "[client] %d data points, %d report blocks, %d GoCB GOOSE\n",
            n_points, n_rbs, n_gsubs);

    /* --- Ricezione GOOSE in tempo reale: parte PRIMA della connessione MMS,
     *     cosi' i frame pubblicati durante le letture vengono registrati --- */
    static GooseCapture gcap;                       /* static: ~40 KB di flussi */
    static GooseListenerCtx gctx[MAX_GOOSE_SUBS];
    GooseReceiver grx = NULL;
    char goose_err[256] = "";
    char goose_src[300] = "";
    bool goose_on = goose_if || goose_pcap;
    struct timespec t_goose_start, t_goose_stop;

    if (goose_on) {
        gcap.frames = calloc(MAX_GOOSE_FRAMES, sizeof(GooseFrameRec));
        if (!gcap.frames) { fprintf(stderr, "memoria insufficiente\n"); return 1; }
        gcap.replay_ms = -1;
    }
    if (goose_if) {
        snprintf(goose_src, sizeof(goose_src), "live:%s", goose_if);
        grx = goose_setup(&gcap, gsubs, n_gsubs, gctx);
        GooseReceiver_setInterfaceId(grx, goose_if);
        GooseReceiver_start(grx);
        clock_gettime(CLOCK_MONOTONIC, &t_goose_start);
        if (!GooseReceiver_isRunning(grx)) {
            snprintf(goose_err, sizeof(goose_err),
                     "ricezione non avviata su %s: servono root o CAP_NET_RAW, "
                     "oppure l'interfaccia non esiste", goose_if);
            fprintf(stderr, "[goose] %s\n", goose_err);
        } else {
            fprintf(stderr, "[goose] in ascolto su %s (%s)\n", goose_if,
                    n_gsubs ? "subscriber" : "observer: tutti i GOOSE");
        }
    }

    /* Timestamp di inizio per misura latenza */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* --- Connessione MMS all'IED --- */
    IedClientError err = IED_ERROR_OK;
    IedConnection con = NULL;
    if (use_mms) {
        con = IedConnection_create();
        IedConnection_connect(con, &err, host, port);
        if (err != IED_ERROR_OK) {
            fprintf(stderr, "[client] connessione fallita a %s:%d (err=%d)\n", host, port, err);
            IedConnection_destroy(con);
            if (grx) GooseReceiver_destroy(grx);   /* ferma anche il thread GOOSE */
            return 2;
        }
        fprintf(stderr, "[client] connesso a %s:%d\n", host, port);
    }

    /* Timestamp a connessione stabilita: chiude la misura di latenza di connessione
     * e apre quella del tempo di lettura */
    struct timespec t_connected;
    clock_gettime(CLOCK_MONOTONIC, &t_connected);

    FILE* fp = fopen(outfile, "w");
    if (!fp) {
        perror("output");
        if (con) { IedConnection_close(con); IedConnection_destroy(con); }
        if (grx) GooseReceiver_destroy(grx);
        return 3;
    }

    /* Corpo JSON: intestazione con metadati della sessione */
    fprintf(fp, "{\n");
    fputs("  \"host\": ", fp);
    if (use_mms) fprint_json_string(fp, host); else fputs("null", fp);
    fputs(",\n", fp);
    if (use_mms) fprintf(fp, "  \"port\": %d,\n", port);
    else fputs("  \"port\": null,\n", fp);
    fprintf(fp, "  \"timestamp_wall\": %ld,\n", (long)time(NULL));   /* epoch in secondi */
    if (use_mms) fprintf(fp, "  \"connect_latency_ms\": %.3f,\n", elapsed_ms(&t_start, &t_connected));
    else fputs("  \"connect_latency_ms\": null,\n", fp);

    /* Sezione "values": un elemento per ogni data point */
    fprintf(fp, "  \"values\": {\n");
    for (int i = 0; use_mms && i < n_points; i++) {
        read_point_json(con, &err, &points[i], fp, i == n_points - 1);
    }
    fprintf(fp, "  },\n");

    /* Report Control Blocks: verifica presenza e attributi base */
    fprintf(fp, "  \"report_blocks\": {\n");
    for (int i = 0; use_mms && i < n_rbs; i++) {
        fputs("    ", fp);
        fprint_json_string(fp, rbs[i].jsonKey);
        fputs(": ", fp);
        /* Si legge solo l'attributo RptEna (report abilitato si'/no).
         * Il FC viene passato a parte, quindi un eventuale segmento ".RP." / ".BR."
         * nel riferimento (stile MMS, es. "LLN0.RP.Urcb01") va rimosso:
         * il riferimento corretto e' "LLN0.Urcb01". */
        char ref[256];
        snprintf(ref, sizeof(ref), "%s", rbs[i].objectRef);
        char* seg = strstr(ref, ".RP.");
        if (!seg) seg = strstr(ref, ".BR.");
        if (seg) memmove(seg, seg + 3, strlen(seg + 3) + 1);
        char attr[300];
        snprintf(attr, sizeof(attr), "%s.RptEna", ref);
        MmsValue* v = IedConnection_readObject(con, &err, attr, rbs[i].fc);
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
        fprintf(fp, "%s\n", i == n_rbs - 1 ? "" : ",");
    }
    fprintf(fp, "  },\n");

    /* Tempo totale impiegato per tutte le letture (dopo la connessione) */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    if (use_mms) fprintf(fp, "  \"total_read_time_ms\": %.3f", elapsed_ms(&t_connected, &t_end));
    else fputs("  \"total_read_time_ms\": null", fp);

    /* --- Fine ricezione GOOSE --- */
    if (goose_if) {
        /* Ascolto per almeno goose_time_s dall'avvio (anche oltre le letture MMS) */
        if (GooseReceiver_isRunning(grx)) {
            struct timespec now, pause = {0, 20 * 1000000L};
            clock_gettime(CLOCK_MONOTONIC, &now);
            while (elapsed_ms(&t_goose_start, &now) < goose_time_s * 1000.0) {
                nanosleep(&pause, NULL);
                clock_gettime(CLOCK_MONOTONIC, &now);
            }
            GooseReceiver_stop(grx);    /* attende la fine del thread di ricezione */
        }
        clock_gettime(CLOCK_MONOTONIC, &t_goose_stop);
        fputs(",\n", fp);
        write_goose_json(fp, &gcap, n_gsubs ? "subscriber" : "observer", goose_src,
                         goose_err[0] ? -1 : elapsed_ms(&t_goose_start, &t_goose_stop),
                         goose_err);
        fprintf(stderr, "[goose] %d frame registrati in %d flussi\n", gcap.nFrames, gcap.nStreams);
    } else if (goose_pcap) {
        /* Analisi offline: stesso decoder e stessa callback, frame dal file */
        snprintf(goose_src, sizeof(goose_src), "pcap:%s", goose_pcap);
        grx = goose_setup(&gcap, gsubs, n_gsubs, gctx);
        long np = goose_replay_pcap(goose_pcap, grx, &gcap, goose_err, sizeof(goose_err));
        if (np < 0) fprintf(stderr, "[goose] %s\n", goose_err);
        else fprintf(stderr, "[goose] %ld pacchetti letti da %s, %d frame GOOSE in %d flussi\n",
                     np, goose_pcap, gcap.nFrames, gcap.nStreams);
        fputs(",\n", fp);
        write_goose_json(fp, &gcap, n_gsubs ? "subscriber" : "observer", goose_src, -1,
                         goose_err);
    }

    fprintf(fp, "\n}\n");
    fclose(fp);

    fprintf(stderr, "[client] dati salvati in %s\n", outfile);

    /* --- Chiusura connessione e rilascio risorse --- */
    if (con) {
        IedConnection_close(con);
        IedConnection_destroy(con);
    }
    if (grx) GooseReceiver_destroy(grx);            /* libera anche i subscriber */
    if (goose_on) {
        for (int i = 0; i < gcap.nFrames; i++) free(gcap.frames[i].values);
        free(gcap.frames);
    }
    return 0;
}
