#ifndef __QER_H__
#define __QER_H__

#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/net.h>

#include "dev.h"
#include "qos_meter.h"
#include "wred.h"

struct qer {
    struct hlist_node hlist_id;
    u64 seid;
    u32 id;
    uint8_t ul_dl_gate;
    struct {
        uint32_t ul_high;
        uint8_t ul_low;
        uint32_t dl_high;
        uint8_t dl_low;
    } mbr;
    struct {
        uint32_t ul_high;
        uint8_t ul_low;
        uint32_t dl_high;
        uint8_t dl_low;
    } gbr;
    uint32_t qer_corr_id;
    uint8_t rqi;
    uint8_t qfi;
    uint8_t ppi;
    uint8_t rcsr;
    struct net_device *dev;
    struct rcu_head rcu_head;
    // For color marking
    struct trtcm_param meter_param;
    struct trtcm_profile meter_profile;
    struct trtcm_runtime meter_runtime;
    // For rule class queue
    struct wred_profile queue_profile;
};

extern void wred_profile_config(struct wred_profile *, struct qer *, uint64_t);
extern void qer_context_delete(struct qer *);
extern struct qer *find_qer_by_id(struct gtp5g_dev *, u64, u32);
extern void qer_update(struct qer *, struct gtp5g_dev *);
extern void qer_append(u64, u32, struct qer *, struct gtp5g_dev *);
extern int qer_get_pdr_ids(u16 *, int, struct qer *, struct gtp5g_dev *);
extern void qer_set_pdr(u64, u32 *, u32, struct hlist_node *, struct gtp5g_dev *);

#endif // __QER_H__
