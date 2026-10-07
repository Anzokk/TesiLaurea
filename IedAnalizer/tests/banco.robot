*** Settings ***
Documentation     Scenari di test della BU9020: ogni test esegue uno scenario
...               YAML (formato in Banco.py). Si esegue con: make test
Library           Banco.py    ${CONFIG}    ${SERIALE}
Test Template     Esegui Scenario

*** Variables ***
${CONFIG}         config/parser/parser.json
${SERIALE}        /dev/ttyUSB0
${SCENARI}        tests/scenari

*** Test Cases ***                  SCENARIO
Riposo e struttura GOOSE            ${SCENARI}/00_riposo.yaml
Controllo di sincronismo            ${SCENARI}/01_sincronismo.yaml
Richiusura automatica               ${SCENARI}/02_richiusura.yaml
Regolazione tensione                ${SCENARI}/03_regolazione_tensione.yaml
