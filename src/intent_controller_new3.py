#!/usr/bin/env python3
"""
Intent Controller — Part 5
Full pipeline: English intent → LLM → JSON → xApp → gNB scheduler
"""
import subprocess
import threading
import requests
import re
import time
import json
import os
import sys

PIPE       = "/tmp/rc_control_pipe"
GNB_LOG    = "/tmp/gnb.log"
OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL      = "qwen3:14b"
XAPP_BINARY = os.path.expanduser("~/flexric/build/examples/xApp/c/kpm_rc/xapp_kpm_rc")
XAPP_DIR    = os.path.expanduser("~/flexric")

SYSTEM_PROMPT = """You are a 5G network resource manager.
A cellular base station serves 2 UEs sharing 106 Physical Resource Blocks (PRBs).
PRBs are the fundamental radio resources — more PRBs means higher throughput for that UE.
Current measured throughput: UE1 = {measured_ue1:.0f} Mbps, UE2 = {measured_ue2:.0f} Mbps.
Maximum achievable throughput per UE is approximately 50 Mbps when given all PRBs.

The operator will describe a situation or intent in plain English.
Your job is to decide what percentage of PRBs each UE should receive,
based on what each UE needs given the application context and relative priority.
"Normal scheduling" or "restore" means equal allocation: 50% each UE.
Think about:
- What application is each UE likely running?
- How sensitive is that application to bandwidth reduction?
- What is the minimum acceptable allocation for each UE?
- What is the relative priority given the operator's description?

OUTPUT FORMAT — one line raw JSON, nothing else, no markdown:
{{"prb_max_pct_ue1": <int 5-95>, "prb_min_pct_ue1": <int 0-90>, "prb_max_pct_ue2": <int 5-95>, "prb_min_pct_ue2": <int 0-90>, "reasoning": "<one sentence explaining your decision>"}}

CONSTRAINTS:
- prb_max_pct_ue1 + prb_max_pct_ue2 should not exceed 100
- prb_max_pct must be strictly greater than prb_min_pct for each UE
- Use your own judgment — reason from the application context
- Do not apply fixed mappings — think about what each situation genuinely requires"""

RULE_OVERRIDES = {}

ue_rnti_map   = {}
ue_throughput = {}
xapp_proc     = None
is_typing     = False


def watch_xapp(proc):
    reg_pattern  = re.compile(r'\[KPM\] Registered (UE\d+) = RNTI 0x([0-9a-fA-F]+)')
    tput_pattern = re.compile(r'(UE\d+)\(0x[0-9a-fA-F]+\)=([\d.]+)\s*kbps')
    for line in proc.stdout:
        line = line.strip()
        if "[KPM]" in line or "[PIPE]" in line or "[RC]" in line:
            if not is_typing:
                ts = time.strftime("%H:%M:%S")
                print(f"  [{ts}]  {line}")
        m = reg_pattern.search(line)
        if m:
            label = m.group(1)
            rnti  = int(m.group(2), 16)
            ue_rnti_map[label] = rnti
        for m in tput_pattern.finditer(line):
            ue_throughput[m.group(1)] = float(m.group(2))


def get_mac_rntis():
    """
    Get actual MAC C-RNTIs from gNB MCS_TRACE log.
    Returns [rnti_ue1, rnti_ue2] sorted by FIRST appearance in log.
    First appearance = first UE to connect = physical UE1.
    """
    first_seen = {}
    try:
        with open(GNB_LOG, 'r') as f:
            for lineno, line in enumerate(f):
                m = re.search(r'MCS_TRACE.*UE\s+([0-9a-fA-F]{4})', line)
                if m:
                    rnti = int(m.group(1), 16)
                    if rnti > 10 and rnti not in first_seen:
                        first_seen[rnti] = lineno
                    if len(first_seen) >= 2:
                        break
    except FileNotFoundError:
        print(f"[CTRL] ❌ {GNB_LOG} not found")
        return []
    except Exception as e:
        print(f"[CTRL] Error reading gNB log: {e}")
        return []
    sorted_rntis = sorted(first_seen.items(), key=lambda x: x[1])
    return [rnti for rnti, _ in sorted_rntis]


