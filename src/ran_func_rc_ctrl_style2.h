#ifndef RAN_FUNC_RC_CTRL_STYLE2_H
#define RAN_FUNC_RC_CTRL_STYLE2_H

#include <stdint.h>
#include <stdbool.h>

/* RAN Parameter IDs for Style 2 / Action 6 */
#define RC_S2_ACT6_PARAM_MIN_PRB   1
#define RC_S2_ACT6_PARAM_MAX_PRB   2
#define RC_S2_ACT6_PARAM_DED_PRB   3
#define RC_S2_ACT6_PARAM_RNTI      4   /* IMPORTANT */



/* Return codes */
#define RC_S2_OK            0
#define RC_S2_ERR_INVALID  -1
#define RC_S2_ERR_NO_MAC   -2
#define RC_S2_ERR_NO_UE    -3

int e2sm_rc_apply_prb_quota(uint16_t rnti,
                           uint8_t  min_prb_pct,
                           uint8_t  max_prb_pct);

#endif
