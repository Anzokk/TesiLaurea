/*
 * ParserMMSV2.c
 * Client MMS per BU9020 - componente di verifica automatica.
 *
 * Uso:
 *   bin/ParserMMSV2 <host> <port> <config.json> <output.json>
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
 *   ]
 * }
 * "key" e "fc" sono opzionali (default: key = ref, fc = MX).
 *
 * Flusso generale:
 *   1. legge config.json e costruisce la lista di data point e di report block;
 *   2. si connette all'IED via MMS (libiec61850);
 *   3. legge ogni data point e ne scrive valore/tipo in output.json;
 *   4. per ogni Report Control Block legge l'attributo RptEna;
 *   5. annota nel JSON le latenze di connessione e di lettura.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "iec61850_client.h"   /* API client di libiec61850 (IedConnection, MmsValue, ...) */

#define MAX_POINTS 256  /* numero massimo di data point / report block caricabili */

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

/* Carica config.json in memoria e popola points[] e rbs[]. Ritorna 1 se ok. */
static int load_config(const char* filename,
                       DataPoint* points, int* n_points,
                       ReportBlockRef* rbs, int* n_rbs)
{
    *n_points = 0;
    *n_rbs = 0;

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
    free(buf);

    if (np < 0 || nr < 0) {
        fprintf(stderr, "config: JSON malformato\n");
        return 0;
    }
    *n_points = np;
    *n_rbs = nr;
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

int main(int argc, char** argv)
{
    /* --- Argomenti da riga di comando --- */
    if (argc < 5) {
        fprintf(stderr,
            "Uso: %s <host> <port> <config.json> <output.json>\n", argv[0]);
        return 1;
    }
    const char* host = argv[1];
    int port = atoi(argv[2]);       /* porta MMS, di norma 102 */
    const char* cfgfile = argv[3];
    const char* outfile = argv[4];

    /* --- Caricamento configurazione --- */
    /* static: ~200 KB, meglio fuori dallo stack; azzerati all'avvio */
    static DataPoint points[MAX_POINTS];
    static ReportBlockRef rbs[MAX_POINTS];
    int n_points = 0, n_rbs = 0;

    if (!load_config(cfgfile, points, &n_points, rbs, &n_rbs)) {
        fprintf(stderr, "Errore lettura config %s\n", cfgfile);
        return 1;
    }
    fprintf(stderr, "[client] %d data points, %d report blocks\n", n_points, n_rbs);

    /* Timestamp di inizio per misura latenza */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* --- Connessione MMS all'IED --- */
    IedClientError err;
    IedConnection con = IedConnection_create();

    IedConnection_connect(con, &err, host, port);
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[client] connessione fallita a %s:%d (err=%d)\n", host, port, err);
        IedConnection_destroy(con);
        return 2;
    }
    fprintf(stderr, "[client] connesso a %s:%d\n", host, port);

    /* Timestamp a connessione stabilita: chiude la misura di latenza di connessione
     * e apre quella del tempo di lettura */
    struct timespec t_connected;
    clock_gettime(CLOCK_MONOTONIC, &t_connected);

    FILE* fp = fopen(outfile, "w");
    if (!fp) { perror("output"); IedConnection_close(con); IedConnection_destroy(con); return 3; }

    /* Corpo JSON: intestazione con metadati della sessione */
    fprintf(fp, "{\n");
    fputs("  \"host\": ", fp);
    fprint_json_string(fp, host);
    fputs(",\n", fp);
    fprintf(fp, "  \"port\": %d,\n", port);
    fprintf(fp, "  \"timestamp_wall\": %ld,\n", (long)time(NULL));   /* epoch in secondi */
    fprintf(fp, "  \"connect_latency_ms\": %.3f,\n",
            (t_connected.tv_sec - t_start.tv_sec) * 1000.0 +
            (t_connected.tv_nsec - t_start.tv_nsec) / 1e6);

    /* Sezione "values": un elemento per ogni data point */
    fprintf(fp, "  \"values\": {\n");
    for (int i = 0; i < n_points; i++) {
        read_point_json(con, &err, &points[i], fp, i == n_points - 1);
    }
    fprintf(fp, "  },\n");

    /* Report Control Blocks: verifica presenza e attributi base */
    fprintf(fp, "  \"report_blocks\": {\n");
    for (int i = 0; i < n_rbs; i++) {
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
    fprintf(fp, "  \"total_read_time_ms\": %.3f\n",
            (t_end.tv_sec - t_connected.tv_sec) * 1000.0 +
            (t_end.tv_nsec - t_connected.tv_nsec) / 1e6);

    fprintf(fp, "}\n");
    fclose(fp);

    fprintf(stderr, "[client] dati salvati in %s\n", outfile);

    /* --- Chiusura connessione e rilascio risorse --- */
    IedConnection_close(con);
    IedConnection_destroy(con);
    return 0;
}