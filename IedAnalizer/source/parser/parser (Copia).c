/*
 * parser.c
 * Parser IEC 61850 per una prova su un singolo IED: in un'unica esecuzione
 *   - legge via MMS i data attribute e lo stato dei Report Control Block;
 *   - riceve in tempo reale i GOOSE del GoCB dell'IED durante le letture;
 * e salva tutto in un file JSON (le verifiche sono demandate ai test Robot).
 * Usa libiec61850 (protocolli) e cJSON (configurazione e output).
 *
 * Uso:
 *   sudo bin/parser [config.json]        (default: config/parser/parser.json)
 * La ricezione GOOSE usa un socket raw: servono root o CAP_NET_RAW.
 *
 * Configurazione (tutti i campi obbligatori):
 * {
 *   "host": "10.145.100.26",          IP dell'IED
 *   "port": 102,                      porta MMS
 *   "interface": "enx503f56005b3e",   interfaccia per i GOOSE
 *   "goose_time_s": 5,                durata minima dell'ascolto GOOSE
 *   "output": "data/results/run.json",
 *   "goose": {"ref": "BU9020CTRL/LLN0$GO$GcbSync", "appid": "0001"},
 *   "points": [
 *     {"ref": "BU9020/MMXU1.TotW.mag.f", "key": "potenza_attiva", "fc": "MX"}
 *   ],
 *   "report_blocks": [
 *     {"ref": "BU9020/LLN0.Urcb01", "key": "rcb_urcb01", "fc": "RP"}
 *   ]
 * }
 * Di un RCB si legge l'attributo RptEna, come un normale punto.
 * GOOSE: si ricevono solo i frame del GoCB indicato (gocbRef e APPID
 * esadecimale), tutti gli altri sono scartati dalla libreria.
 *
 * Output (formato "iedanalizer/3", descritto in docs/parser.md):
 *   nomi in camelCase come in IEC 61850 / SCL / tshark; i campi del protocollo
 *   mantengono il nome dello standard (stNum, timeAllowedtoLive, t, ...), quelli
 *   aggiunti dal parser hanno il suffisso dell'unita' (rxMs, connectMs, ...);
 *   tutti i tempi sono in ms (assoluti = ms da epoch); gli errori hanno tutti
 *   la forma {"kind": ..., "code": n} oppure {"kind": ..., "msg": "..."}.
 *
 * Exit code: 0 = completato, 1 = configurazione non valida, 2 = connessione
 * MMS fallita, 3 = output non scrivibile, 4 = ricezione GOOSE non avviata.
 * In caso di errore non viene scritto nessun file.
 *
 * Flusso generale:
 *   1. legge la configurazione;
 *   2. avvia la ricezione GOOSE (prima della connessione MMS);
 *   3. si connette all'IED, legge i data point e lo stato degli RCB;
 *   4. attende la fine dell'ascolto GOOSE e ferma la ricezione;
 *   5. scrive il JSON con valori MMS e frame GOOSE (le statistiche sui
 *      frame sono calcolate dai test Robot).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <cjson/cJSON.h>       /* configurazione e output JSON */
#include "iec61850_client.h"   /* API client di libiec61850 (IedConnection, MmsValue, ...) */
#include "goose_receiver.h"    /* ricezione GOOSE (GooseReceiver) */
#include "goose_subscriber.h"  /* decodifica di un flusso GOOSE (GooseSubscriber) */

#define DEFAULT_CONFIG     "config/parser/parser.json"
#define OUTPUT_FORMAT      "iedanalizer/3"

/* Parametri di esecuzione (puntano dentro l'albero della configurazione) */
typedef struct {
    const char* host;
    int port;
    const char* iface;
    double gooseTimeS;
    const char* output;
    const char* gocbRef;
    int appid;
    const cJSON* points;
    const cJSON* reportBlocks;
} RunCfg;

/* ------------------------------------------------------------------ */
/* Configurazione                                                      */
/* ------------------------------------------------------------------ */