def query_llm(intent):
    measured_ue1 = ue_throughput.get("UE1", 29000) / 1000
    measured_ue2 = ue_throughput.get("UE2", 29000) / 1000

    filled_prompt = SYSTEM_PROMPT.format(
        measured_ue1=measured_ue1,
        measured_ue2=measured_ue2
    )

    prompt = filled_prompt + f'\n\nIntent: "{intent}" /no_think'

    try:
        t_start = time.time()
        response = requests.post(
            OLLAMA_URL,
            json={
                "model": MODEL,
                "prompt": prompt,
                "stream": False,
                "think": False, 
                "options": {"temperature": 0, "num_predict": 400}
            },
            timeout=30
        )
        #return response.json()["response"].strip()
        t_llm_ms = (time.time() -t_start) * 1000
        print(f"[CTRL] LLM inference time: {t_llm_ms: .0f} ms")
        raw = response.json()["response"].strip()
# Strip Qwen3 thinking block if present
        if "<think>" in raw:
            import re as _re
            raw = _re.sub(r'<think>.*?</think>', '', raw, flags=_re.DOTALL).strip()
        return raw
    except requests.exceptions.ConnectionError:
        print("[CTRL] ❌ Cannot reach Ollama at localhost:11434")
        return None

    except requests.exceptions.Timeout:
        print("[CTRL] ❌ Ollama request timed out after 30s")
        return None

    except Exception as e:
        print(f"[CTRL] ❌ Ollama error: {e}")
        return None


def parse_llm_output(raw):
    if not raw:
        return None, "Empty response"
    raw = re.sub(r'```json\s*', '', raw)
    raw = re.sub(r'```\s*', '', raw)
    match = re.search(r'\{.*\}', raw, re.DOTALL)
    if not match:
        return None, "No JSON object found in response"
    json_str = match.group(0)
    json_str = json_str.replace("'", '"')
    json_str = re.sub(r',\s*}', '}', json_str)
    json_str = re.sub(r',\s*]', ']', json_str)
    try:
        data = json.loads(json_str)
        return data, None
    except json.JSONDecodeError as e:
        return None, f"JSON parse error: {e}\nRaw: {json_str[:100]}"


def validate_policy(data):
    required = ["prb_max_pct_ue1", "prb_min_pct_ue1", "prb_max_pct_ue2", "prb_min_pct_ue2"]
    for field in required:
        if field not in data:
            return False, f"Missing field: {field}"
    try:
        for field in required:
            data[field] = int(data[field])
    except (ValueError, TypeError) as e:
        return False, f"Non-integer value: {e}"
    for field in required:
        if not 0 <= data[field] <= 100:
            return False, f"{field}={data[field]} out of range [0,100]"
    if data["prb_min_pct_ue1"] >= data["prb_max_pct_ue1"]:
        return False, f"UE1: min({data['prb_min_pct_ue1']}) >= max({data['prb_max_pct_ue1']})"
    if data["prb_min_pct_ue2"] >= data["prb_max_pct_ue2"]:
        return False, f"UE2: min({data['prb_min_pct_ue2']}) >= max({data['prb_max_pct_ue2']})"
    return True, None


def check_overrides(intent):
    intent_lower = intent.lower()
    for keyword, values in RULE_OVERRIDES.items():
        if keyword in intent_lower:
            return values
    return None

