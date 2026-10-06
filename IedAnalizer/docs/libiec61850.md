# libiec61850 nel banco di prova BU9020

Questo documento spiega **come funziona libiec61850** e **come la usa il nostro
parser** (`source/parser/parser.c`). Per ogni argomento c'è, a sinistra, cosa
offre la libreria e, a destra, il codice corrispondente del parser con il
numero di riga.

| | |
|---|---|
| Versione | **1.6.2**, ramo `v1.6_develop` (commit `bc495e97`) |
| Sorgenti | `~/libiec61850` |
| Header e libreria | `~/libiec61850/.install/{include,lib}`, collegati al progetto da `lib/libiec61850` |
| Licenza | **GPLv3** (esiste una licenza commerciale di MZ Automation) |

> **Licenza.** Per la tesi e per un banco di prova interno la GPLv3 va bene.
> Se il software diventasse un prodotto distribuito da COL Group, servirebbe
> la licenza commerciale. Da segnalare al tutor.

---

## 0. Glossario minimo

| Termine | Significato | Esempio (IED di test) |
|---|---|---|
| **IED** | il dispositivo (relè, controllore di stallo) | `XSCT1M01SMNPBMUC` |
| **LD** – Logical Device | gruppo di funzioni dell'IED | `XSCT1M01SMNPBMUCM1` |
| **LN** – Logical Node | una funzione (misure, interruttore, sincronismo…) | `LBMULPHD1` |
| **DO** – Data Object | un dato della funzione | `PhyHealth` |
| **DA** – Data Attribute | un campo del dato | `stVal`, `q`, `t` |
| **FC** – Functional Constraint | categoria dell'attributo | `ST` stato, `MX` misura, `DC` descrizione |
| **MMS** | protocollo client/server su TCP (porta 102) per leggere e scrivere i dati | |
| **GOOSE** | messaggi multicast Ethernet che l'IED pubblica a ogni evento | |
| **GoCB** | il blocco che controlla un flusso GOOSE | `LLN0$GO$GseBCU_Op_gocb` |
| **RCB** | Report Control Block: l'IED invia i dati al client quando cambiano | `LLN0.op_urcb01` |

---

## 1. Cosa fa la libreria e quali parti usiamo

libiec61850 è scritta in C e implementa i protocolli di IEC 61850. Ha una parte
*server* (per costruire un IED) e una parte *client* (per interrogarlo).

