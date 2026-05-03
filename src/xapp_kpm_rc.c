/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "../../../../src/xApp/e42_xapp_api.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_struct.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_list.h"
#include "../../../../src/util/time_now_us.h"
#include "../../../../src/util/alg_ds/ds/lock_guard/lock_guard.h"
#include "../../../../src/util/e.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
/* ── Per-UE tracking (supports up to 2 UEs for demo) ── */
#define MAX_UES 2

typedef struct {
  ue_id_e2sm_t ue_id;
  uint16_t     rnti;           /* C-RNTI extracted from ran_ue_id     */
  double       dl_tput_kbps;   /* latest DRB.UEThpDl from KPM         */
  bool         valid;          /* true once this slot is populated     */
} tracked_ue_t;

static tracked_ue_t tracked_ues[MAX_UES] = {0};
static int          n_tracked_ues        = 0;

static uint64_t const period_ms = 100;
static pthread_mutex_t mtx;

/* Named pipe path — Python orchestrator writes commands here */
#define CTRL_PIPE_PATH "/tmp/rc_control_pipe"

static
void log_gnb_ue_id(ue_id_e2sm_t ue_id)
{
  if (ue_id.gnb.gnb_cu_ue_f1ap_lst != NULL) {
    for (size_t i = 0; i < ue_id.gnb.gnb_cu_ue_f1ap_lst_len; i++) {
      printf("UE ID type = gNB-CU, gnb_cu_ue_f1ap = %u\n", ue_id.gnb.gnb_cu_ue_f1ap_lst[i]);
    }
  } else {
    printf("UE ID type = gNB, amf_ue_ngap_id = %lu\n", ue_id.gnb.amf_ue_ngap_id);
  }
  if (ue_id.gnb.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb.ran_ue_id); // RAN UE NGAP ID
  }
}

static
void log_du_ue_id(ue_id_e2sm_t ue_id)
{
  printf("UE ID type = gNB-DU, gnb_cu_ue_f1ap = %u\n", ue_id.gnb_du.gnb_cu_ue_f1ap);
  if (ue_id.gnb_du.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb_du.ran_ue_id); // RAN UE NGAP ID
  }
}

static
void log_cuup_ue_id(ue_id_e2sm_t ue_id)
{
  printf("UE ID type = gNB-CU-UP, gnb_cu_cp_ue_e1ap = %u\n", ue_id.gnb_cu_up.gnb_cu_cp_ue_e1ap);
  if (ue_id.gnb_cu_up.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb_cu_up.ran_ue_id); // RAN UE NGAP ID
  }
}

typedef void (*log_ue_id)(ue_id_e2sm_t ue_id);

static
log_ue_id log_ue_id_e2sm[END_UE_ID_E2SM] = {
    log_gnb_ue_id, // common for gNB-mono, CU and CU-CP
    log_du_ue_id,
    log_cuup_ue_id,
    NULL,
    NULL,
    NULL,
    NULL,
};

static
void log_int_value(byte_array_t name, meas_record_lst_t meas_record)
{
  if (cmp_str_ba("RRU.PrbTotDl", name) == 0) {
    printf("RRU.PrbTotDl = %d [PRBs]\n", meas_record.int_val);
  } else if (cmp_str_ba("RRU.PrbTotUl", name) == 0) {
    printf("RRU.PrbTotUl = %d [PRBs]\n", meas_record.int_val);
  } else if (cmp_str_ba("DRB.PdcpSduVolumeDL", name) == 0) {
    printf("DRB.PdcpSduVolumeDL = %d [kb]\n", meas_record.int_val);
  } else if (cmp_str_ba("DRB.PdcpSduVolumeUL", name) == 0) {
    printf("DRB.PdcpSduVolumeUL = %d [kb]\n", meas_record.int_val);
  } else {
    printf("Measurement Name not yet supported\n");
  }
}

static
void log_real_value(byte_array_t name, meas_record_lst_t meas_record)
{
  if (cmp_str_ba("DRB.RlcSduDelayDl", name) == 0) {
    printf("DRB.RlcSduDelayDl = %.2f [μs]\n", meas_record.real_val);
  } else if (cmp_str_ba("DRB.UEThpDl", name) == 0) {
    printf("DRB.UEThpDl = %.2f [kbps]\n", meas_record.real_val);
  } else if (cmp_str_ba("DRB.UEThpUl", name) == 0) {
    printf("DRB.UEThpUl = %.2f [kbps]\n", meas_record.real_val);
  } else {
    printf("Measurement Name not yet supported\n");
  }
}

