/*
 * parser.c
 * Legge via MMS i data attribute di un IED e riceve i GOOSE di un suo GoCB
 * nello stesso intervallo; stampa tutto in JSON su stdout.
 *
 * Uso:  sudo bin/parser config.json > risultato.json
 * (la ricezione GOOSE usa un socket raw: servono root o CAP_NET_RAW)
 *
 * Configurazione:
 * {
 *   "host": "10.145.100.26", "port": 102, "interface": "enx503f56005b3e",
 *   "goose_time_s": 5,                   ascolto GOOSE dopo le letture MMS
 *   "gocbRef": "LD/LLN0$GO$nome",
 *   "points": {"LD/LN.DO.DA": "ST", ...} riferimento -> FC
 * }
 * Output:
 * {
 *   "mms":   {"LD/LN.DO.DA": valore, ...},
 *   "goose": [{"rxMs", "goID", "datSet", "stNum", "sqNum",
 *              "timeAllowedtoLive", "t", "allData"}, ...]
 * }
 * Un valore e' bool, numero, stringa (anche le bit string, bit 0 a sinistra)
 * o array (strutture); in caso di errore e' {"clientError": n} (richiesta
 * fallita, IedClientError) o {"accessError": n} (rifiutata dall'IED,
 * MmsDataAccessError); null se di un tipo non gestito.
 * Exit code: 0 = completato, 1 = errore (motivo su stderr).
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include "iec61850_client.h"
#include "goose_receiver.h"
#include "goose_subscriber.h"

/* Valore MMS in JSON (ricorsivo sulle strutture) */
static cJSON* mms_json(MmsValue* v)
{
    if (!v) return cJSON_CreateNull();
    switch (MmsValue_getType(v)) {
        case MMS_STRUCTURE:
        case MMS_ARRAY: {
            cJSON* a = cJSON_CreateArray();
            for (uint32_t i = 0; i < MmsValue_getArraySize(v); i++)
                cJSON_AddItemToArray(a, mms_json(MmsValue_getElement(v, i)));
            return a;
        }
        case MMS_BOOLEAN: return cJSON_CreateBool(MmsValue_getBoolean(v));
        case MMS_INTEGER: return cJSON_CreateNumber(MmsValue_toInt64(v));
        case MMS_FLOAT: return cJSON_CreateNumber(MmsValue_toFloat(v));
        case MMS_UTC_TIME: return cJSON_CreateNumber(MmsValue_getUtcTimeInMs(v));
        case MMS_VISIBLE_STRING:
        case MMS_STRING: return cJSON_CreateString(MmsValue_toString(v));
        case MMS_BIT_STRING: {
            char bits[256];
            int n = MmsValue_getBitStringSize(v) < 255 ? MmsValue_getBitStringSize(v) : 255;
            for (int i = 0; i < n; i++) bits[i] = MmsValue_getBitStringBit(v, i) ? '1' : '0';
            bits[n] = '\0';
            return cJSON_CreateString(bits);
        }
        case MMS_DATA_ACCESS_ERROR: {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "accessError", MmsValue_getDataAccessError(v));
            return e;
        }
        default: return cJSON_CreateNull();
    }
}

/* Chiamata dal thread del GooseReceiver per ogni frame del GoCB. Il dataset
 * va convertito qui: la libreria lo sovrascrive al frame successivo.
 * L'array frames e' letto dal main solo dopo GooseReceiver_stop(), che
 * attende la fine del thread: non serve un mutex. */
static void goose_listener(GooseSubscriber sub, void* frames)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    cJSON* f = cJSON_CreateObject();
    cJSON_AddNumberToObject(f, "rxMs", ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
    cJSON_AddStringToObject(f, "goID", GooseSubscriber_getGoId(sub));
    cJSON_AddStringToObject(f, "datSet", GooseSubscriber_getDataSet(sub));
    cJSON_AddNumberToObject(f, "stNum", GooseSubscriber_getStNum(sub));
    cJSON_AddNumberToObject(f, "sqNum", GooseSubscriber_getSqNum(sub));
    cJSON_AddNumberToObject(f, "timeAllowedtoLive", GooseSubscriber_getTimeAllowedToLive(sub));
    cJSON_AddNumberToObject(f, "t", GooseSubscriber_getTimestamp(sub));
    cJSON_AddItemToObject(f, "allData", mms_json(GooseSubscriber_getDataSetValues(sub)));
    cJSON_AddItemToArray(frames, f);
}

int main(int argc, char** argv)
{
    /* --- Configurazione --- */
    static char buf[1 << 20];
    FILE* fp = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!fp) { fprintf(stderr, "uso: parser config.json\n"); return 1; }
    buf[fread(buf, 1, sizeof(buf) - 1, fp)] = '\0';
    fclose(fp);
    cJSON* cfg = cJSON_Parse(buf);
    const char* host = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "host"));
    const char* iface = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "interface"));
    char* gocbRef = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "gocbRef"));
    const cJSON* port = cJSON_GetObjectItem(cfg, "port");
    const cJSON* gooseTimeS = cJSON_GetObjectItem(cfg, "goose_time_s");
    const cJSON* points = cJSON_GetObjectItem(cfg, "points");
    if (!host || !iface || !gocbRef || !cJSON_IsNumber(port) || !cJSON_IsNumber(gooseTimeS)) {
        fprintf(stderr, "config non valida: servono host, port, interface, goose_time_s, gocbRef\n");
        return 1;
    }

    /* --- Ricezione GOOSE: parte prima delle letture MMS --- */
    cJSON* frames = cJSON_CreateArray();
    GooseReceiver rx = GooseReceiver_create();
    GooseReceiver_setInterfaceId(rx, iface);
    GooseSubscriber sub = GooseSubscriber_create(gocbRef, NULL);
    GooseSubscriber_setListener(sub, goose_listener, frames);
    GooseReceiver_addSubscriber(rx, sub);
    GooseReceiver_start(rx);
    if (!GooseReceiver_isRunning(rx)) {
        fprintf(stderr, "GOOSE non avviato su %s: servono root o CAP_NET_RAW\n", iface);
        return 1;
    }

    /* --- Letture MMS --- */
    IedClientError err;
    IedConnection con = IedConnection_create();
    IedConnection_connect(con, &err, host, port->valueint);
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "connessione MMS a %s fallita (err=%d)\n", host, err);
        return 1;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* mms = cJSON_AddObjectToObject(root, "mms");
    const cJSON* p;
    cJSON_ArrayForEach(p, points) {
        MmsValue* v = IedConnection_readObject(con, &err, p->string,
                                               FunctionalConstraint_fromString(p->valuestring));
        if (v) {
            cJSON_AddItemToObject(mms, p->string, mms_json(v));
            MmsValue_delete(v);
        } else {
            cJSON_AddNumberToObject(cJSON_AddObjectToObject(mms, p->string), "clientError", err);
        }
    }
    IedConnection_close(con);
    IedConnection_destroy(con);

    /* --- Fine ascolto GOOSE e output --- */
    sleep((unsigned)gooseTimeS->valueint);
    GooseReceiver_stop(rx);         /* attende la fine del thread di ricezione */
    cJSON_AddItemToObject(root, "goose", frames);
    puts(cJSON_Print(root));
    return 0;                       /* la memoria e' liberata all'uscita */
}