def run_feedback_loop(ue1_max, ue1_min, ue2_max, ue2_min, rnti_ue1, rnti_ue2):
    """
    Closed-loop PRB controller.
    Adjusts PRB quota every 3s until measured KPM throughput
    ratio matches the intended PRB allocation ratio.
    Convergence = actual throughput ratio within 5% of target.
    """
    # Target ratio from LLM decision
    total_intended = ue1_max + ue2_max
    if total_intended == 0:
        return
    target_ratio_ue1 = ue1_max / total_intended

    current_max_ue1 = ue1_max
    current_max_ue2 = ue2_max

    TOLERANCE    = 0.05   # 5% ratio tolerance
    MAX_ITER     = 8      # max iterations
    LOOP_INTERVAL = 3     # seconds between adjustments
    GAIN         = 25     # how aggressively to adjust PRB %

    print(f"\n[LOOP] Starting feedback loop")
    print(f"[LOOP] Target ratio — UE1: {target_ratio_ue1:.0%}  UE2: {1-target_ratio_ue1:.0%}")
    print(f"[LOOP] Initial PRB  — UE1: {current_max_ue1}%  UE2: {current_max_ue2}%")
    print(f"[LOOP] {'Iter':>4}  {'UE1 Mbps':>9}  {'UE2 Mbps':>9}  {'Ratio UE1':>10}  {'PRB UE1':>8}  {'PRB UE2':>8}  {'Error':>7}")
    print(f"[LOOP] " + "-"*70)

    meas_ue1 = meas_ue2 = 0.0

    for iteration in range(1, MAX_ITER + 1):

        # Apply current PRB allocation
        send_pipe(rnti_ue1, ue1_min, current_max_ue1)
        time.sleep(0.5)
        send_pipe(rnti_ue2, ue2_min, current_max_ue2)

        # Wait for scheduler to enforce and KPM to update
        time.sleep(LOOP_INTERVAL)

        # Read KPM measurements
        meas_ue1 = ue_throughput.get("UE1", 0) / 1000   # kbps → Mbps
        meas_ue2 = ue_throughput.get("UE2", 0) / 1000

        total_meas = meas_ue1 + meas_ue2
        if total_meas < 1.0:
            print(f"[LOOP] {iteration:>4}  No throughput yet, waiting...")
            continue

        actual_ratio_ue1 = meas_ue1 / total_meas
        ratio_error = target_ratio_ue1 - actual_ratio_ue1

        print(f"[LOOP] {iteration:>4}  {meas_ue1:>8.1f}  {meas_ue2:>9.1f}  "
              f"{actual_ratio_ue1:>9.0%}  {current_max_ue1:>8}%  "
              f"{current_max_ue2:>8}%  {ratio_error:>+7.2f}")

        # Check convergence
        if abs(ratio_error) < TOLERANCE:
            print(f"[LOOP] ✅ Converged at iteration {iteration}!")
            break

        # Proportional adjustment — increase UE1 PRB if below target ratio
        adjustment = int(ratio_error * GAIN)
        current_max_ue1 = max(10, min(90, current_max_ue1 + adjustment))
        current_max_ue2 = max(10, min(90, current_max_ue2 - adjustment))
        # Ensure total never exceeds 95 to avoid gNB PRB assertion crash
        total = current_max_ue1 + current_max_ue2
        if total > 95:
            scale = 95 / total
            current_max_ue1 = int(current_max_ue1 * scale)
            current_max_ue2 = int(current_max_ue2 * scale)

    else:
        print(f"[LOOP] ⚠️  Did not converge in {MAX_ITER} iterations")

    print(f"[LOOP] Final — UE1: {meas_ue1:.1f} Mbps (PRB={current_max_ue1}%)  "
          f"UE2: {meas_ue2:.1f} Mbps (PRB={current_max_ue2}%)")
def send_pipe(rnti, min_pct, max_pct):
    cmd = json.dumps({"rnti": rnti, "min_pct": min_pct, "max_pct": max_pct})
    try:
        with open(PIPE, "w") as f:
            f.write(cmd + "\n")
        return True
    except Exception as e:
        print(f"[CTRL] ❌ Pipe error: {e}")
        return False