typedef void (*log_meas_value)(byte_array_t name, meas_record_lst_t meas_record);

static
log_meas_value get_meas_value[END_MEAS_VALUE] = {
    log_int_value,
    log_real_value,
    NULL,
};

static
void match_meas_name_type(meas_type_t meas_type, meas_record_lst_t meas_record)
{
  // Get the value of the Measurement
  get_meas_value[meas_record.value](meas_type.name, meas_record);
}

static
void match_id_meas_type(meas_type_t meas_type, meas_record_lst_t meas_record)
{
  (void)meas_type;
  (void)meas_record;
  assert(false && "ID Measurement Type not yet supported");
}

typedef void (*check_meas_type)(meas_type_t meas_type, meas_record_lst_t meas_record);

static
check_meas_type match_meas_type[END_MEAS_TYPE] = {
    match_meas_name_type,
    match_id_meas_type,
};

static
void log_kpm_measurements(kpm_ind_msg_format_1_t const* msg_frm_1)
{
  assert(msg_frm_1->meas_info_lst_len > 0 && "Cannot correctly print measurements");

  // UE Measurements per granularity period
  for (size_t j = 0; j < msg_frm_1->meas_data_lst_len; j++) {
    meas_data_lst_t const data_item = msg_frm_1->meas_data_lst[j];

    for (size_t z = 0; z < data_item.meas_record_len; z++) {
      meas_type_t const meas_type = msg_frm_1->meas_info_lst[z].meas_type;
      meas_record_lst_t const record_item = data_item.meas_record_lst[z];

      match_meas_type[meas_type.type](meas_type, record_item);

      if (data_item.incomplete_flag && *data_item.incomplete_flag == TRUE_ENUM_VALUE)
        printf("Measurement Record not reliable");
    }
  }

}
static
void sm_cb_kpm(sm_ag_if_rd_t const* rd)
{
  assert(rd != NULL);
  assert(rd->type == INDICATION_MSG_AGENT_IF_ANS_V0);
  assert(rd->ind.type == KPM_STATS_V3_0);

  kpm_ind_data_t const* ind        = &rd->ind.kpm.ind;
  kpm_ind_msg_format_3_t const* f3 = &ind->msg.frm_3;

  static int counter = 1;

  lock_guard(&mtx);

  for (size_t i = 0; i < f3->ue_meas_report_lst_len; i++) {
    ue_id_e2sm_t const uid = f3->meas_report_per_ue[i].ue_meas_report_lst;

    /* Extract RNTI from ran_ue_id — it is uint64_t* (optional field) */
    uint16_t rnti = 0;
    if (uid.type == GNB_UE_ID_E2SM && uid.gnb.ran_ue_id != NULL)
      rnti = (uint16_t)(*uid.gnb.ran_ue_id);

    /* Find or register this UE in our tracking table */
    int slot = -1;
    for (int j = 0; j < n_tracked_ues; j++) {
      if (tracked_ues[j].rnti == rnti) { slot = j; break; }
    }
    if (slot < 0 && n_tracked_ues < MAX_UES) {
      slot = n_tracked_ues++;
      free_ue_id_e2sm(&tracked_ues[slot].ue_id);
      tracked_ues[slot].ue_id = cp_ue_id_e2sm(&uid);
      tracked_ues[slot].rnti  = rnti;
      tracked_ues[slot].valid = true;
      printf("[KPM] Registered UE%d = RNTI 0x%04x\n", slot + 1, rnti);
    }

    /* Extract DRB.UEThpDl from measurements */
    kpm_ind_msg_format_1_t const* f1 =
        &f3->meas_report_per_ue[i].ind_msg_format_1;

    for (size_t j = 0; j < f1->meas_data_lst_len; j++) {
      for (size_t z = 0; z < f1->meas_data_lst[j].meas_record_len; z++) {
        meas_type_t      const mt  = f1->meas_info_lst[z].meas_type;
        meas_record_lst_t const mr = f1->meas_data_lst[j].meas_record_lst[z];

        if (mt.type == NAME_MEAS_TYPE
            && cmp_str_ba("DRB.UEThpDl", mt.name) == 0
            && mr.value == REAL_MEAS_VALUE
            && slot >= 0) {
          tracked_ues[slot].dl_tput_kbps = mr.real_val;
        }
      }
    }
  }

  /* Print live throughput every 10 reports */
  if (counter % 10 == 0) {
    printf("[KPM] ");
    for (int j = 0; j < n_tracked_ues; j++)
      printf("UE%d(0x%04x)=%.1f kbps  ",
             j+1, tracked_ues[j].rnti,
             tracked_ues[j].dl_tput_kbps);
    printf("\n");
  }
  counter++;
}


