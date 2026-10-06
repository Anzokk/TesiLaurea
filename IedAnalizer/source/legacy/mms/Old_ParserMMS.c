#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iec61850_client.h"

// Struttura per definire cosa leggere dal cassetto
typedef struct {
    const char* objectRef;    // Riferimento oggetto (es. "IED1/MMXU1.TotW.mag.f")
    const char* jsonKey;      // Chiave che avra nel file JSON (es. "potenza_totale")
    FunctionalConstraint fc;  // Functional Constraint (es. IEC61850_FC_MX per misure)
} DataPoint;

int main(int argc, char** argv) {
    // --- 1. PARAMETRI DI CONFIGURAZIONE ---
    const char* hostname = "10.145.100.26"; // IP del cassetto elettrico
    int tcpPort = 102;                      // Porta MMS standard
    const char* jsonFilename = "dati_cassetto.json";

    // --- 2. DEFINIZIONE DEI DATI DA LEGGERE ---
    // NOTA: Devi modificare questi riferimenti in base al modello dati reale del tuo cassetto
    DataPoint points[] = {
        // Esempi basati su Logical Node MMXU (Misure) e XCBR (Interruttore)
        {"IED1/MMXU1.TotW.mag.f",  "potenza_attiva",   IEC61850_FC_MX},
        {"IED1/MMXU1.Hz.mag.f",    "frequenza",        IEC61850_FC_MX},
        {"IED1/MMXU1.A.phsA.cVal.mag.f", "corrente_fase_a", IEC61850_FC_MX},
        {"IED1/XCBR1.Pos.stVal",   "stato_interruttore", IEC61850_FC_ST}
    };
    int numPoints = sizeof(points) / sizeof(points[0]);

    // --- 3. INIZIALIZZAZIONE CLIENT IEC 61850 ---
    IedClientError error;
    IedConnection con = IedConnection_create();

    printf("Connessione a %s:%i...\n", hostname, tcpPort);
    IedConnection_connect(con, &error, hostname, tcpPort);

    if (error != IED_ERROR_OK) {
        printf("Errore di connessione: %i\n", error);
        IedConnection_destroy(con);
        return 1;
    }
    printf("Connesso.\n");

    // --- 4. APERTURA FILE JSON ---
    FILE* fp = fopen(jsonFilename, "w");
    if (!fp) {
        perror("Impossibile aprire il file JSON");
        IedConnection_close(con);
        IedConnection_destroy(con);
        return 1;
    }

    // Inizio struttura JSON
    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp\": %ld,\n", (long)time(NULL)); // Timestamp UNIX
    fprintf(fp, "  \"data\": {\n");

    // --- 5. CICLO DI LETTURA E SCRITTURA ---
    for (int i = 0; i < numPoints; i++) {
        MmsValue* value = IedConnection_readObject(con, &error, points[i].objectRef, points[i].fc);

        if (value != NULL && error == IED_ERROR_OK) {
            fprintf(fp, "    \"%s\": ", points[i].jsonKey);

            MmsType type = MmsValue_getType(value);

            // Gestione dei tipi di dato più comuni
            if (type == MMS_FLOAT) {
                float fval = MmsValue_toFloat(value);
                fprintf(fp, "%.3f", fval);
            } 
            else if (type == MMS_INTEGER || type == MMS_UNSIGNED) {
                int ival = MmsValue_toInt32(value);
                fprintf(fp, "%d", ival);
            } 
            else if (type == MMS_BOOLEAN) {
                bool bval = MmsValue_getBoolean(value);
                fprintf(fp, "%s", bval ? "true" : "false");
            } 
            else {
                // Fallback per tipi non gestiti
                fprintf(fp, "\"tipo_non_supportato_%d\"", type);
            }

            // Aggiungi virgola se non è l'ultimo elemento
            if (i < numPoints - 1) fprintf(fp, ",");
            fprintf(fp, "\n");

            // Libera la memoria del valore
            MmsValue_delete(value);
        } else {
            // Se la lettura fallisce, scrivi null o logga l'errore
            fprintf(fp, "    \"%s\": null /* Errore: %i */\n", points[i].jsonKey, error);
            if (i < numPoints - 1) fprintf(fp, ",");
        }
    }

    // Chiusura struttura JSON
    fprintf(fp, "  }\n");
    fprintf(fp, "}\n");
    fclose(fp);

    printf("Dati salvati in %s\n", jsonFilename);

    // --- 6. PULIZIA ---
    IedConnection_close(con);
    IedConnection_destroy(con);

    return 0;
}