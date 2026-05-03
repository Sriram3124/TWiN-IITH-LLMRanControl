Startup sequence (run in this order):

1. setup_ssh_tunnel.sh
   ssh -N -L 11434:localhost:11434 user@GPU_SERVER_IP

2. start_cn5g.sh
   docker compose -f configs/docker-compose-oai-cn5g.yaml up -d
   Wait for: AMF Ready

3. start_gnb.sh
   cd openairinterface5g/cmake_targets/ran_build/build
   sudo ./nr-softmodem -O gnb.conf --rfsim
   Wait for: UE registration messages

4. start_flexric.sh
   ./nearRT-RIC
   Wait for: E2 Setup Request received

5. start_ues.sh
   Starts UE1 and UE2 rfsimulator instances
   Wait for: PDU Session Established

6. start_iperf.sh
   iperf3 UDP downlink 30 Mbps per UE toward each UE

7. start_xapp.sh
   ./src/xapp_kpm_rc
   Wait for: KPM subscription confirmed

8. Run orchestrator
   python3 src/intent_controller_new3.py --map-rnti
   Then: python3 src/intent_controller_new3.py --intent "your intent here"