typedef enum {
  DRB_QoS_Configuration_7_6_2_1 = 1,
  QoS_flow_mapping_configuration_7_6_2_1 = 2,
  Logical_channel_configuration_7_6_2_1 = 3,
  Radio_admission_control_7_6_2_1 = 4,
  DRB_termination_control_7_6_2_1 = 5,
  DRB_split_ratio_control_7_6_2_1 = 6,
  PDCP_Duplication_control_7_6_2_1 = 7,
} rc_ctrl_service_style_1_e;

typedef enum {
  DRB_ID_8_4_2_2 = 1,
  LIST_OF_QOS_FLOWS_MOD_IN_DRB_8_4_2_2 = 2,
  QOS_FLOW_ITEM_8_4_2_2 = 3,
  QOS_FLOW_ID_8_4_2_2 = 4,
  QOS_FLOW_MAPPING_IND_8_4_2_2 = 5,
} qos_flow_mapping_conf_e;

static
seq_ran_param_t fill_drb_id_param(void)
{
  seq_ran_param_t drb_param = {0};

  drb_param.ran_param_id = DRB_ID_8_4_2_2;
  drb_param.ran_param_val.type = ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE;
  drb_param.ran_param_val.flag_true = calloc(1, sizeof(ran_parameter_value_t));
  assert(drb_param.ran_param_val.flag_true != NULL && "Memory exhausted");
  // Let's suppose that it is the DRB 5
  drb_param.ran_param_val.flag_true->type = INTEGER_RAN_PARAMETER_VALUE;
  drb_param.ran_param_val.flag_true->int_ran = 5;

  return drb_param;
}

static
seq_ran_param_t fill_qos_flows_param(void)
{
  seq_ran_param_t qos_param = {0};

  qos_param.ran_param_id = LIST_OF_QOS_FLOWS_MOD_IN_DRB_8_4_2_2;
  qos_param.ran_param_val.type = LIST_RAN_PARAMETER_VAL_TYPE;
  qos_param.ran_param_val.lst = calloc(1, sizeof(ran_param_list_t));
  assert(qos_param.ran_param_val.lst != NULL && "Memory exhausted");
  ran_param_list_t* rpl = qos_param.ran_param_val.lst;

  rpl->sz_lst_ran_param = 1;
  rpl->lst_ran_param = calloc(1, sizeof(lst_ran_param_t));
  assert(rpl->lst_ran_param != NULL && "Memory exhausted");

  // QoS Flow Item
  // Bug in the standard. RAN Parameter List 9.3.13
  // has a mandatory ie RAN Parameter ID 9.3.8
  // and a mandatory ie RAN Parameter Structure 9.3.12
  // However, the ASN
  // RANParameter-LIST ::= SEQUENCE {
  // list-of-ranParameter  SEQUENCE (SIZE(1..maxnoofItemsinList)) OF RANParameter-STRUCTURE,
  // ..
  // }
  //
  // Misses RAN Parameter ID and only has RAN Parameter Structure

  // rpl->lst_ran_param[0].ran_param_id = QOS_FLOW_ITEM_8_4_2_2;

  rpl->lst_ran_param[0].ran_param_struct.sz_ran_param_struct = 2;
  rpl->lst_ran_param[0].ran_param_struct.ran_param_struct = calloc(2, sizeof(seq_ran_param_t));
  assert(rpl->lst_ran_param[0].ran_param_struct.ran_param_struct != NULL && "Memory exhausted");
  seq_ran_param_t* rps = rpl->lst_ran_param[0].ran_param_struct.ran_param_struct;

  // QoS Flow Identifier
  rps[0].ran_param_id = QOS_FLOW_ID_8_4_2_2;
  rps[0].ran_param_val.type = ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE;
  rps[0].ran_param_val.flag_true = calloc(1, sizeof(ran_parameter_value_t));
  assert(rps[0].ran_param_val.flag_true != NULL && "Memory exhausted");
  rps[0].ran_param_val.flag_true->type = INTEGER_RAN_PARAMETER_VALUE;
  // Let's suppose that we have QFI 10
  rps[0].ran_param_val.flag_true->int_ran = 10;

  // QoS Flow Mapping Indication
  rps[1].ran_param_id = QOS_FLOW_MAPPING_IND_8_4_2_2;
  rps[1].ran_param_val.type = ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  rps[1].ran_param_val.flag_false = calloc(1, sizeof(ran_parameter_value_t));
  assert(rps[1].ran_param_val.flag_false != NULL && "Memory exhausted");

  // ENUMERATED (ul, dl, ...)
  rps[1].ran_param_val.flag_false->type = INTEGER_RAN_PARAMETER_VALUE;
  rps[1].ran_param_val.flag_false->int_ran = 1;

  return qos_param;
}
/* ────────────────────────────────────────────────────────────
 * Style 2 / Action 6 — PRB Quota Setting
 * Builds an RC Control message for a specific UE RNTI.
 * ──────────────────────────────────────────────────────────── */
