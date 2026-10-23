#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ConIntr(dos_far_ptr rq);
void PrnIntr(dos_far_ptr rq);
void AuxIntr(dos_far_ptr rq);
void Lpt1Intr(dos_far_ptr rq);
void Lpt2Intr(dos_far_ptr rq);
void Lpt3Intr(dos_far_ptr rq);
void Com2Intr(dos_far_ptr rq);
void Com3Intr(dos_far_ptr rq);
void Com4Intr(dos_far_ptr rq);
void ClkEntry(dos_far_ptr rq);
void BlkEntry(dos_far_ptr rq);
void NulIntr(dos_far_ptr rq);

#if defined(ORANGE386)
/* Values stored in dhdr.arm.dh_interrupt (see hdr/device.h). */
enum native_dev_id {
    NATIVE_DEV_NONE = 0,
    NATIVE_DEV_ConIntr,
    NATIVE_DEV_PrnIntr,
    NATIVE_DEV_AuxIntr,
    NATIVE_DEV_Lpt1Intr,
    NATIVE_DEV_Lpt2Intr,
    NATIVE_DEV_Lpt3Intr,
    NATIVE_DEV_Com2Intr,
    NATIVE_DEV_Com3Intr,
    NATIVE_DEV_Com4Intr,
    NATIVE_DEV_ClkEntry,
    NATIVE_DEV_BlkEntry,
    NATIVE_DEV_NulIntr,
    NATIVE_DEV_COUNT
};
#define NATIVE_DEV_INTR(fn) NATIVE_DEV_##fn

typedef void (*native_dev_intr_t)(dos_far_ptr rq);
/* NULL for an unknown id. */
native_dev_intr_t native_dev_intr_lookup(UDWORD id);
#else
#define NATIVE_DEV_INTR(fn) fn
#endif

/* Return non-zero when dhp names an ATTR_NATIVE device and the request was
 * dispatched.  status receives the request's resulting r_status. */
int fdos_native_execrh(dos_far_ptr dhp, dos_far_ptr rq, UWORD *status);

/* Read only the two x86 entry offsets required by x86_execrh(). */
void fdos_x86_dhdr_entries(dos_far_ptr dhp, UWORD *strategy, UWORD *interrupt);

#ifdef __cplusplus
}
#endif