/* Stringa non vuota del campo k di o, oppure NULL */
static const char* jstr(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

/* Valore numerico positivo del campo k di o, oppure -1 */
static double jnum(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && v->valuedouble > 0 ? v->valuedouble : -1;
}

/* Ogni elemento della lista name deve avere "ref", "key" e un "fc" valido.
 * FunctionalConstraint_fromString guarda solo i primi due caratteri: per
 * questo si controlla anche la lunghezza. */
static bool check_items(const cJSON* items, const char* name)
{
    const cJSON* it;
    int i = 0;
    cJSON_ArrayForEach(it, items) {
        const char* fc = jstr(it, "fc");
        if (!jstr(it, "ref") || !jstr(it, "key") || !fc || strlen(fc) != 2 ||
            FunctionalConstraint_fromString(fc) == IEC61850_FC_NONE) {
            fprintf(stderr, "config: %s[%d]: servono \"ref\", \"key\" e \"fc\" validi\n", name, i);
            return false;
        }
        i++;
    }
    return true;
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
    free(buf);
    if (!cJSON_IsObject(root)) {
        fprintf(stderr, "config: JSON non valido\n");
        cJSON_Delete(root); return NULL;
    }

    const cJSON* goose = cJSON_GetObjectItemCaseSensitive(root, "goose");
    const char* appid = jstr(goose, "appid");
    run->host = jstr(root, "host");
    run->iface = jstr(root, "interface");
    run->output = jstr(root, "output");
    run->port = (int)jnum(root, "port");
    run->gooseTimeS = jnum(root, "goose_time_s");
    run->gocbRef = jstr(goose, "ref");
    run->appid = appid ? (int)strtol(appid, NULL, 16) : -1;   /* esadecimale, come nell'SCL */
    run->points = cJSON_GetObjectItemCaseSensitive(root, "points");
    run->reportBlocks = cJSON_GetObjectItemCaseSensitive(root, "report_blocks");

    if (!run->host || run->port < 0 || !run->iface || run->gooseTimeS < 0 ||
        !run->output || !run->gocbRef || !appid) {
        fprintf(stderr, "config: servono \"host\", \"port\", \"interface\", \"goose_time_s\", "
                        "\"output\" e \"goose\" con \"ref\" e \"appid\"\n");
        cJSON_Delete(root); return NULL;
    }
    if (!check_items(run->points, "points") || !check_items(run->reportBlocks, "report_blocks")) {
        cJSON_Delete(root); return NULL;
    }
    return root;
}

/* ------------------------------------------------------------------ */
/* Valori JSON                                                         */
/* ------------------------------------------------------------------ */

/* Tempo in ms con 3 decimali (risoluzione 1 us), senza le cifre spurie
 * che il double darebbe con 17 cifre significative */
static cJSON* ms_json(double ms)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", ms);
    return cJSON_CreateRaw(buf);
}

/* Errore in forma unica: {"kind": ..., "code": n} oppure {"kind": ..., "msg": "..."} */
static cJSON* error_json(const char* kind, int code, const char* msg)
{
    cJSON* e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "kind", kind);
    if (msg) cJSON_AddStringToObject(e, "msg", msg);
    else cJSON_AddNumberToObject(e, "code", code);
    return e;
}

/* ------------------------------------------------------------------ */
/* Valori MMS                                                          */
/* ------------------------------------------------------------------ */

/* Aggiunge a o i campi "type" e "value" di un valore MMS, per esempio
 *   {"type": "float", "value": 230.5}
 *   {"type": "bitstring", "value": "0000000000000"}
 *   {"type": "struct", "value": [ ...valori tipizzati... ]}
 * e "error" se il valore e' un errore o di un tipo non usato dall'IED di
 * prova ("unsupported"). Usato sia per le letture MMS sia per i dataset
 * GOOSE (ricorsivo sulle strutture). Bit string: un carattere
 * per bit, bit 0 a sinistra; l'interpretazione (es. Dbpos o Tcmd, entrambe di
 * 2 bit) dipende dal tipo del DA nell'SCL e non e' fatta qui. */