static
rc_ctrl_req_data_t fill_prb_quota_ctrl(uint16_t rnti,
                                        uint8_t  min_pct,
                                        uint8_t  max_pct)
{
  rc_ctrl_req_data_t rc_ctrl = {0};

  /* ── Header ── */
  rc_ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
  rc_ctrl.hdr.frmt_1.ric_style_type = 2;   /* Style 2 */
  rc_ctrl.hdr.frmt_1.ctrl_act_id    = 6;   /* Action 6: PRB Quota */

  /* Set UE ID — use ran_ue_id = RNTI for monolithic gNB */
  rc_ctrl.hdr.frmt_1.ue_id.type = GNB_UE_ID_E2SM;
  uint64_t *ran_id = calloc(1, sizeof(uint64_t));
  assert(ran_id != NULL);
  *ran_id = rnti;
  rc_ctrl.hdr.frmt_1.ue_id.gnb.ran_ue_id = ran_id;

  /* ── Message — 3 RAN parameters ── */
  rc_ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
  rc_ctrl.msg.frmt_1.sz_ran_param = 3;
  rc_ctrl.msg.frmt_1.ran_param    = calloc(3, sizeof(seq_ran_param_t));
  assert(rc_ctrl.msg.frmt_1.ran_param != NULL);

  /* Param 1 — Min PRB Policy Ratio */
  rc_ctrl.msg.frmt_1.ran_param[0].ran_param_id = 1;
  rc_ctrl.msg.frmt_1.ran_param[0].ran_param_val.type =
      ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  rc_ctrl.msg.frmt_1.ran_param[0].ran_param_val.flag_false =
      calloc(1, sizeof(ran_parameter_value_t));
  assert(rc_ctrl.msg.frmt_1.ran_param[0].ran_param_val.flag_false != NULL);
  rc_ctrl.msg.frmt_1.ran_param[0].ran_param_val.flag_false->type  = INTEGER_RAN_PARAMETER_VALUE;
  rc_ctrl.msg.frmt_1.ran_param[0].ran_param_val.flag_false->int_ran = min_pct;

  /* Param 2 — Max PRB Policy Ratio */
  rc_ctrl.msg.frmt_1.ran_param[1].ran_param_id = 2;
  rc_ctrl.msg.frmt_1.ran_param[1].ran_param_val.type =
      ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  rc_ctrl.msg.frmt_1.ran_param[1].ran_param_val.flag_false =
      calloc(1, sizeof(ran_parameter_value_t));
  assert(rc_ctrl.msg.frmt_1.ran_param[1].ran_param_val.flag_false != NULL);
  rc_ctrl.msg.frmt_1.ran_param[1].ran_param_val.flag_false->type  = INTEGER_RAN_PARAMETER_VALUE;
  rc_ctrl.msg.frmt_1.ran_param[1].ran_param_val.flag_false->int_ran = max_pct;

  /* Param 4 — Target C-RNTI (explicit in body for cross-check) */
  rc_ctrl.msg.frmt_1.ran_param[2].ran_param_id = 4;
  rc_ctrl.msg.frmt_1.ran_param[2].ran_param_val.type =
      ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  rc_ctrl.msg.frmt_1.ran_param[2].ran_param_val.flag_false =
      calloc(1, sizeof(ran_parameter_value_t));
  assert(rc_ctrl.msg.frmt_1.ran_param[2].ran_param_val.flag_false != NULL);
  rc_ctrl.msg.frmt_1.ran_param[2].ran_param_val.flag_false->type  = INTEGER_RAN_PARAMETER_VALUE;
  rc_ctrl.msg.frmt_1.ran_param[2].ran_param_val.flag_false->int_ran = rnti;

  return rc_ctrl;
}
static
void fill_rc_ctrl_act(seq_ctrl_act_2_t const* ctrl_act,
                             size_t const sz,
                             e2sm_rc_ctrl_hdr_frmt_1_t* hdr,
                             e2sm_rc_ctrl_msg_frmt_1_t* msg)
{
  assert(ctrl_act != NULL);

  for (size_t i = 0; i < sz; i++) {
    /* Handle both Style 1 Act 2 and Style 2 Act 6 */
    if (cmp_str_ba("QoS flow mapping configuration", ctrl_act[i].name) == 0) {
      hdr->ctrl_act_id = 2;
      msg->sz_ran_param = 2;
      msg->ran_param = calloc(msg->sz_ran_param, sizeof(seq_ran_param_t));
      assert(msg->ran_param != NULL && "Memory exhausted");
      msg->ran_param[0] = fill_drb_id_param();
      msg->ran_param[1] = fill_qos_flows_param();
    } else if (cmp_str_ba("PRB Quota Setting", ctrl_act[i].name) == 0) {
      /* Style 2 / Act 6 is handled via pipe_listener — skip here */
      printf("[RC] PRB Quota Setting action found in RAN func def\n");
    } else {
      printf("[RC] Unknown action '%.*s' — skipping\n",
             (int)ctrl_act[i].name.len, ctrl_act[i].name.buf);
    }
  }
}

