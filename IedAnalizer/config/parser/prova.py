import json    
with open ('parser.json', 'r', encoding='utf-8') as file:
        dati = json.load(file)

print(dati)