static void add_mms_value(cJSON* o, MmsValue* v)
{
    const char* type = NULL;
    cJSON* val = NULL;
    cJSON* err = NULL;
    MmsType t = v ? MmsValue_getType(v) : MMS_DATA_ACCESS_ERROR;

    if (v) switch (t) {
        case MMS_STRUCTURE:
            type = "struct";
            val = cJSON_CreateArray();
            for (uint32_t i = 0; i < MmsValue_getArraySize(v); i++) {
                cJSON* e = cJSON_CreateObject();
                add_mms_value(e, MmsValue_getElement(v, i));
                cJSON_AddItemToArray(val, e);
            }
            break;
        case MMS_BOOLEAN:
            type = "bool";
            val = cJSON_CreateBool(MmsValue_getBoolean(v));
            break;
        case MMS_INTEGER:
            /* Include gli enumerati (es. Mod, Beh, Health) */
            type = "int";
            val = cJSON_CreateNumber((double)MmsValue_toInt64(v));
            break;
        case MMS_FLOAT: {
            /* 9 cifre significative: sufficienti per un float a 32 bit, senza
             * le cifre spurie della conversione a double. NaN/Inf non sono JSON */
            double d = MmsValue_toDouble(v);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.9g", d);
            type = "float";
            val = isfinite(d) ? cJSON_CreateRaw(buf) : cJSON_CreateNull();
            break;
        }
        case MMS_BIT_STRING: {
            int n = MmsValue_getBitStringSize(v);
            char* bits = malloc((size_t)n + 1);
            for (int i = 0; i < n; i++)
                bits[i] = MmsValue_getBitStringBit(v, i) ? '1' : '0';
            bits[n] = '\0';
            type = "bitstring";
            val = cJSON_CreateString(bits);
            free(bits);
            break;
        }
        case MMS_VISIBLE_STRING:
        case MMS_STRING: {
            const char* s = MmsValue_toString(v);
            type = "string";
            val = cJSON_CreateString(s ? s : "");
            break;
        }
        case MMS_UTC_TIME:
            /* Timestamp IEC 61850 in millisecondi da epoch */
            type = "utcTime";
            val = cJSON_CreateNumber((double)MmsValue_getUtcTimeInMs(v));
            break;
        case MMS_DATA_ACCESS_ERROR:
            /* L'IED ha rifiutato l'accesso all'oggetto (es. 10 = inesistente, 3 = negato) */
            err = error_json("access", (int)MmsValue_getDataAccessError(v), NULL);
            break;
        default:
            err = error_json("unsupported", t, NULL);   /* code = MmsType */
            break;
    }
    cJSON_AddItemToObject(o, "type", type ? cJSON_CreateString(type) : cJSON_CreateNull());
    cJSON_AddItemToObject(o, "value", val ? val : cJSON_CreateNull());
    if (err) cJSON_AddItemToObject(o, "error", err);
}

/* Legge i punti di items e ritorna un oggetto con un elemento per punto:
 *   "chiave": {"ref", "fc", "type", "value"} (+ "error" se non letto).
 * attr: attributo da leggere sotto ref (RCB: "RptEna"), NULL per i punti.
 * Errori: "client" (richiesta fallita, code = IedClientError), "access"
 * (rifiutata dall'IED, code = MmsDataAccessError). */
static cJSON* read_points(IedConnection con, const cJSON* items, const char* attr)
{
    cJSON* section = cJSON_CreateObject();
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        const char* fc = jstr(item, "fc");
        char ref[300];
        if (attr) snprintf(ref, sizeof(ref), "%s.%s", jstr(item, "ref"), attr);
        else snprintf(ref, sizeof(ref), "%s", jstr(item, "ref"));

        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ref", ref);
        cJSON_AddStringToObject(o, "fc", fc);
        IedClientError err;
        MmsValue* v = IedConnection_readObject(con, &err, ref, FunctionalConstraint_fromString(fc));
        add_mms_value(o, err == IED_ERROR_OK ? v : NULL);
        if (err != IED_ERROR_OK || !v)
            cJSON_AddItemToObject(o, "error", error_json("client", err, NULL));
        if (v) MmsValue_delete(v);   /* il valore e' allocato dalla libreria */
        cJSON_AddItemToObject(section, jstr(item, "key"), o);
    }
    return section;
}