| Parte della libreria | Header | La usiamo? |
|---|---|---|
| Client MMS | `iec61850_client.h` | **sì**: letture dei punti e degli RCB |
| Ricezione GOOSE | `goose_receiver.h`, `goose_subscriber.h` | **sì**: tutti i GOOSE in rete |
| Valori MMS | `mms_value.h` (incluso dai precedenti) | **sì**: conversione in JSON |
| Server IEC 61850 | `iec61850_server.h` | no (solo l'esempio `server_example_basic_io` per i test) |
| Pubblicazione GOOSE, Sampled Values, R-GOOSE | `goose_publisher.h`, `sv_*.h` | no |

```mermaid
flowchart TB
    P["parser.c"]
    subgraph LIB["libiec61850"]
        C["Client MMS<br/>IedConnection_*"]
        G["Ricezione GOOSE<br/>GooseReceiver_* / GooseSubscriber_*"]
        V["Valori<br/>MmsValue_*"]
        S["Stack MMS su TCP (porta 102)"]
        E["Socket Ethernet raw (livello 2)"]
    end
    P --> C --> S
    P --> G --> E
    C --> V
    G --> V
```

Due conseguenze pratiche:

- **MMS** usa TCP/IP: bastano IP e porta dell'IED (`host`, `port` in configurazione).
- **GOOSE** non usa IP: è un frame Ethernet (EtherType `0x88B8`) inviato a un
  MAC multicast. Per riceverlo la libreria apre un *socket raw*
  sull'interfaccia di rete: per questo il parser va eseguito con **`sudo`**
  (o con `setcap cap_net_raw+ep bin/parser`).

---

## 2. I nomi dei dati

Lo stesso dato ha nomi diversi a seconda di dove compare. È la causa più
frequente di errori.

| Dove | Forma | Esempio |
|---|---|---|
| **Configurazione del parser** e API client | `LD/LN.DO.DA` + FC a parte | `XSCT1M01SMNPBMUCM1/LBMULPHD1.PhyHealth.stVal`, FC `ST` |
| **Sulla rete** (variabile MMS) | dominio `LD`, nome `LN$FC$DO$DA` | `LBMULPHD1$ST$PhyHealth$stVal` |
| **Frame GOOSE** | `LD/LN$GO$nome` | `XSCT1M01SMNPBMUCM1/LLN0$GO$GseBCU_Op_gocb` |

La libreria converte da sola la prima forma nella seconda: il nome del LD
diventa il *dominio* MMS, i `.` diventano `$` e l'FC viene inserito dopo il LN.
Il nome della variabile MMS non può superare **64 caratteri** (il riferimento
completo 129): oltre, la lettura fallisce con errore 12.

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

L'FC è un tipo enumerato (`IEC61850_FC_ST`, `IEC61850_FC_MX`…).
`FunctionalConstraint_fromString("ST")` lo ricava dalla stringa, ma guarda
**solo i primi due caratteri**: `"STX"` verrebbe preso per `ST`.

</td><td>

`config_item()`, righe 112-115: si controlla anche la lunghezza.

```c
const char* f = jstr(o, "fc", "MX");
*fc = strlen(f) == 2
      ? FunctionalConstraint_fromString(f)
      : IEC61850_FC_NONE;
```

Un FC non valido non viene letto: nell'output compare
`"config_error": "FC sconosciuto \"STX\""`.

</td></tr>
</table>

---

## 3. Connessione MMS

La connessione si gestisce con un oggetto `IedConnection`, in quattro passi:
**crea → connetti → usa → chiudi e distruggi**.

```mermaid
sequenceDiagram
    participant P as parser.c
    participant L as libiec61850
    participant I as IED :102
    P->>L: IedConnection_create()
    P->>L: IedConnection_connect(host, port)
    L->>I: apertura TCP + avvio sessione MMS
    I-->>L: risposta
    L-->>P: err = IED_ERROR_OK
    loop per ogni punto
        P->>L: IedConnection_readObject(ref, FC)
        L->>I: MMS Read
        I-->>L: valore o errore
        L-->>P: MmsValue*
    end
    P->>L: IedConnection_close() + destroy()
```

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

`IedConnection_connect()` è **bloccante**: ritorna quando la sessione è aperta
o è fallita. L'esito è in `err`.

Timeout di default (non li cambiamo):
connessione **10 s**, ogni richiesta **5 s**
(`IedConnection_setConnectTimeout` / `setRequestTimeout` per modificarli).

Internamente la libreria avvia un **thread** che riceve le risposte.

</td><td>

`main()`, righe 629-638:

```c
IedConnection con = IedConnection_create();
IedConnection_connect(con, &err, run.host, run.port);
if (err != IED_ERROR_OK) {
    /* es. err=5: IED spento o irraggiungibile */
    IedConnection_destroy(con);
    GooseReceiver_destroy(grx);
    ...
    return 2;               /* exit code 2 */
}
```

Righe 753-754, alla fine:

```c
IedConnection_close(con);
IedConnection_destroy(con);
```

</td></tr>
</table>

---

## 4. Lettura di un dato

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

`IedConnection_readObject(con, &err, ref, fc)` legge un attributo (o un DO
intero) e restituisce un **`MmsValue*`**.

Regole:

- se la richiesta fallisce, `err` ≠ `IED_ERROR_OK`;
- il valore restituito **va liberato** con `MmsValue_delete()`;
- leggendo un DO intero (es. `GGIO1.AnIn1`, FC `MX`) si ottiene una
  struttura con valore, qualità e timestamp in una sola richiesta.

</td><td>

`read_point_json()`, righe 291-304:

```c
IedClientError err;
MmsValue* value =
    IedConnection_readObject(con, &err, ref, fc);
...
if (value == NULL || err != IED_ERROR_OK)
    fprintf(fp, "{\"type\": null, \"value\": null,"
                " \"error\": %d}", err);
else
    fprint_mms_value(fp, value);
if (value) MmsValue_delete(value);
```

</td></tr>
</table>

---

## 5. I valori: `MmsValue`

`MmsValue` è un contenitore generico: prima si chiede il **tipo**, poi si
legge il valore con la funzione giusta. Il parser lo fa in un'unica funzione,
`fprint_mms_value()` (righe 205-287), usata sia per MMS sia per GOOSE.

| Tipo MMS | Dati IEC 61850 tipici | Funzione della libreria | Output del parser |
|---|---|---|---|
| `MMS_BOOLEAN` | SPS.stVal, ACT.general | `MmsValue_getBoolean` | `{"type": "bool", "value": true}` |
| `MMS_INTEGER` | enumerati: Mod, Beh, Health | `MmsValue_toInt64` | `{"type": "int", "value": 1}` |
| `MMS_UNSIGNED` | contatori | `MmsValue_toUint32` | `{"type": "uint", "value": 3}` |
| `MMS_FLOAT` | misure (mag.f) | `MmsValue_toDouble` | `{"type": "float", "value": 0.841471076}` |
| `MMS_BIT_STRING` 2 bit | **Dbpos** (Pos.stVal) | `Dbpos_fromMmsValue` | `{"type": "dbpos", "value": 2, "label": "on", "bits": "10"}` |
| `MMS_BIT_STRING` altri | **Quality** (q, 13 bit) | `MmsValue_getBitStringBit` | `{"type": "bitstring", "value": 0, "size": 13, "bits": "0000000000000"}` |
| `MMS_VISIBLE_STRING` | NamPlt.vendor | `MmsValue_toString` | `{"type": "string", "value": "TMW"}` |
| `MMS_UTC_TIME` | timestamp (t) | `MmsValue_getUtcTimeInMs` | `{"type": "utc_time", "value": 444958306940}` |
| `MMS_STRUCTURE` | DO o DA composto | `MmsValue_getElement(v, i)` | `{"type": "struct", "value": [ ... ]}` |
| `MMS_DATA_ACCESS_ERROR` | (errore, vedi §6) | `MmsValue_getDataAccessError` | `{"type": null, "value": null, "access_error": 10}` |

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

Strutture e array contengono altri `MmsValue`, raggiungibili con
`MmsValue_getElement()`. Questi elementi appartengono alla struttura e
**non** vanno liberati uno per uno.

Le bit string di 2 bit si interpretano con `Dbpos_fromMmsValue()`:
0 intermedio, 1 off, 2 on, 3 bad.
Attenzione: anche **Tcmd** (comando tap, `ATCC.TapChg`) è di 2 bit e
significa 1 = lower, 2 = higher.

</td><td>

Struttura → lista ricorsiva (righe 210-219):

```c
case MMS_STRUCTURE:
case MMS_ARRAY: {
    int n = (int)MmsValue_getArraySize(v);
    ...
    for (int i = 0; i < n; i++)
        fprint_mms_value(fp, MmsValue_getElement(v, i));
```

Dbpos (righe 238-251):

```c
if (n == 2) {
    Dbpos d = Dbpos_fromMmsValue(v);
    fprintf(fp, "{\"type\": \"dbpos\", \"value\": %d,"
                " \"label\": \"%s\", ", (int)d, dbpos_name(d));
}
...
for (int i = 0; i < n; i++)      /* "bits": bit 0 a sinistra */
    fputc(MmsValue_getBitStringBit(v, i) ? '1' : '0', fp);
```

</td></tr>
</table>

---

## 6. Gli errori: tre livelli

Nel JSON prodotto dal parser un punto può fallire in tre modi diversi:

```mermaid
flowchart LR
    C{"configurazione<br/>valida?"} -- no --> CE["config_error<br/>(non letto)"]
    C -- sì --> R["readObject"] --> E{"err == OK?"}
    E -- no --> SE["error<br/>(comunicazione)"]
    E -- sì --> T{"tipo del valore"}
    T -- DATA_ACCESS_ERROR --> AE["access_error<br/>(l'IED rifiuta l'oggetto)"]
    T -- altro --> OK["valore"]
```

| Campo nel JSON | Chi lo genera | Codici più comuni |
|---|---|---|
| `config_error` | il parser, prima di leggere | `manca "ref"`, `FC sconosciuto "STX"` |
| `error` (`IedClientError`, da `iec61850_client.h`) | la libreria: la richiesta non è andata a buon fine | **5** IED spento/irraggiungibile, 3 connessione persa, 12 riferimento non valido, 20 timeout |
| `access_error` (`MmsDataAccessError`, da `mms_value.h`) | l'IED: la richiesta è arrivata ma l'oggetto è rifiutato | **10** oggetto inesistente, **3** accesso negato, 2 temporaneamente non disponibile, 7 tipo incoerente |

Casi visti sull'IED di test: `error 5` durante la riconfigurazione,
`access_error 10` con i riferimenti `BU9020/...` (nome del LD sbagliato),
`access_error 3` su `LBMULTMS1.TmSrc.stVal`.

---

## 7. Report Control Block

Un **RCB** fa sì che sia l'IED a inviare al client i valori di un dataset
quando cambiano, con un timestamp dell'IED. È più preciso del polling.

- `RP` = report non bufferizzato, `BR` = bufferizzato (l'IED conserva i report
  se il client è scollegato).
- Ogni RCB esiste in più **istanze**, una per client: `op_urcb01` … `op_urcb04`.
- `RptEna` dice se l'istanza è abilitata.

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

Le API dei report usano due forme diverse di riferimento:

- `IedConnection_readObject` vuole il riferimento **senza** FC
  (`LLN0.op_urcb01.RptEna` + `IEC61850_FC_RP`);
- `IedConnection_getRCBValues` vuole il riferimento **con** l'FC
  (`LLN0.RP.op_urcb01`).

Oggi il parser **legge solo `RptEna`**; per ricevere i report andrebbero usati
`getRCBValues`, `installReportHandler` e `setRCBValues` (vedi
`examples/iec61850_client_example_reporting`).

</td><td>

`main()`, righe 707-722: si accettano entrambe le forme togliendo `.RP.`/`.BR.`.

```c
snprintf(attr, sizeof(attr), "%s.RptEna", ref);
char* seg = strstr(attr, ".RP.");
if (!seg) seg = strstr(attr, ".BR.");
if (seg) memmove(seg, seg + 3, strlen(seg + 3) + 1);
MmsValue* v = IedConnection_readObject(con, &err, attr, fc);
if (v && err == IED_ERROR_OK &&
    MmsValue_getType(v) == MMS_BOOLEAN)
    fprintf(fp, "{\"RptEna\": %s}", ...);
```

Risultato sull'IED di test: `{"RptEna": false}` per i tre RCB.

</td></tr>
</table>

---

## 8. GOOSE

### 8.1 Come si comporta un IED

- A **ogni cambio** di un valore del dataset: `stNum` aumenta di 1, `sqNum`
  riparte da 0 (o 1), il frame viene ripetuto a intervalli crescenti a partire
  da `MinTime` (3 ms nell'IED di test).
- **Senza cambi**: il frame viene ripetuto ogni `MaxTime` (heartbeat, 2000 ms
  nell'IED di test) con `sqNum` crescente.
- Ogni frame dichiara un **TTL** (`timeAllowedToLive`, 4000 ms nell'IED di
  test): se il frame successivo non arriva entro il TTL, il flusso è
  considerato perso.

### 8.2 Ricevitore e subscriber

La ricezione usa due oggetti:

- **`GooseReceiver`**: apre l'interfaccia di rete e, in un suo **thread**,
  riceve i frame;
- **`GooseSubscriber`**: riceve i frame di un GoCB (dato il `gocbRef`) oppure,
  in modo **observer**, di tutti i GoCB; per ogni frame chiama una callback.

<table>
<tr><th>Libreria</th><th>parser.c</th></tr>
<tr><td>

Creazione:

1. `GooseReceiver_create()` + `setInterfaceId(iface)`
2. `GooseSubscriber_create(gocbRef, NULL)`
3. `setObserver()` per ricevere **tutti** i GOOSE
4. `setListener(callback, parametro)`
5. `GooseReceiver_addSubscriber()`

</td><td>

`goose_setup()`, righe 470-476: un solo subscriber in modo observer.

```c
GooseReceiver rx = GooseReceiver_create();
GooseReceiver_setInterfaceId(rx, iface);
static char any[] = "";
GooseSubscriber s = GooseSubscriber_create(any, NULL);
GooseSubscriber_setObserver(s);
GooseSubscriber_setListener(s, goose_listener, cap);
GooseReceiver_addSubscriber(rx, s);
```

I GoCB della configurazione non filtrano la ricezione: servono solo a marcare
i flussi come attesi (`goose_stream_for()`, riga 362).

</td></tr>
<tr><td>

Avvio e arresto:

- `GooseReceiver_start()` avvia il thread; se non riesce ad aprire il socket
  (niente root, interfaccia sbagliata) `GooseReceiver_isRunning()` resta
  `false`;
- `GooseReceiver_stop()` **aspetta la fine del thread**;
- `GooseReceiver_destroy()` libera anche i subscriber.

</td><td>

`main()`, righe 612-621: la ricezione parte **prima** della connessione MMS.

```c
GooseReceiver_start(grx);
if (!GooseReceiver_isRunning(grx)) {
    snprintf(goose_err, ..., "ricezione non avviata su %s: "
             "servono root o CAP_NET_RAW ...", run.iface);
```

Righe 733-742: dopo le letture MMS si attende fino a `goose_time_s`, poi:

```c
GooseReceiver_stop(grx);   /* attende la fine del thread */
```

</td></tr>
<tr><td>

Nella callback si leggono i campi del frame:

| Funzione | Campo |
|---|---|
| `getGoCbRef`, `getGoId`, `getDataSet` | identità del flusso |
| `getStNum`, `getSqNum` | stato e sequenza |
| `getTimeAllowedToLive` | TTL (ms) |
| `getTimestamp` | `t`: ultimo cambio di stato (orologio IED) |
| `getConfRev`, `isTest`, `needsCommission` | configurazione e flag |
| `isValid` | frame decodificato correttamente |
| `getDataSetValues` | valori del dataset (`MmsValue`) |

La callback gira sul thread del ricevitore: deve essere **breve**. I valori
del dataset vanno usati **solo dentro la callback**: la libreria riusa lo
stesso `MmsValue` al frame successivo.

</td><td>

`goose_listener()`, righe 390-449:

```c
double rx = now_epoch_ms();          /* istante di ricezione */
const char* ref = GooseSubscriber_getGoCbRef(sub);
int appid = GooseSubscriber_getAppId(sub);
...
f->stNum = GooseSubscriber_getStNum(sub);
f->sqNum = GooseSubscriber_getSqNum(sub);
f->ttl   = GooseSubscriber_getTimeAllowedToLive(sub);
f->t_ms  = GooseSubscriber_getTimestamp(sub);
f->valid = GooseSubscriber_isValid(sub);
```

I valori vengono **subito convertiti in testo JSON** con la stessa funzione
usata per MMS:

```c
FILE* mem = open_memstream(&f->values, &len);
for (int i = 0; i < f->entries; i++)
    fprint_mms_value(mem, MmsValue_getElement(values, i));
```

</td></tr>
</table>

> **Thread e dati condivisi.** I frame vengono scritti dal thread del
> ricevitore e letti dal `main` solo dopo `GooseReceiver_stop()`, che aspetta
> la fine di quel thread: per questo non serve un mutex.

### 8.3 libiec61850 o tshark?

| | libiec61850 (`bin/parser`) | tshark / Wireshark |
|---|---|---|
| Quando | in tempo reale, insieme alle letture MMS | a posteriori, su file `.pcapng` |
| Risultato | JSON per i test Robot | file da analizzare a mano |
| Istante di ricezione | preso nella callback (meno preciso) | preso dal kernel (più preciso) |
| Dataset | decodificato completamente | tshark 4.2.2 segnala i frame come malformati (bug del decoder) |

---

## 9. Comandi (FC `CO`) – non usati

Il parser non invia comandi. Se servissero (per esempio per uno scenario di
richiusura) si usa `ControlObjectClient`, con una sequenza che dipende dal
`ctlModel` del dato: `operate` diretto, oppure `select` + `operate` (SBO).
Nell'IED di test `LDFRRDRE1.RcdTrg` è `sbo-with-enhanced-security`.

```c
ControlObjectClient ctl = ControlObjectClient_create("LD/LN.DO", con);
MmsValue* val = MmsValue_newBoolean(true);
if (ControlObjectClient_selectWithValue(ctl, val))     /* solo per SBO */
    ControlObjectClient_operate(ctl, val, 0);
MmsValue_delete(val);
ControlObjectClient_destroy(ctl);
```

> Un comando agisce sull'impianto: va usato solo con l'autorizzazione del
> tutor e sugli oggetti previsti dagli scenari.

---

## 10. Compilazione e strumenti

**Compilazione del parser** (`make`, dalla cartella `IedAnalizer`):

```sh
cc -Ilib/libiec61850/include -O2 -g -Wall -Wextra source/parser/parser.c -o bin/parser \
   -Llib/libiec61850/lib -liec61850 -lcjson -lpthread
```

`-lpthread` serve perché la libreria usa thread (connessione MMS e ricevitore
GOOSE); `-lcjson` per la lettura della configurazione.

**Ricompilare la libreria**:

```sh
cd ~/libiec61850
make && sudo make install      # header e libreria in .install/
```

**Strumenti di diagnosi** compilati in `~/libiec61850/build/examples/`:

| Comando | A cosa serve |
|---|---|
| `mms_utility/mms_utility -h <ip> -i` | identità del server (IED di test: *Tamarack MMSd 8.8*) |
| `mms_utility/mms_utility -h <ip> -d` | elenco dei Logical Device |
| `mms_utility/mms_utility -h <ip> -a <LD> -r 'LLN0$ST$Health$stVal'` | lettura di una variabile, indipendente dal nostro codice |
| `server_example_basic_io/server_example_basic_io 10102` | IED simulato in locale, per provare il parser senza hardware |

---

## 11. Problemi comuni

| Sintomo nel JSON o sul terminale | Causa probabile | Cosa controllare |
|---|---|---|
| exit code 2, `connessione fallita (err=5)` | IED spento, IP errato, cavo | `ping`, `mms_utility -i` |
| `access_error: 10` su **tutti** i punti | nome del LD sbagliato | `mms_utility -d`, file SCL |
| `access_error: 10` su un punto | DO, DA o FC sbagliati | `scl_extract.py` |
| `access_error: 3` | l'IED nega la lettura | togliere il punto o chiedere al tutor |
| valori `0` o stringhe vuote | l'IED non popola il modello | `mms_utility`: se dà lo stesso valore, non è il parser |
| `RptEna: null` con `access_error: 10` | manca l'indice d'istanza (`op_urcb` invece di `op_urcb01`) | `report_inventario.csv` |
| exit code 4, `goose.error` valorizzato | manca `sudo` o l'interfaccia è sbagliata | `sudo`, `ip link` |
| GOOSE atteso con `"frames": 0` | il GoCB non pubblica, APPID diverso, VLAN | `sudo tshark -i <if> -Y goose` |

---

## Riferimenti

- Header: `~/libiec61850/.install/include` (`iec61850_client.h`,
  `mms_value.h`, `goose_receiver.h`, `goose_subscriber.h`)
- Esempi ufficiali: `~/libiec61850/examples` (`iec61850_client_example1`,
  `iec61850_client_example_reporting`, `iec61850_client_example_control`,
  `goose_subscriber`)
- Sito del progetto: <https://libiec61850.com>
- Standard: IEC 61850-7-2 (servizi), IEC 61850-7-3 (classi di dati),
  IEC 61850-8-1 (mappatura su MMS e GOOSE)
