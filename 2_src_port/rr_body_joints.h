#ifndef RR_BODY_JOINTS_H
#define RR_BODY_JOINTS_H
#include "rr_cham_types.h"
/* Attachment helpers remain available in the rig palette, but are not body
 * joints. A parked weapon helper can sit at the character origin. */
static int rr_rig_is_body_joint(int bone) {
    return bone>=0 && bone<RR_CHAM_BONES &&
           bone!=RR_RIG_WEAPONE1_JNT && bone!=RR_RIG_MAGAZINE1_JNT &&
           bone!=RR_RIG_BAG_JNT && bone!=RR_RIG_L_BAG_JNT && bone!=RR_RIG_R_BAG_JNT;
}

/* Skeleton display filter. The facial joints (eyelids, eyes, lips, nose,
 * cheeks, brows, jaw, tongue) draw a dense web across the face, so only the
 * centre face bone (M_HEAD_2) and the head link are kept. Aim targeting uses
 * rr_rig_is_body_joint and is unaffected. */
static int rr_rig_is_skeleton_joint(int bone) {
    if (!rr_rig_is_body_joint(bone)) return 0;
    if (bone > RR_RIG_M_HEAD_2_JNT && bone <= RR_RIG_M_TONGUE_3_JNT) return 0;
    return 1;
}
#endif
