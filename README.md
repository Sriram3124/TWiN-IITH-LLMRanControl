
# TWiN-IITH-LLMRanControl

## LLM-Driven Intent-Based 5G RAN Control

> **English Intent → LLM → E2SM-RC Style 2 → OAI gNB MAC Scheduler**
>
> TWiN Course Final Project · IIT Hyderabad · MTech CSE · Sriram Dharmarajan

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Repository Structure](#3-repository-structure)
4. [Prerequisites](#4-prerequisites)
5. [Setup Instructions](#5-setup-instructions)
6. [How to Run](#6-how-to-run)
7. [Experiment Reproduction](#7-experiment-reproduction)
8. [Results Summary](#8-results-summary)
9. [LLM Usage Disclosure](#9-llm-usage-disclosure)
10. [References](#10-references)

---

## 1. Project Overview

This project implements a complete, end-to-end system that allows a **non-expert network operator** to type a plain-English sentence describing UE application context and have that intent **enforced directly at the 5G gNB MAC scheduler**, with measured warm end-to-end latency of approximately 1.76 seconds in the demo setup, and scheduler-side enforcement checked every 1 ms TTI.

### What This System Does

```
Operator types:  "UE2 is a surgeon performing a remote robotic operation"
                          ↓
LLM reasons:     UE2 needs high, stable BW (life-critical). UE1 deprioritised.
                          ↓
Policy sent:     UE1: min=20%, max=40%  |  UE2: min=60%, max=70%
                          ↓
MAC enforces:    UE2 = 30.9 Mbps (PRB=70%)  |  UE1 = 19.9 Mbps (PRB=40%)
                 [RC-QUOTA] lines appear in gNB log every 1ms ✓
```

### Key Contributions

| Contribution | Details |
|---|---|
| **Open-source prototype of E2SM-RC Style 2 based per-UE PRB quota control in OAI** | 7 OAI source files modified to enforce per-UE PRB quotas at pf_dl() per TTI |
| **Zero-rule LLM intent translation** | qwen3:14b achieves 90% pass rate on 31 intents with no hardcoded domain rules |
| **Closed-loop KPM feedback controller** | Proportional controller (Kp=0.25) converges to target throughput ratio within 5% |
| **Real 5G stack validation** | Full OAI NR protocol stack — PHY, MAC, RLC, PDCP, RRC, NAS all execute |

### Model Comparison Study Result

| Configuration | Pass Rate | Key Finding |
|---|---|---|
| llama3:8b + 24 hardcoded rules (mid-term) | 96% | Rules did the work, not the LLM |
| llama3:8b + zero rules | 77% | 19-point drop proves rule dependency |
| qwen3:14b + zero rules (final) | **90%** | Genuine world-knowledge reasoning |

---

## 2. System Architecture

```
OPERATOR TERMINAL → PYTHON ORCHESTRATOR (intent_controller_new3.py)
                              ↓ SSH tunnel          ↓ Named Pipe
                         LLM qwen3:14b          C xApp (xapp_kpm_rc.c)
                         RTX A6000 GPU          E2SM-RC Style 2
                                                      ↓ E2 Interface
                                               FlexRIC (near-RT RIC)
                                                      ↓
                                     OAI gNB (rfsim) — pf_dl() MAC Scheduler
                                     [RC-QUOTA] logged every 1ms TTI
                                     UE1 (iperf UDP)   UE2 (iperf UDP)
                                                      ↓ N2/N3
                                              OAI CN5G (Docker)
                                              AMF · SMF · UPF
```

---

## 3. Repository Structure

```
TWiN-IITH-LLMRanControl/
│
├── README.md
├── requirements.txt                   ← Python stdlib only, no external packages
│
├── src/                               ← All source code
│   ├── intent_controller_new3.py      ← Main Python orchestrator
│   │                                     (referred to as intent_controller_new.py in report)
│   ├── xapp_kpm_rc.c                  ← Custom C xApp (KPM + RC)
│   ├── nr_mac_gNB.h                   ← Modified OAI: rc_max/min_prb_pct fields added
│   ├── gNB_scheduler_dlsch.c          ← Modified OAI: pf_dl() PRB capping + RC-QUOTA logging
│   ├── ran_func_rc.c                  ← Modified OAI: E2SM-RC Style 2 control handler
│   ├── ran_func_rc_ctrl_style2.c      ← New file: e2sm_rc_apply_prb_quota() implementation
│   ├── ran_func_rc_ctrl_style2.h      ← New file: header for above
│   ├── ran_func_rc.h                  ← Modified OAI: updated declarations
│   └── ran_func_rc_extern.h           ← OAI extern references
│
├── configs/
│   └── config_notes.txt               ← gNB, LLM, and controller parameters
│
├── experiments/
│   └── intent_suite.json              ← 31 test intents with expected outputs and pass/fail
│
├── results/
│   ├── plot1_per_ue_throughput.png    ← Per-UE throughput for 8 intents vs baseline
│   ├── plot2_convergence.png          ← LLM target vs KPM-measured ratio
│   ├── plot3_ablation_study.png       ← Model comparison study (96/77/90%)
│   └── plot4_latency_breakdown.png    ← End-to-end latency breakdown
│
├── scripts/
│   └── README.txt                     ← Step-by-step startup sequence (8 steps)
│                                        Note: documentation only, not executable scripts
│
├── demo/
│   ├── gnb_rc_quota_enforcement.png
│   ├── online_exam_intent.png
│   ├── icu_monitoring_intent.png
│   └── equal_video_call_intent.png
│
└── docs/
    ├── TWiN_Final_Report.pdf
    └── TWiN_Final_Presentation.pdf
```

---

## 4. Prerequisites

### Hardware

| Component | Requirement |
|---|---|
| Laptop / Workstation | Ubuntu 22.04 LTS, ≥16 GB RAM |
| GPU Server | NVIDIA GPU ≥24 GB VRAM (tested: RTX A6000 48 GB) |
| Network | SSH access from laptop to GPU server |

### Software

```bash
# Ubuntu dependencies
sudo apt-get install -y git cmake build-essential libsctp-dev \
    libgnutls28-dev libgcrypt-dev libssl-dev libidn11-dev \
    libconfig-dev python3 iperf3

# Docker
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER

# On GPU Server — Ollama + models
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen3:14b
ollama pull llama3:8b
```

---

## 5. Setup Instructions

### Step 1 — Clone Repository

```bash
git clone https://github.com/Sriram3124/TWiN-IITH-LLMRanControl.git
cd TWiN-IITH-LLMRanControl
```

### Step 2 — Clone and Patch OAI gNB

```bash
git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git
cd openairinterface5g && git checkout develop

# Set your project directory path
PROJECT_DIR=/path/to/TWiN-IITH-LLMRanControl

# Copy modified files into OAI source tree
# Note: verify exact filename capitalization matches your OAI build tree
cp $PROJECT_DIR/src/nr_mac_gNB.h              openair2/LAYER2/NR_MAC_gNB/
cp $PROJECT_DIR/src/gNB_scheduler_dlsch.c     openair2/LAYER2/NR_MAC_gNB/
cp $PROJECT_DIR/src/ran_func_rc.c             openair2/E2AP/flexric/src/agent/
cp $PROJECT_DIR/src/ran_func_rc_ctrl_style2.c openair2/E2AP/flexric/src/agent/
cp $PROJECT_DIR/src/ran_func_rc_ctrl_style2.h openair2/E2AP/flexric/src/agent/
cp $PROJECT_DIR/src/ran_func_rc.h             openair2/E2AP/flexric/src/agent/
cp $PROJECT_DIR/src/ran_func_rc_extern.h      openair2/E2AP/flexric/src/agent/

# Build gNB
cd cmake_targets
./build_oai -I --gNB -x --build-lib "telnetsrv" -w SIMU --ninja
```

### Step 3 — Clone and Build FlexRIC

```bash
git clone https://gitlab.eurecom.fr/mosaic5g/flexric.git
cd flexric && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install
```

### Step 4 — Pull OAI CN5G (Core Network)

```bash
git clone https://github.com/OPENAIRINTERFACE/oai-cn5g.git
cd oai-cn5g
docker compose up -d
# Wait until: AMF Ready appears in Docker logs
```

### Step 5 — Build the Custom C xApp

```bash
cd TWiN-IITH-LLMRanControl/src
gcc xapp_kpm_rc.c -o xapp_kpm_rc \
    $(pkg-config --cflags --libs flexric) \
    -lpthread -lm
```

---

## 6. How to Run

Full step-by-step sequence is in `scripts/README.txt` (documentation only — provides the exact manual startup sequence used in the demo). Summary:

```bash
# 1. SSH tunnel to GPU server
ssh -N -L 11434:localhost:11434 user@GPU_SERVER_IP

# 2. Start CN5G core network (inside oai-cn5g/)
docker compose up -d   # wait for AMF Ready

# 3. Start OAI gNB
sudo ./nr-softmodem -O gnb.conf --rfsim

# 4. Start FlexRIC
./nearRT-RIC

# 5. Start UEs (UE1 and UE2 rfsimulator instances)

# 6. Start iperf traffic (30 Mbps UDP per UE)

# 7. Start xApp
./src/xapp_kpm_rc

# 8. Run orchestrator — RNTI mapping first, then intent
python3 src/intent_controller_new3.py --map-rnti
python3 src/intent_controller_new3.py \
    --intent "UE2 is a surgeon performing a remote robotic operation"
```

### Expected Output

```
[CTRL] LLM inference time: 1751 ms
[CTRL] Decision: UE1: min=20% max=40% | UE2: min=60% max=70%
[LOOP] Converged at iteration 1!
[LOOP] Final — UE1: 19.9 Mbps (PRB=40%)  UE2: 30.9 Mbps (PRB=70%)
```

**In gNB log simultaneously:**

```
[NR_MAC] [RC-QUOTA] RNTI 0xd37e: capped rbSize 106 → 5 (max=5%)
[NR_MAC] [RC-QUOTA] RNTI 0xe3df: capped rbSize 101 → 100 (max=95%)
```

---

## 7. Experiment Reproduction

### Single Intent (5 minutes)

```bash
python3 src/intent_controller_new3.py \
    --intent "UE1 is a student giving an online exam right now"
# Expected: UE1 ~32.7 Mbps (PRB=70%), UE2 ~13.4 Mbps (PRB=30%)
```

### LLM Standalone Test (no OAI stack needed)

```bash
# Only requires SSH tunnel to GPU server running
curl http://localhost:11434/api/generate -d '{
  "model": "qwen3:14b",
  "prompt": "UE2 is monitoring a patient in an ICU remotely. Output JSON only: {\"prb_max_pct_ue1\":...,\"prb_min_pct_ue1\":...,\"prb_max_pct_ue2\":...,\"prb_min_pct_ue2\":...,\"reasoning\":\"...\"}",
  "stream": false
}'
# Expected: prb_max_pct_ue2 >= 60, prb_max_pct_ue1 <= 40
```

### Full 31-Intent Suite

See `experiments/intent_suite.json` — contains all 31 intents with expected priority labels and pass/fail results across 5 categories (medical, professional, educational, infrastructure, symmetric).

---

## 8. Results Summary

| Intent | UE1 | UE2 | Baseline |
|---|---|---|---|
| Maximize UE2 | 13.7 Mbps | 31.3 Mbps | 29/29 Mbps |
| Emergency video (UE1) | 34.9 Mbps | 14.4 Mbps | 29/29 Mbps |
| Give 70% to UE1 | 31.7 Mbps | 13.1 Mbps | 29/29 Mbps |
| Restore normal | 25.5 Mbps | 25.2 Mbps | 29/29 Mbps |
| Radiologist (UE1) | 32.2 Mbps | 13.3 Mbps | 29/29 Mbps |
| Surgeon (UE2) | 13.8 Mbps | 31.4 Mbps | 29/29 Mbps |
| Cloud backup (UE1) | 34.9 Mbps | 14.4 Mbps | 29/29 Mbps |
| Job interview (UE2) | 17.1 Mbps | 25.9 Mbps | 29/29 Mbps |

All 8 intents converged at iteration 1. Max ratio error: 3.4%. E2 enforcement latency: <12 ms.
Quantitative plots are in `results/`.

---

## 9. LLM Usage Disclosure

| LLM | Purpose |
|---|---|
| qwen3:14b (Ollama, RTX A6000) | Production intent translation — zero rules, Temperature=0 |
| llama3:8b (Ollama, RTX A6000) | Model comparison study baseline |
| Claude (Anthropic) | Debugging and error resolution only — compilation errors, BWP crash, pipe IPC bugs |

Sections where debugging assistance was applied are marked inline:

```c
// Bug fix — debugging assisted — TWiN Project
```

---

## 10. References

1. O-RAN Alliance, E2SM-RC v03.00, 2023
2. O-RAN Alliance, E2SM-KPM v03.00, 2023
3. S. D'Oro et al., FlexRIC, ACM CoNEXT 2022
4. Z. Ali et al., AutoRAN, IEEE INFOCOM 2024
5. 3GPP TS 38.214, Release 17
6. OpenAirInterface: https://gitlab.eurecom.fr/oai/openairinterface5g
7. Qwen Team, Qwen3 Technical Report, Alibaba Cloud, April 2025

---

**Author:** Sriram Dharmarajan · MTech CSE · IIT Hyderabad  

**Course:** Topics in Wireless Networks (TWiN) · 2026  

**GitHub:** https://github.com/Sriram3124