/* ------------------------------------------------------------------ */
/* GOOSE                                                               */
/* ------------------------------------------------------------------ */

/* Stato della ricezione: scritto solo dal thread del GooseReceiver e letto
 * dal main solo dopo GooseReceiver_stop(), che attende la fine del thread:
 * non serve un mutex. */
typedef struct {
    cJSON* frames;              /* frame ricevuti, gia' in JSON */
    int n;
    char goID[130], datSet[130];
} GooseCapture;

static double now_epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Callback chiamata da libiec61850 per ogni frame del GoCB. Gira sul thread
 * del GooseReceiver: deve essere breve. I valori del dataset vanno usati
 * solo qui (la libreria li sovrascrive al frame successivo), per questo il
 * frame viene subito convertito in JSON. */
static void goose_listener(GooseSubscriber sub, void* param)
{
    GooseCapture* cap = (GooseCapture*)param;
    double rx = now_epoch_ms();
    if (cap->n++ == 0) {
        /* Informazioni fisse del flusso, prese dal primo frame */
        const char* id = GooseSubscriber_getGoId(sub);
        const char* ds = GooseSubscriber_getDataSet(sub);
        snprintf(cap->goID, sizeof(cap->goID), "%s", id ? id : "");
        snprintf(cap->datSet, sizeof(cap->datSet), "%s", ds ? ds : "");
    }

    MmsValue* values = GooseSubscriber_getDataSetValues(sub);
    bool list = values && (MmsValue_getType(values) == MMS_ARRAY ||
                           MmsValue_getType(values) == MMS_STRUCTURE);
    int entries = list ? (int)MmsValue_getArraySize(values) : 0;

    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", cap->n);
    cJSON_AddItemToObject(o, "rxMs", ms_json(rx));
    cJSON_AddNumberToObject(o, "stNum", GooseSubscriber_getStNum(sub));
    cJSON_AddNumberToObject(o, "sqNum", GooseSubscriber_getSqNum(sub));
    cJSON_AddNumberToObject(o, "timeAllowedtoLive", GooseSubscriber_getTimeAllowedToLive(sub));
    cJSON_AddNumberToObject(o, "t", (double)GooseSubscriber_getTimestamp(sub));
    cJSON_AddNumberToObject(o, "confRev", GooseSubscriber_getConfRev(sub));
    cJSON_AddBoolToObject(o, "simulation", GooseSubscriber_isTest(sub));
    cJSON_AddBoolToObject(o, "ndsCom", GooseSubscriber_needsCommission(sub));
    cJSON_AddBoolToObject(o, "valid", GooseSubscriber_isValid(sub));
    cJSON* data = cJSON_AddArrayToObject(o, "allData");
    for (int i = 0; i < entries; i++) {
        cJSON* e = cJSON_CreateObject();
        add_mms_value(e, MmsValue_getElement(values, i));
        cJSON_AddItemToArray(data, e);
    }
    cJSON_AddItemToArray(cap->frames, o);
}

/* Crea il ricevitore con un subscriber per il solo GoCB della configurazione */
static GooseReceiver goose_setup(GooseCapture* cap, const RunCfg* run)
{
    GooseReceiver rx = GooseReceiver_create();
    GooseReceiver_setInterfaceId(rx, run->iface);
    GooseSubscriber s = GooseSubscriber_create((char*)run->gocbRef, NULL);  /* copia il nome */
    GooseSubscriber_setAppId(s, (uint16_t)run->appid);
    GooseSubscriber_setListener(s, goose_listener, cap);
    GooseReceiver_addSubscriber(rx, s);
    return rx;
}

/* Sezione "goose": flusso e frame. Il nodo cap->frames passa all'oggetto
 * ritornato. */
