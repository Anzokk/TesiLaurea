import pyshark
import json
import time

def extract_goose_from_pcap(pcap_file, output_json):
    """
    Estrae i campi GOOSE da un file PCAP e li salva in JSON.
    """
    # Il filtro display di Wireshark 'goose' identifica direttamente i pacchetti GOOSE
    cap = pyshark.FileCapture(pcap_file, display_filter='goose')
    
    goose_records = []
    
    for pkt in cap:
        try:
            record = {
                # Informazioni di base sul pacchetto
                "frame_number": pkt.frame_info.number,
                "timestamp": pkt.frame_info.time,
                "src_mac": pkt.eth.src,
                "dst_mac": pkt.eth.dst,
                
                # Campi principali di GOOSE
                "gocb_ref": getattr(pkt.goose, 'gocbRef', None),
                "go_id": getattr(pkt.goose, 'goID', None),
                "dat_set": getattr(pkt.goose, 'datSet', None),
                "app_id": getattr(pkt.goose, 'appid', None),
                "time_allowed_to_live": getattr(pkt.goose, 'timeAllowedtoLive', None),
                "st_num": getattr(pkt.goose, 'stNum', None),
                "sq_num": getattr(pkt.goose, 'sqNum', None),
                "simulation": getattr(pkt.goose, 'simulation', None),
                "conf_rev": getattr(pkt.goose, 'confRev', None),
                "nds_com": getattr(pkt.goose, 'ndsCom', None),
                "num_dat_set_entries": getattr(pkt.goose, 'numDatSetEntries', None),
                "timestamp_goose": getattr(pkt.goose, 't', None),  # timestamp interno GOOSE
            }
            goose_records.append(record)
        except AttributeError as e:
            print(f"Avviso: il pacchetto {pkt.frame_info.number} non contiene un campo atteso: {e}")
            continue
    
    cap.close()
    
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(goose_records, f, indent=2, ensure_ascii=False)
    
    print(f"Estratti {len(goose_records)} pacchetti GOOSE, salvati in {output_json}")
    return goose_records


def extract_goose_live(interface, duration_sec=60, output_json="goose_live.json"):
    """
    Acquisisce GOOSE in tempo reale da un'interfaccia di rete.
    """
    print(f"Inizio acquisizione GOOSE in tempo reale dall'interfaccia {interface} per {duration_sec} secondi...")
    
    # Utilizza il filtro BPF per l'acquisizione in tempo reale, intercetta solo il traffico GOOSE
    cap = pyshark.LiveCapture(interface=interface, bpf_filter='ether proto 0x88b8')
    
    goose_records = []
    
    # Utilizza il callback per elaborare i pacchetti e interrompere dopo il timeout
    start_time = time.time()
    
    def handle_packet(pkt):
        if time.time() - start_time > duration_sec:
            raise KeyboardInterrupt("Acquisizione completata")
        
        try:
            record = {
                "frame_number": pkt.frame_info.number,
                "timestamp": pkt.frame_info.time,
                "src_mac": pkt.eth.src,
                "dst_mac": pkt.eth.dst,
                "gocb_ref": getattr(pkt.goose, 'gocbRef', None),
                "go_id": getattr(pkt.goose, 'goID', None),
                "dat_set": getattr(pkt.goose, 'datSet', None),
                "app_id": getattr(pkt.goose, 'appid', None),
                "st_num": getattr(pkt.goose, 'stNum', None),
                "sq_num": getattr(pkt.goose, 'sqNum', None),
                "timestamp_goose": getattr(pkt.goose, 't', None),
            }
            goose_records.append(record)
            print(f"GOOSE: stNum={record['st_num']}, sqNum={record['sq_num']}, gocbRef={record['gocb_ref']}")
        except AttributeError:
            pass
    
    try:
        cap.apply_on_packets(handle_packet, timeout=duration_sec)
    except KeyboardInterrupt:
        pass
    finally:
        cap.close()
    
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(goose_records, f, indent=2, ensure_ascii=False)
    
    print(f"Acquisizione in tempo reale completata, estratti {len(goose_records)} pacchetti GOOSE")
    return goose_records


if __name__ == "__main__":
    import sys
    
    if len(sys.argv) < 2:
        print("Utilizzo:")
        print("  Da file: python goose_extractor.py file.pcapng output.json")
        print("  In tempo reale: python goose_extractor.py --live eth0 60 output.json")
        sys.exit(1)
    
    if sys.argv[1] == "--live":
        interface = sys.argv[2] if len(sys.argv) > 2 else "eth0"
        duration = int(sys.argv[3]) if len(sys.argv) > 3 else 60
        output = sys.argv[4] if len(sys.argv) > 4 else "goose_live.json"
        extract_goose_live(interface, duration, output)
    else:
        pcap_file = sys.argv[1]
        output = sys.argv[2] if len(sys.argv) > 2 else "goose_output.json"
        extract_goose_from_pcap(pcap_file, output)