static
rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const* ran_func)
{
  assert(ran_func != NULL);

  rc_ctrl_req_data_t rc_ctrl = {0};

  for (size_t i = 0; i < ran_func->sz_seq_ctrl_style; i++) {
    const char* style_name = (const char*)ran_func->seq_ctrl_style[i].name.buf;
    printf("[RC] Found control style: %.*s\n",
           (int)ran_func->seq_ctrl_style[i].name.len,
           style_name);

    /* Only send initial Style 1 control — Style 2 handled by pipe */
    if (cmp_str_ba("Radio Bearer Control", ran_func->seq_ctrl_style[i].name) == 0) {
      rc_ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
      rc_ctrl.hdr.frmt_1.ric_style_type = 1;
      {
        lock_guard(&mtx);
        if (tracked_ues[0].valid)
          rc_ctrl.hdr.frmt_1.ue_id = cp_ue_id_e2sm(&tracked_ues[0].ue_id);
      }
      rc_ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
      fill_rc_ctrl_act(ran_func->seq_ctrl_style[i].seq_ctrl_act,
                       ran_func->seq_ctrl_style[i].sz_seq_ctrl_act,
                       &rc_ctrl.hdr.frmt_1,
                       &rc_ctrl.msg.frmt_1);
    }
    /* Style 2 intentionally skipped here — pipe_listener handles it */
  }

  return rc_ctrl;
}