static cJSON* goose_json(GooseCapture* cap, const RunCfg* run, double listenMs)
{
    char appid[8];
    snprintf(appid, sizeof(appid), "%04X", run->appid);
    cJSON* g = cJSON_CreateObject();
    cJSON_AddItemToObject(g, "listenMs", ms_json(listenMs));
    cJSON_AddStringToObject(g, "gocbRef", run->gocbRef);
    cJSON_AddStringToObject(g, "appid", appid);
    cJSON_AddItemToObject(g, "goID", cap->n ? cJSON_CreateString(cap->goID) : cJSON_CreateNull());
    cJSON_AddItemToObject(g, "datSet", cap->n ? cJSON_CreateString(cap->datSet) : cJSON_CreateNull());
    cJSON_AddItemToObject(g, "frames", cap->frames);
    cap->frames = NULL;
    return g;
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
    fprintf(stderr, "[parser] %s:%d, %d data points, %d report blocks, GoCB %s, "
            "GOOSE su %s per %.1f s\n", run.host, run.port, cJSON_GetArraySize(run.points),
            cJSON_GetArraySize(run.reportBlocks), run.gocbRef, run.iface, run.gooseTimeS);

    /* --- Ricezione GOOSE: parte PRIMA della connessione MMS, cosi' i frame
     *     pubblicati durante le letture vengono registrati --- */
    GooseCapture gcap = {0};
    gcap.frames = cJSON_CreateArray();
    GooseReceiver grx = goose_setup(&gcap, &run);
    GooseReceiver_start(grx);
    struct timespec t_goose_start, t_goose_stop;
    clock_gettime(CLOCK_MONOTONIC, &t_goose_start);
    if (!GooseReceiver_isRunning(grx)) {
        fprintf(stderr, "[goose] ricezione non avviata su %s: servono root o CAP_NET_RAW, "
                        "oppure l'interfaccia non esiste\n", run.iface);
        GooseReceiver_destroy(grx);
        cJSON_Delete(gcap.frames);
        cJSON_Delete(cfg);
        return 4;
    }
    fprintf(stderr, "[goose] in ascolto su %s\n", run.iface);

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
        cJSON_Delete(gcap.frames);
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
        cJSON_Delete(gcap.frames);
        cJSON_Delete(cfg);
        return 3;
    }

    /* Sezione "mms": punti e report block */
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", OUTPUT_FORMAT);
    cJSON* mms = cJSON_AddObjectToObject(root, "mms");
    cJSON_AddItemToObject(mms, "connectMs", ms_json(elapsed_ms(&t_start, &t_connected)));
    cJSON_AddItemToObject(mms, "points", read_points(con, run.points, NULL));
    cJSON_AddItemToObject(mms, "reportBlocks", read_points(con, run.reportBlocks, "RptEna"));

    /* Tempo totale impiegato per tutte le letture (dopo la connessione) */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    cJSON_AddItemToObject(mms, "readMs", ms_json(elapsed_ms(&t_connected, &t_end)));

    /* --- Fine ricezione GOOSE: ascolto per almeno goose_time_s dall'avvio --- */
    struct timespec now, pause = {0, 20 * 1000000L};
    clock_gettime(CLOCK_MONOTONIC, &now);
    while (elapsed_ms(&t_goose_start, &now) < run.gooseTimeS * 1000.0) {
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    }
    GooseReceiver_stop(grx);        /* attende la fine del thread di ricezione */
    clock_gettime(CLOCK_MONOTONIC, &t_goose_stop);
    fprintf(stderr, "[goose] %d frame GOOSE ricevuti\n", gcap.n);
    cJSON_AddItemToObject(root, "goose",
                          goose_json(&gcap, &run, elapsed_ms(&t_goose_start, &t_goose_stop)));

    /* --- Scrittura del file --- */
    char* text = cJSON_Print(root);
    fputs(text, fp);
    fputc('\n', fp);
    fclose(fp);
    cJSON_free(text);
    fprintf(stderr, "[parser] dati salvati in %s\n", run.output);

    /* --- Chiusura connessione e rilascio risorse --- */
    IedConnection_close(con);
    IedConnection_destroy(con);
    GooseReceiver_destroy(grx);                     /* libera anche il subscriber */
    cJSON_Delete(root);                             /* comprende i frame GOOSE */
    cJSON_Delete(cfg);                              /* run.host/iface/... puntano qui */
    return 0;
}
