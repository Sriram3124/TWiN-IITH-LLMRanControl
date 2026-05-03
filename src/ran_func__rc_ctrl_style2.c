/*
 * E2SM-RC Control Style 2 — PRB Quota application
 * Writes xApp-commanded PRB quota into target UE scheduling context.
 */
#include "ran_func_rc_ctrl_style2.h"
#include "common/ran_context.h"
#include "common/utils/LOG/log.h"
#include "openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h"

extern RAN_CONTEXT_t RC;

int e2sm_rc_apply_prb_quota(uint16_t rnti,
                             uint8_t  min_prb_pct,
                             uint8_t  max_prb_pct)
{
  /* Validate inputs */
  if (min_prb_pct > 100 || max_prb_pct > 100 || min_prb_pct > max_prb_pct) {
    LOG_E(NR_MAC,
          "[E2SM-RC S2] Invalid quota: min=%u%% max=%u%% RNTI=0x%04x\n",
          min_prb_pct, max_prb_pct, rnti);
    return RC_S2_ERR_INVALID;
  }

  if (!RC.nrmac || !RC.nrmac[0]) {
    LOG_E(NR_MAC, "[E2SM-RC S2] MAC not ready\n");
    return RC_S2_ERR_NO_MAC;
  }

  gNB_MAC_INST *mac = RC.nrmac[0];

  /* Find UE by RNTI and write quota */
  NR_UE_info_t *target = NULL;
  UE_iterator(mac->UE_info.connected_ue_list, ue) {
    if (ue && ue->rnti == rnti) {
      target = ue;
      break;
    }
  }

  if (!target) {
    LOG_W(NR_MAC,
          "[E2SM-RC S2] RNTI 0x%04x not in MAC table\n", rnti);
    return RC_S2_ERR_NO_UE;
  }

  if (max_prb_pct == 100 && min_prb_pct == 0) {
    /* Restore normal PF scheduling — disable quota */
    target->UE_sched_ctrl.rc_prb_quota_set = false;
    target->UE_sched_ctrl.rc_min_prb_pct   = 0;
    target->UE_sched_ctrl.rc_max_prb_pct   = 100;
  } else {
    /* Apply PRB quota */
    target->UE_sched_ctrl.rc_min_prb_pct   = min_prb_pct;
    target->UE_sched_ctrl.rc_max_prb_pct   = max_prb_pct;
    target->UE_sched_ctrl.rc_prb_quota_set  = true;
  }
 
 

  if (max_prb_pct == 100 && min_prb_pct == 0) {
    LOG_I(NR_MAC,
          "[E2SM-RC S2] Quota CLEARED: RNTI=0x%04x — normal PF scheduling restored\n",
          rnti);
  } else {
    LOG_I(NR_MAC,
          "[E2SM-RC S2] Quota SET: RNTI=0x%04x min=%u%% max=%u%%\n",
          rnti, min_prb_pct, max_prb_pct);
  }
  return RC_S2_OK;
}