static
test_info_lst_t filter_predicate(test_cond_type_e type, test_cond_e cond, int value)
{
  test_info_lst_t dst = {0};

  dst.test_cond_type = type;
  // It can only be TRUE_TEST_COND_TYPE so it does not matter the type
  // but ugly ugly...
  dst.S_NSSAI = TRUE_TEST_COND_TYPE;

  dst.test_cond = calloc(1, sizeof(test_cond_e));
  assert(dst.test_cond != NULL && "Memory exhausted");
  *dst.test_cond = cond;

  dst.test_cond_value = calloc(1, sizeof(test_cond_value_t));
  assert(dst.test_cond_value != NULL && "Memory exhausted");
  dst.test_cond_value->type = OCTET_STRING_TEST_COND_VALUE;

  dst.test_cond_value->octet_string_value = calloc(1, sizeof(byte_array_t));
  assert(dst.test_cond_value->octet_string_value != NULL && "Memory exhausted");
//  const size_t len_nssai = 1;
 // dst.test_cond_value->octet_string_value->len = len_nssai;
//  dst.test_cond_value->octet_string_value->buf = calloc(len_nssai, sizeof(uint8_t));
//  assert(dst.test_cond_value->octet_string_value->buf != NULL && "Memory exhausted");
 // dst.test_cond_value->octet_string_value->buf[0] = value;
  /* Send SST (1 byte) + SD (3 bytes) = 4 bytes total */
  /* SD = 0x000001 matching UE config nssai_sd=1 */
  const size_t len_nssai = 4;
  dst.test_cond_value->octet_string_value->len = len_nssai;
  dst.test_cond_value->octet_string_value->buf = calloc(len_nssai, sizeof(uint8_t));
  assert(dst.test_cond_value->octet_string_value->buf != NULL && "Memory exhausted");
  dst.test_cond_value->octet_string_value->buf[0] = value;   /* SST = 1   */
  dst.test_cond_value->octet_string_value->buf[1] = 0x00;   /* SD byte 1 */
  dst.test_cond_value->octet_string_value->buf[2] = 0x00;   /* SD byte 2 */
  dst.test_cond_value->octet_string_value->buf[3] = 0x01;   /* SD byte 3 */  
  return dst;
}

static
label_info_lst_t fill_kpm_label(void)
{
  label_info_lst_t label_item = {0};

  label_item.noLabel = ecalloc(1, sizeof(enum_value_e));
  *label_item.noLabel = TRUE_ENUM_VALUE;

  return label_item;
}

static
kpm_act_def_format_1_t fill_act_def_frm_1(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);

  kpm_act_def_format_1_t ad_frm_1 = {0};

  size_t const sz = report_item->meas_info_for_action_lst_len;

  // [1, 65535]
  ad_frm_1.meas_info_lst_len = sz;
  ad_frm_1.meas_info_lst = calloc(sz, sizeof(meas_info_format_1_lst_t));
  assert(ad_frm_1.meas_info_lst != NULL && "Memory exhausted");

  for (size_t i = 0; i < sz; i++) {
    meas_info_format_1_lst_t* meas_item = &ad_frm_1.meas_info_lst[i];
    // 8.3.9
    // Measurement Name
    meas_item->meas_type.type = NAME_MEAS_TYPE;
    meas_item->meas_type.name = copy_byte_array(report_item->meas_info_for_action_lst[i].name);

    // [1, 2147483647]
    // 8.3.11
    meas_item->label_info_lst_len = 1;
    meas_item->label_info_lst = ecalloc(1, sizeof(label_info_lst_t));
    meas_item->label_info_lst[0] = fill_kpm_label();
  }

  // 8.3.8 [0, 4294967295]
  ad_frm_1.gran_period_ms = period_ms;

  // 8.3.20 - OPTIONAL
  ad_frm_1.cell_global_id = NULL;

#if defined KPM_V2_03 || defined KPM_V3_00
  // [0, 65535]
  ad_frm_1.meas_bin_range_info_lst_len = 0;
  ad_frm_1.meas_bin_info_lst = NULL;
#endif

  return ad_frm_1;
}

static
kpm_act_def_t fill_report_style_4(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);
  assert(report_item->act_def_format_type == FORMAT_4_ACTION_DEFINITION);

  kpm_act_def_t act_def = {.type = FORMAT_4_ACTION_DEFINITION};

  // Fill matching condition
  // [1, 32768]
  act_def.frm_4.matching_cond_lst_len = 1;
  act_def.frm_4.matching_cond_lst = calloc(act_def.frm_4.matching_cond_lst_len, sizeof(matching_condition_format_4_lst_t));
  assert(act_def.frm_4.matching_cond_lst != NULL && "Memory exhausted");
  // Filter connected UEs by S-NSSAI criteria
  test_cond_type_e const type = S_NSSAI_TEST_COND_TYPE; // CQI_TEST_COND_TYPE
  test_cond_e const condition = EQUAL_TEST_COND; // GREATERTHAN_TEST_COND
  int const value = 0x01;
  act_def.frm_4.matching_cond_lst[0].test_info_lst = filter_predicate(type, condition, value);

  // Fill Action Definition Format 1
  // 8.2.1.2.1
  act_def.frm_4.action_def_format_1 = fill_act_def_frm_1(report_item);

  return act_def;
}