def process_intent(intent, rnti_ue1, rnti_ue2):
    ts = time.strftime("%H:%M:%S")
    print(f"\n[{ts}] [CTRL] Processing: \"{intent}\"")
    override = check_overrides(intent)
    if override:
        ue1_max, ue1_min, ue2_max, ue2_min = override
        print(f"[CTRL] Rule override applied (known LLM edge case)")
        reasoning = f"Rule-based override for: {intent}"
    else:
        global is_typing
        is_typing = True
        print(f"[CTRL] Querying LLM ({MODEL} on GPU)...")
        raw = query_llm(intent)
        is_typing = False
        if raw is None:
            print("[CTRL] ❌ LLM query failed — intent not applied")
            return False
        print(f"[CTRL] LLM raw response: {raw[:150]}")
        data, err = parse_llm_output(raw)
        if data is None:
            print(f"[CTRL] ❌ Parse failed: {err}")
            return False
        valid, verr = validate_policy(data)
        if not valid:
            print(f"[CTRL] ❌ Validation failed: {verr}")
            print(f"[CTRL] Applying safe fallback: 50/50 balance")
            ue1_max, ue1_min, ue2_max, ue2_min = 50, 5, 50, 5
            reasoning = "Fallback due to invalid LLM output"
        else:
            ue1_max = data["prb_max_pct_ue1"]
            ue1_min = data["prb_min_pct_ue1"]
            ue2_max = data["prb_max_pct_ue2"]
            ue2_min = data["prb_min_pct_ue2"]
            reasoning = data.get("reasoning", "")

    print(f"\n[CTRL] ═══════════════════════════════════════")
    print(f"[CTRL] Decision:")
    print(f"[CTRL]   UE1: min={ue1_min}%  max={ue1_max}%")
    print(f"[CTRL]   UE2: min={ue2_min}%  max={ue2_max}%")
    print(f"[CTRL]   Reasoning: {reasoning}")
    print(f"[CTRL] ═══════════════════════════════════════")

    total_prbs = 106
    ue1_prbs = (total_prbs * ue1_max) // 100
    ue2_prbs = (total_prbs * ue2_max) // 100
    print(f"[CTRL] PRBs: UE1≤{ue1_prbs}  UE2≤{ue2_prbs}  (of 106 total)")

   # print(f"[CTRL] Sending to gNB scheduler...")
   # ok1 = send_pipe(rnti_ue1, ue1_min, ue1_max)
   # time.sleep(1.0)
   # ok2 = send_pipe(rnti_ue2, ue2_min, ue2_max)
   # if ok1 and ok2:
       # print(f"[CTRL] ✅ Controls sent — enforced from next TTI (1ms)")
        #print(f"[CTRL] Watch KPM throughput lines below...")
   # else:
        #print(f"[CTRL] ❌ Pipe send failed")
        #return False
    #return True
    print(f"[CTRL] Sending initial policy to gNB scheduler...")
    ok1 = send_pipe(rnti_ue1, ue1_min, ue1_max)
    time.sleep(0.5)
    ok2 = send_pipe(rnti_ue2, ue2_min, ue2_max)
    if not (ok1 and ok2):
        print(f"[CTRL] ❌ Pipe send failed")
        return False

    print(f"[CTRL] ✅ Initial policy sent — starting feedback loop...")
    run_feedback_loop(ue1_max, ue1_min, ue2_max, ue2_min, rnti_ue1, rnti_ue2)
    return True 