typedef kpm_act_def_t (*fill_kpm_act_def)(ric_report_style_item_t const* report_item);

static
fill_kpm_act_def get_kpm_act_def[END_RIC_SERVICE_REPORT] = {
    NULL,
    NULL,
    NULL,
    fill_report_style_4,
    NULL,
};

static
kpm_sub_data_t gen_kpm_subs(kpm_ran_function_def_t const* ran_func)
{
  assert(ran_func != NULL);
  assert(ran_func->ric_event_trigger_style_list != NULL);

  kpm_sub_data_t kpm_sub = {0};

  // Generate Event Trigger
  assert(ran_func->ric_event_trigger_style_list[0].format_type == FORMAT_1_RIC_EVENT_TRIGGER);
  kpm_sub.ev_trg_def.type = FORMAT_1_RIC_EVENT_TRIGGER;
  kpm_sub.ev_trg_def.kpm_ric_event_trigger_format_1.report_period_ms = period_ms;

  // Generate Action Definition
  kpm_sub.sz_ad = 1;
  kpm_sub.ad = calloc(kpm_sub.sz_ad, sizeof(kpm_act_def_t));
  assert(kpm_sub.ad != NULL && "Memory exhausted");

  // Multiple Action Definitions in one SUBSCRIPTION message is not supported in this project
  // Multiple REPORT Styles = Multiple Action Definition = Multiple SUBSCRIPTION messages
  ric_report_style_item_t* const report_item = &ran_func->ric_report_style_list[0];
  ric_service_report_e const report_style_type = report_item->report_style_type;
  *kpm_sub.ad = get_kpm_act_def[report_style_type](report_item);

  return kpm_sub;
}

static
bool eq_sm(sm_ran_function_t const* elem, int const id)
{
  if (elem->id == id)
    return true;

  return false;
}

static
size_t find_sm_idx(sm_ran_function_t* rf, size_t sz, bool (*f)(sm_ran_function_t const*, int const), int const id)
{
  for (size_t i = 0; i < sz; i++) {
    if (f(&rf[i], id))
      return i;
  }

  assert(0 != 0 && "SM ID could not be found in the RAN Function List");
}

/* ────────────────────────────────────────────────────────────
 * Pipe command listener — runs in a background thread.
 * Python orchestrator writes JSON lines to /tmp/rc_control_pipe.
 * Format: {"rnti": 6699, "min_pct": 5, "max_pct": 70}
 * ──────────────────────────────────────────────────────────── */
static e2_node_connected_xapp_t* g_e2_node  = NULL;
static int                       g_RC_ran_fn = 3;

static
void* pipe_listener(void* arg)
{
  (void)arg;

  /* Create named pipe if it does not exist */
  mkfifo(CTRL_PIPE_PATH, 0666);
  printf("[PIPE] Listening on %s\n", CTRL_PIPE_PATH);

  while (1) {
    FILE* fp = fopen(CTRL_PIPE_PATH, "r");
    if (!fp) { sleep(1); continue; }

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
      uint32_t rnti    = 0;
      uint32_t min_pct = 0;
      uint32_t max_pct = 100;

      /* Parse JSON manually — no dependencies */
      if (sscanf(buf,
                 "{\"rnti\": %u, \"min_pct\": %u, \"max_pct\": %u}",
                 &rnti, &min_pct, &max_pct) == 3) {

        printf("[PIPE] CMD: RNTI=0x%04x min=%u%% max=%u%%\n",
               rnti, min_pct, max_pct);

        if (g_e2_node == NULL) {
          printf("[PIPE] E2 node not ready yet — dropping\n");
          continue;
        }

        rc_ctrl_req_data_t ctrl =
            fill_prb_quota_ctrl((uint16_t)rnti,
                                (uint8_t)min_pct,
                                (uint8_t)max_pct);

        control_sm_xapp_api(&g_e2_node->id, g_RC_ran_fn, &ctrl);
        free_rc_ctrl_req_data(&ctrl);

        printf("[PIPE] Control sent for RNTI=0x%04x\n", rnti);
      } else {
        printf("[PIPE] Could not parse: %s", buf);
      }
    }
    fclose(fp);
  }
  return NULL;
}

int main(int argc, char* argv[])
{
  fr_args_t args = init_fr_args(argc, argv);

  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  assert(nodes.len > 0);
  printf("[KPM RC]: Connected E2 nodes = %d\n", nodes.len);

  pthread_mutexattr_t attr = {0};
  int rc_ret = pthread_mutex_init(&mtx, &attr);
  assert(rc_ret == 0);

  sm_ans_xapp_t* hndl = calloc(nodes.len, sizeof(sm_ans_xapp_t));
  assert(hndl != NULL);

  ////////////
  // START KPM
  ////////////
  int const KPM_ran_function = 2;

  for (size_t i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];
    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, KPM_ran_function);
    assert(n->rf[idx].defn.type == KPM_RAN_FUNC_DEF_E);

    if (n->rf[idx].defn.kpm.ric_report_style_list != NULL) {
      kpm_sub_data_t kpm_sub = gen_kpm_subs(&n->rf[idx].defn.kpm);
      hndl[i] = report_sm_xapp_api(&n->id, KPM_ran_function, &kpm_sub, sm_cb_kpm);
      assert(hndl[i].success == true);
      free_kpm_sub_data(&kpm_sub);
    }
  }

  /* Wait for KPM to populate UE table before starting RC */
  printf("[KPM RC]: Waiting 5s for UEs to appear via KPM...\n");
  sleep(5);

  ////////////
  // START RC
  ////////////
  int const RC_ran_function = 3;

  for (size_t i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];
    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, RC_ran_function);
    assert(n->rf[idx].defn.type == RC_RAN_FUNC_DEF_E);

    /* Store E2 node pointer for pipe listener */
    g_e2_node  = n;
    g_RC_ran_fn = RC_ran_function;

    /* Send one initial Style 1 control to confirm RC path works */
    if (n->rf[idx].defn.rc.ctrl != NULL) {
      rc_ctrl_req_data_t rc_ctrl = gen_rc_ctrl_msg(n->rf[idx].defn.rc.ctrl);
      if (rc_ctrl.msg.frmt_1.ran_param != NULL) {
        control_sm_xapp_api(&n->id, RC_ran_function, &rc_ctrl);
        printf("[RC] Initial Style 1 control sent\n");
      }
      free_rc_ctrl_req_data(&rc_ctrl);
    }
  }

  /* Start pipe listener thread — accepts Style 2 PRB quota commands */
  pthread_t pipe_thread;
  int pt = pthread_create(&pipe_thread, NULL, pipe_listener, NULL);
  assert(pt == 0);
  printf("[KPM RC]: Pipe listener started. Write to %s to send PRB controls.\n",
         CTRL_PIPE_PATH);
  printf("[KPM RC]: Format: {\"rnti\": <decimal>, \"min_pct\": <0-100>, \"max_pct\": <0-100>}\n");
  printf("[KPM RC]: Example: echo '{\"rnti\": 6699, \"min_pct\": 5, \"max_pct\": 30}' > %s\n",
         CTRL_PIPE_PATH);

  /* Run forever — KPM prints throughput, pipe listener sends controls */
  printf("[KPM RC]: Running. Press Ctrl+C to stop.\n");
  while (1)
    sleep(1);

  /* Cleanup (unreachable in normal demo — Ctrl+C exits) */
  for (int i = 0; i < (int)nodes.len; ++i)
    if (hndl[i].success == true)
      rm_report_sm_xapp_api(hndl[i].u.handle);
  free(hndl);

  while (try_stop_xapp_api() == false)
    usleep(1000);

  free_e2_node_arr_xapp(&nodes);
  rc_ret = pthread_mutex_destroy(&mtx);
  assert(rc_ret == 0);

  printf("[KPM RC]: Done\n");
  return 0;
}