def main():
    global xapp_proc

    print("""
╔══════════════════════════════════════════════════════╗
║         5G RAN Intent Controller — Part 5           ║
║  English Intent → LLM → E2SM-RC → MAC Scheduler    ║
╚══════════════════════════════════════════════════════╝
""")

    print("[CTRL] Checking Ollama connection...")
    try:
        r = requests.get("http://localhost:11434/api/tags", timeout=5)
        models = [m["name"] for m in r.json().get("models", [])]
        if MODEL not in models and not any(MODEL.split(":")[0] in m for m in models):
            print(f"[CTRL] ❌ Model {MODEL} not found. Available: {models}")
            sys.exit(1)
        print(f"[CTRL] ✅ Ollama connected — model {MODEL} ready")
    except Exception:
        print("[CTRL] ❌ Cannot reach Ollama at localhost:11434")
        print("       ssh -L 11434:localhost:11434 -N -f <user>@<gpu_ip>")
        sys.exit(1)

    if not os.path.exists(GNB_LOG):
        print(f"[CTRL] ❌ {GNB_LOG} not found")
        sys.exit(1)
    print(f"[CTRL] ✅ gNB log found at {GNB_LOG}")

    print("[CTRL] Starting xApp...")
    xapp_proc = subprocess.Popen(
        ["stdbuf", "-oL", XAPP_BINARY],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=XAPP_DIR
    )
    print(f"[CTRL] ✅ xApp started (PID {xapp_proc.pid})")

    t = threading.Thread(target=watch_xapp, args=(xapp_proc,), daemon=True)
    t.start()

    print("[CTRL] Waiting for UEs via KPM (max 30s)...")
    for i in range(30):
        if len(ue_rnti_map) >= 2:
            break
        time.sleep(1)
        if i % 5 == 0 and i > 0:
            print(f"[CTRL] {i}s — UEs seen: {len(ue_rnti_map)}")

    if len(ue_rnti_map) < 2:
        print("[CTRL] ❌ UEs not discovered via KPM")
        xapp_proc.terminate()
        sys.exit(1)
    print("[CTRL] ✅ KPM discovered both UEs")

    print("[CTRL] Reading MAC C-RNTIs from gNB MCS_TRACE log...")
    time.sleep(2)
    mac_rntis = get_mac_rntis()

    if len(mac_rntis) < 2:
        print("[CTRL] ❌ Cannot find 2 MAC C-RNTIs in gNB log. Are UEs attached?")
        xapp_proc.terminate()
        sys.exit(1)

    rnti_ue1 = mac_rntis[0]
    rnti_ue2 = mac_rntis[1]
    print(f"[CTRL] ✅ MAC C-RNTIs (by connection order):")
    print(f"[CTRL]   UE1 = 0x{rnti_ue1:04x}  (connected first)")
    print(f"[CTRL]   UE2 = 0x{rnti_ue2:04x}  (connected second)")

    print("[CTRL] Waiting for xApp pipe...")
    for i in range(20):
        if os.path.exists(PIPE):
            print("[CTRL] ✅ Pipe ready")
            break
        time.sleep(1)
    else:
        print("[CTRL] ❌ Pipe not created after 20s")
        xapp_proc.terminate()
        sys.exit(1)

    time.sleep(3)
    print("\n[CTRL] Current throughput:")
    for label, kbps in ue_throughput.items():
        print(f"[CTRL]   {label} = {kbps/1000:.1f} Mbps")

    print("""
[CTRL] ══════════════════════════════════════════════
[CTRL] READY — Enter your intent in plain English
[CTRL] Examples:
[CTRL]   "Maximize the throughput of UE1"
[CTRL]   "UE2 is doing an emergency video call"
[CTRL]   "Give 70 percent of resources to UE1"
[CTRL]   "Balance both users equally"
[CTRL]   "Restore normal scheduling"
[CTRL] Type 'quit' or press Ctrl+C to exit
[CTRL] ══════════════════════════════════════════════
""")

    try:
        while True:
            global is_typing
            is_typing = True
            sys.stdout.write("\nIntent > ")
            sys.stdout.flush()
            try:
                with open('/dev/tty', 'r') as tty:
                    intent = tty.readline().strip()
            except Exception:
                intent = input("").strip()
            is_typing = False

            if not intent:
                continue
            if intent.lower() in ("quit", "exit", "q"):
                print("[CTRL] Exiting...")
                break

            process_intent(intent, rnti_ue1, rnti_ue2)

            time.sleep(3)
            print("\n[CTRL] Updated throughput:")
            for label, kbps in ue_throughput.items():
                print(f"[CTRL]   {label} = {kbps/1000:.1f} Mbps")

    except KeyboardInterrupt:
        print("\n[CTRL] Ctrl+C received — shutting down")

    finally:
        is_typing = False
        print("[CTRL] Restoring equal sharing before exit...")
        try:
            send_pipe(rnti_ue1, 0, 100)
            time.sleep(0.3)
            send_pipe(rnti_ue2, 0, 100)
        except Exception:
            pass
        if xapp_proc:
            xapp_proc.terminate()
        print("[CTRL] Done.")


if __name__ == "__main__":
    main()
ENDOFFILE
