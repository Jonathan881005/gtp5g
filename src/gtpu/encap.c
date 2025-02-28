#include <linux/version.h>
#include <linux/socket.h>
#include <linux/rculist.h>
#include <linux/udp.h>
#include <linux/gtp.h>

#include <net/ip.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>

#include "dev.h"
#include "link.h"
#include "encap.h"
#include "gtp.h"
#include "pdr.h"
#include "far.h"
#include "qer.h"
#include "urr.h"
#include "report.h"

#include "genl.h"
#include "genl_report.h"

#include "log.h"
#include "api_version.h"
#include "pktinfo.h"

#include "clk_freq.h"
#include "qos_meter.h"
#include "wred.h"
/* used to compatible with api with/without seid */
#define MSG_KOV_LEN 4

enum msg_type {
    TYPE_BUFFER = 1,
    TYPE_URR_REPORT,
    TYPE_BAR_INFO,
};

static void gtp5g_encap_disable_locked(struct sock *);
static int gtp5g_encap_recv(struct sock *, struct sk_buff *);
static int gtp1u_udp_encap_recv(struct gtp5g_dev *, struct sk_buff *);
static int gtp5g_rx(struct pdr *, struct sk_buff *, unsigned int, unsigned int);
static int gtp5g_fwd_skb_encap(struct sk_buff *, struct net_device *,
        unsigned int, struct pdr *, uint64_t);
static int unix_sock_send(struct pdr *, void *, u32, u32);
static int gtp5g_fwd_skb_ipv4(struct sk_buff *, 
    struct net_device *, struct gtp5g_pktinfo *, 
    struct pdr *, uint64_t);

struct sock *gtp5g_encap_enable(int fd, int type, struct gtp5g_dev *gtp){
    struct udp_tunnel_sock_cfg tuncfg = {NULL};
    struct socket *sock;
    struct sock *sk;
    int err;

    GTP5G_LOG(NULL, "enable gtp5g for the fd(%d) type(%d)\n", fd, type);

    sock = sockfd_lookup(fd, &err);
    if (!sock) {
        GTP5G_ERR(NULL, "Failed to find the socket fd(%d)\n", fd);
        return NULL;
    }

    if (sock->sk->sk_protocol != IPPROTO_UDP) {
        GTP5G_ERR(NULL, "socket fd(%d) is not a UDP\n", fd);
        sk = ERR_PTR(-EINVAL);
        goto out_sock;
    }

    lock_sock(sock->sk);
    if (sock->sk->sk_user_data) {
        GTP5G_ERR(NULL, "Failed to set sk_user_datat of socket fd(%d)\n", fd);
        sk = ERR_PTR(-EBUSY);
        goto out_sock;
    }

    sk = sock->sk;
    sock_hold(sk);

    tuncfg.sk_user_data = gtp;
    tuncfg.encap_type = type;
    tuncfg.encap_rcv = gtp5g_encap_recv;
    tuncfg.encap_destroy = gtp5g_encap_disable_locked;

    setup_udp_tunnel_sock(sock_net(sock->sk), sock, &tuncfg);

out_sock:
    release_sock(sock->sk);
    sockfd_put(sock);
    return sk;
}


void gtp5g_encap_disable(struct sock *sk)
{
    struct gtp5g_dev *gtp;

    if (!sk) {
        return;
    }

    lock_sock(sk);
    gtp = sk->sk_user_data;
    if (gtp) {
        gtp->sk1u = NULL;
        udp_sk(sk)->encap_type = 0;
        rcu_assign_sk_user_data(sk, NULL);
        sock_put(sk);
    }
    release_sock(sk);
}

static void gtp5g_encap_disable_locked(struct sock *sk)
{
    rtnl_lock();
    gtp5g_encap_disable(sk);
    rtnl_unlock();
}

static int gtp5g_encap_recv(struct sock *sk, struct sk_buff *skb)
{
    struct gtp5g_dev *gtp;
    int ret = 0;

    gtp = rcu_dereference_sk_user_data(sk);
    if (!gtp) {
        return 1;
    }

    switch (udp_sk(sk)->encap_type) {
    case UDP_ENCAP_GTP1U:
        ret = gtp1u_udp_encap_recv(gtp, skb);
        break;
    default:
        ret = -1; // Should not happen
    }

    switch (ret) {
    case 1:
        GTP5G_ERR(gtp->dev, "Pass up to the process\n");
        break;
    case 0:
        break;
    case -1:
        GTP5G_ERR(gtp->dev, "GTP packet has been dropped\n");
        kfree_skb(skb);
        ret = 0;
        break;
    }

    return ret;
}

static int gtp1c_handle_echo_req(struct sk_buff *skb, struct gtp5g_dev *gtp)
{
    struct gtpv1_hdr *req_gtp1;
    struct gtp1_hdr_opt *req_gtpOptHdr;

    struct gtpv1_echo_resp *gtp_pkt;

    struct rtable *rt;
    struct flowi4 fl4;
    struct iphdr *iph;

    __u8   flags = 0;
    __be16 seq_number = 0;

    req_gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));

    flags = req_gtp1->flags;
    if (flags & GTPV1_HDR_FLG_SEQ) {
         req_gtpOptHdr = (struct gtp1_hdr_opt *)(skb->data + sizeof(struct udphdr) 
                                                            + sizeof(struct gtpv1_hdr));
         seq_number = req_gtpOptHdr->seq_number;
    } else {
        GTP5G_ERR(gtp->dev, "GTP echo request shall bring sequence number\n");
        seq_number = 0;
    }

    pskb_pull(skb, skb->len);          

    gtp_pkt = skb_push(skb, sizeof(struct gtpv1_echo_resp));
    if (!gtp_pkt) {
        GTP5G_ERR(gtp->dev, "can not construct GTP Echo Response\n");
        return 1;
    }
    memset(gtp_pkt, 0, sizeof(struct gtpv1_echo_resp));

    /* gtp header*/
    gtp_pkt->gtpv1_h.flags = GTPV1 | GTPV1_HDR_FLG_SEQ;
    gtp_pkt->gtpv1_h.type = GTPV1_MSG_TYPE_ECHO_RSP;
    gtp_pkt->gtpv1_h.length =
        htons(sizeof(struct gtpv1_echo_resp) - sizeof(struct gtpv1_hdr));
    gtp_pkt->gtpv1_h.tid = 0;

    /* gtp opt header*/
    gtp_pkt->gtpv1_opt_h.seq_number = seq_number;

    /* gtp recovery*/
    gtp_pkt->recov.type_num = GTPV1_IE_RECOVERY;
    gtp_pkt->recov.cnt = 0;

    iph = ip_hdr(skb);
  
    rt = ip4_find_route(skb, iph, gtp->sk1u, gtp->dev, 
        iph->daddr ,
        iph->saddr, 
        &fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(gtp->dev, "no route for GTP echo response from %pI4\n", 
        &iph->saddr);
        return 1;
    }

    udp_tunnel_xmit_skb(rt, gtp->sk1u, skb,
                    fl4.saddr, fl4.daddr,
                    iph->tos,
                    ip4_dst_hoplimit(&rt->dst),
                    0,
                    htons(GTP1U_PORT), htons(GTP1U_PORT),
                    !net_eq(sock_net(gtp->sk1u),
                        dev_net(gtp->dev)),
                    false);

    return 0;
}

static int gtp1u_udp_encap_recv(struct gtp5g_dev *gtp, struct sk_buff *skb)
{
    unsigned int hdrlen = sizeof(struct udphdr) + sizeof(struct gtpv1_hdr);
    struct gtpv1_hdr *gtpv1;
    struct pdr *pdr;
    unsigned int pull_len = hdrlen;
    u8 gtp_type;
    u32 teid;

    if (!pskb_may_pull(skb, pull_len)) {
        GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", pull_len);
        return -1;
    }

    gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if ((gtpv1->flags >> 5) != GTP_V1) {
        GTP5G_ERR(gtp->dev, "GTP version is not v1: %#x\n",
            gtpv1->flags);
        return 1;
    }

    gtp_type = gtpv1->type;
    teid = gtpv1->tid;
    if (gtp_type == GTPV1_MSG_TYPE_ECHO_REQ) {
        GTP5G_INF(gtp->dev, "GTP-C message type is GTP echo request: %#x\n",
            gtp_type);

        return gtp1c_handle_echo_req(skb, gtp);
    }

    if (gtp_type != GTPV1_MSG_TYPE_TPDU && gtp_type != GTPV1_MSG_TYPE_EMARK) {
        GTP5G_ERR(gtp->dev, "GTP-U message type is not a TPDU or End Marker: %#x\n",
            gtp_type);
        return 1;
    }

    /** TS 29.281 Chapter 5.1 and Figure 5.1-1
     * GTP-U header at least 8 byte
     *
     * This field shall be present if and only if any one or more of the S, PN and E flags are set.
     * This field means seq number (2 Octect), N-PDU number (1 Octet) and  Next ext hdr type (1 Octet).
     *
     * TODO: Validate the Reserved flag set or not, if it is set then protocol error
     */
    if (gtpv1->flags & GTPV1_HDR_FLG_MASK) {
        u8 *ext_hdr = NULL;
        hdrlen += sizeof(struct gtp1_hdr_opt);
        pull_len = hdrlen;
        if (!pskb_may_pull(skb, pull_len)) {
            GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", pull_len);
            return -1;
        }

        /** TS 29.281 Chapter 5.2 and Figure 5.2.1-1
         * The length of the Extension header shall be defined in a variable length of 4 octets,
         * i.e. m+1 = n*4 octets, where n is a positive integer.
         */
        while (*(ext_hdr = (u8 *)(skb->data + hdrlen - 1))) {
            u8 ext_hdr_type = *ext_hdr;
            pull_len = hdrlen + 1; // 1 byte for the length of extension hdr
            if (!pskb_may_pull(skb, pull_len)) {
                GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", pull_len);
                return -1;
            }
            hdrlen += (*((u8 *)(skb->data + hdrlen))) * 4; // total length of extension hdr
            pull_len = hdrlen;
            if (!pskb_may_pull(skb, pull_len)) {
                GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", pull_len);
                return -1;
            }
            switch (ext_hdr_type) {
                case GTPV1_NEXT_EXT_HDR_TYPE_85:
                {
                    // ext_pdu_sess_ctr_t *etype85 = (ext_pdu_sess_ctr_t *) (skb->data + hdrlen);
                    // pdu_sess_ctr_t *pdu_sess_info = &etype85->pdu_sess_ctr;

                    // Commented the below code due to support N9 packet downlink
                    // if (pdu_sess_info->type_spare == PDU_SESSION_INFO_TYPE0)
                    //     return -1;
            
                    //TODO: validate pdu_sess_ctr
                    break;
                }
            }
        }
    }

    pdr = pdr_find_by_gtp1u(gtp, skb, hdrlen, teid, gtp_type);
    if (!pdr) {
        GTP5G_ERR(gtp->dev, "No PDR match this skb : teid[%d]\n", ntohl(teid));
        return -1;
    }

    return gtp5g_rx(pdr, skb, hdrlen, gtp->role);
}

static int gtp5g_drop_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    struct pdr *pdr)
{
    struct gtpv1_hdr *gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if (gtp1->type == GTPV1_MSG_TYPE_TPDU) {
        pdr->ul_drop_cnt++;
        GTP5G_INF(NULL, "PDR (%u) UL_DROP_CNT (%llu)", pdr->id, pdr->ul_drop_cnt);
    }
    dev_kfree_skb(skb);
    return 0;
}

static int gtp5g_buf_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    unsigned int hdrlen, struct pdr *pdr)
{
    struct gtpv1_hdr *gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if (gtp1->type == GTPV1_MSG_TYPE_TPDU) {
        // Get rid of the GTP-U + UDP headers.
        if (iptunnel_pull_header(skb,
                hdrlen,
                skb->protocol,
                !net_eq(sock_net(pdr->sk), dev_net(dev)))) {
            GTP5G_ERR(dev, "Failed to pull GTP-U and UDP headers\n");
            return -1;
        }

        if (unix_sock_send(pdr, skb->data, skb_headlen(skb), 0) < 0) {
            GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
            ++pdr->ul_drop_cnt;
        }
    }
    dev_kfree_skb(skb);
    return 0;
}

/* Function unix_sock_{...} are used to handle buffering */
// Send PDR ID, FAR action and buffered packet to user space
static int unix_sock_send(struct pdr *pdr, void *buf, u32 len, u32 report_num)
{
    struct msghdr msg;
    struct kvec *kov;

    int msg_kovlen;
    int total_kov_len = 0;
    int i, rt;
    u8  type_hdr[1] = {TYPE_BUFFER};
    u64 self_seid_hdr[1] = {pdr->seid};
    u16 self_hdr[2] = {pdr->id, pdr->far->action};
    u32 self_num_hdr[1] = {report_num};

    if (!pdr->sock_for_buf) {
        GTP5G_ERR(NULL, "Failed Socket buffer is NULL\n");
        return -EINVAL;
    }

    memset(&msg, 0, sizeof(msg));
    if (report_num > 0) {
        type_hdr[0] = TYPE_URR_REPORT;
    }

    msg_kovlen = MSG_KOV_LEN;
    kov = kmalloc_array(msg_kovlen, sizeof(struct kvec),
        GFP_KERNEL);
    if (kov == NULL)
        return -ENOMEM;

    memset(kov, 0, sizeof(struct kvec) * msg_kovlen);

    kov[0].iov_base = type_hdr;
    kov[0].iov_len = sizeof(type_hdr);
    kov[1].iov_base = self_seid_hdr;
    kov[1].iov_len = sizeof(self_seid_hdr);

    //buf is report in this case
    if (report_num > 0) {
        kov[2].iov_base = self_num_hdr;
        kov[2].iov_len = sizeof(self_num_hdr);
    }
    else {
        kov[2].iov_base = self_hdr;
        kov[2].iov_len = sizeof(self_hdr);
    }

    kov[3].iov_base = buf;
    kov[3].iov_len = len;

    for (i = 0; i < msg_kovlen; i++)
        total_kov_len += kov[i].iov_len;

    msg.msg_name = 0;
    msg.msg_namelen = 0;
    iov_iter_kvec(&msg.msg_iter, WRITE, kov, msg_kovlen, total_kov_len);
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = MSG_DONTWAIT;

    rt = sock_sendmsg(pdr->sock_for_buf, &msg);
    kfree(kov);

    return rt;
}

bool increment_and_check_counter(struct VolumeMeasurement *volmeasure, struct Volume *volume, u64 vol, bool uplink, bool mnop){
    if (!volmeasure) {
        return false;
    }

    if (mnop) {
        if (uplink) {
            volmeasure->uplinkPktNum++;
        } else {
            volmeasure->downlinkPktNum++;
        }
        volmeasure->totalPktNum = volmeasure->uplinkPktNum + volmeasure->downlinkPktNum;
    }

    if (uplink) {
        volmeasure->uplinkVolume += vol;
    } else {
        volmeasure->downlinkVolume += vol;
    }

    volmeasure->totalVolume = volmeasure->uplinkVolume + volmeasure->downlinkVolume;

    if (!volume) {
        return false;
    }

    if (volume->totalVolume && (volmeasure->totalVolume >= volume->totalVolume) && (volume->flag & URR_VOLUME_TOVOL)) {
        return true;
    } else if (volume->uplinkVolume && (volmeasure->uplinkVolume >= volume->uplinkVolume) && (volume->flag & URR_VOLUME_ULVOL)) {
        return true;
    } else if (volume->downlinkVolume && (volmeasure->downlinkVolume >= volume->downlinkVolume) && (volume->flag & URR_VOLUME_DLVOL)) {
        return true;
    }

    return false;
}

int check_urr(struct pdr *pdr, u64 vol, u64 vol_mbqe, bool uplink){
    struct gtp5g_dev *gtp = netdev_priv(pdr->dev);
    int i;
    int ret = 1;
    u64 volume;
    u64 len;
    u32 *triggers;
    u32 report_num = 0;
    struct urr *urr, **urrs;
    struct user_report *report;
    bool mnop;

    urrs = kzalloc(sizeof(struct urr *) * pdr->urr_num , GFP_ATOMIC);
    triggers = kzalloc(sizeof(u32) * pdr->urr_num , GFP_ATOMIC);

    for (i = 0; i < pdr->urr_num; i++) {
        urr = find_urr_by_id(gtp, pdr->seid,  pdr->urr_ids[i]);

        mnop = false;
        if (urr->info & URR_INFO_MNOP)
            mnop = true;
        
        if (!(urr->info & URR_INFO_INAM)) {
            if (urr->method & URR_METHOD_VOLUM) {
                if (urr->trigger == 0) {
                    GTP5G_ERR(pdr->dev, "no supported trigger(%u) in URR(%u) and related to PDR(%u)",
                        urr->trigger, urr->id, pdr->id);
                    ret = 0;
                    goto err1;
                }

                if (urr->info & URR_INFO_MBQE) {
                    // TODO: gtp5g isn't support QoS enforcement yet
                    // Currently MBQE Volume = MAQE Volume
                    vol_mbqe = vol;
                    volume = vol_mbqe;
                } else {
                    volume = vol;
                }
                // Caculate Volume measurement for each trigger
                if (urr->trigger & URR_RPT_TRIGGER_VOLTH) {
                    if (increment_and_check_counter(&urr->bytes, &urr->volumethreshold, volume, uplink, mnop)) {
                        triggers[report_num] = USAR_TRIGGER_VOLTH;
                        urrs[report_num++] = urr;
                    }
                } else {
                    // For other triggers, only increment bytes
                    increment_and_check_counter(&urr->bytes, NULL, volume, uplink,mnop);
                }
                if (urr->trigger & URR_RPT_TRIGGER_VOLQU) {
                    if (increment_and_check_counter(&urr->consumed, &urr->volumequota, volume, uplink, mnop)) {
                        triggers[report_num] = USAR_TRIGGER_VOLQU;
                        urrs[report_num++] = urr;
                        urr_quota_exhaust_action(urr, gtp);
                        GTP5G_LOG(NULL, "URR (%u) Quota Exhaust, stop measure", urr->id);
                    }
                }
            }
        } else {
            GTP5G_TRC(pdr->dev, "URR stop measurement");
        }
    }
    if (report_num > 0) {
        len = sizeof(*report) * report_num;

        report = kzalloc(len, GFP_ATOMIC);
        for (i = 0; i < report_num; i++) {
            urr = urrs[i];

            urr->end_time = ktime_get_real();
            report[i] = (struct user_report) {
                    urr->id,
                    triggers[i],
                    urr->bytes,
                    0,
                    urr->start_time,
                    urr->end_time
            };

            memset(&urr->bytes, 0, sizeof(struct VolumeMeasurement));
            urr->start_time = ktime_get_real();
        }

        if (unix_sock_send(pdr, report, len, report_num) < 0) {
            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
            ret = -1;
            kfree(report);
            goto err1;
        }
    }

err1:
    kfree(urrs);
    kfree(triggers);
    return ret;
}

static int gtp5g_rx(struct pdr *pdr, struct sk_buff *skb,
    unsigned int hdrlen, unsigned int role)
{
    int rt = -1;
    u64 volume_mbqe = 0;
    struct far *far = pdr->far;
    // struct qer *qer = pdr->qer;

    if (!far) {
        GTP5G_ERR(pdr->dev, "FAR not exists for PDR(%u)\n", pdr->id);
        goto out;
    }

    //TODO: QER
    //if (qer) {
    //    printk_ratelimited("%s:%d QER Rule found, id(%#x) qfi(%#x)\n", __func__, __LINE__,
    //        qer->id, qer->qfi);
    //}

    // TODO: not reading the value of outer_header_removal now,
    // just check if it is assigned.
    if (pdr->outer_header_removal) {
        // One and only one of the DROP, FORW and BUFF flags shall be set to 1.
        // The NOCP flag may only be set if the BUFF flag is set.
        // The DUPL flag may be set with any of the DROP, FORW, BUFF and NOCP flags.
        switch(far->action & FAR_ACTION_MASK) {
        case FAR_ACTION_DROP:
            rt = gtp5g_drop_skb_encap(skb, pdr->dev, pdr);
            break;
        case FAR_ACTION_FORW:
            rt = gtp5g_fwd_skb_encap(skb, pdr->dev, hdrlen, pdr, volume_mbqe);
            break;
        case FAR_ACTION_BUFF:
            rt = gtp5g_buf_skb_encap(skb, pdr->dev, hdrlen, pdr);
            break;
        default:
            GTP5G_ERR(pdr->dev, "Unhandled apply action(%u) in FAR(%u) and related to PDR(%u)\n",
                far->action, far->id, pdr->id);
        }
        goto out;
    } 

    // TODO: this action is not supported
    GTP5G_ERR(pdr->dev, "Uplink: PDR(%u) didn't has a OHR information "
        "(which routed to the gtp interface and matches a PDR)\n", pdr->id);

out:
    return rt;
}

static int gtp5g_fwd_skb_encap(struct sk_buff *skb, struct net_device *dev,
    unsigned int hdrlen, struct pdr *pdr, uint64_t volume_mbqe)
{
    struct far *far = pdr->far;
    struct forwarding_parameter *fwd_param = far->fwd_param;
    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;
    struct gtpv1_hdr *gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    struct iphdr *iph;
    struct udphdr *uh;
    struct pcpu_sw_netstats *stats;
    int ret;
    u64 volume = 0;

    if (gtp1->type == GTPV1_MSG_TYPE_TPDU)
        volume = ip4_rm_header(skb, hdrlen);

    if (fwd_param) {
        if ((fwd_policy = fwd_param->fwd_policy))
            skb->mark = fwd_policy->mark;

        if ((hdr_creation = fwd_param->hdr_creation)) {
            // Just modify the teid and packet dest ip
            gtp1->tid = hdr_creation->teid;

            skb_push(skb, 20); // L3 Header Length
            iph = ip_hdr(skb);

            if (!pdr->pdi->f_teid) {
                GTP5G_ERR(dev, "Failed to hdr removal + creation "
                    "due to pdr->pdi->f_teid not exist\n");
                return -1;
            }

            iph->saddr = pdr->pdi->f_teid->gtpu_addr_ipv4.s_addr;
            iph->daddr = hdr_creation->peer_addr_ipv4.s_addr;
            iph->check = 0;

            uh = udp_hdr(skb);
            uh->check = 0;

            if (ip_xmit(skb, pdr->sk, dev) < 0) {
                GTP5G_ERR(dev, "Failed to transmit skb through ip_xmit\n");
                return -1;
            }

            if (pdr->urr_num != 0) {
                if (check_urr(pdr, volume, volume_mbqe, true) < 0)
                    GTP5G_ERR(pdr->dev, "Fail to send Usage Report");
            }
            return 0;
        }
    }

    if (gtp1->type != GTPV1_MSG_TYPE_TPDU) {
        GTP5G_WAR(dev, "Uplink: GTPv1 msg type is not TPDU\n");
        return -1;
    }

    // Get rid of the GTP-U + UDP headers.
    if (iptunnel_pull_header(skb,
            hdrlen, 
            skb->protocol,
            !net_eq(sock_net(pdr->sk), 
            dev_net(dev)))) {
        GTP5G_ERR(dev, "Failed to pull GTP-U and UDP headers\n");
        return -1;
    }

    /* Now that the UDP and the GTP header have been removed, set up the
     * new network header. This is required by the upper layer to
     * calculate the transport header.
     * */
    skb_reset_network_header(skb);

    skb->dev = dev;

    stats = this_cpu_ptr(skb->dev->tstats);
    u64_stats_update_begin(&stats->syncp);
    stats->rx_packets++;
    stats->rx_bytes += skb->len;
    u64_stats_update_end(&stats->syncp);

    pdr->ul_pkt_cnt++;
    pdr->ul_byte_cnt += skb->len; /* length without GTP header */
    GTP5G_INF(NULL, "PDR (%u) UL_PKT_CNT (%llu) UL_BYTE_CNT (%llu)", pdr->id, pdr->ul_pkt_cnt, pdr->ul_byte_cnt);

    ret = netif_rx(skb);
    if (ret != NET_RX_SUCCESS) {
        GTP5G_ERR(dev, "Uplink: Packet got dropped\n");
    }

    if (pdr->urr_num != 0) {
        if (check_urr(pdr, volume, volume_mbqe, true) < 0)
            GTP5G_ERR(pdr->dev, "Fail to send Usage Report");
    }

    return 0;
}

static int gtp5g_drop_skb_ipv4(struct sk_buff *skb, struct net_device *dev, 
    struct pdr *pdr)
{
    ++pdr->dl_drop_cnt;
    GTP5G_INF(NULL, "PDR (%u) DL_DROP_CNT (%llu)", pdr->id, pdr->dl_drop_cnt);
    dev_kfree_skb(skb);
    return FAR_ACTION_DROP;
}

static int gtp5g_fwd_skb_ipv4(struct sk_buff *skb, 
    struct net_device *dev, struct gtp5g_pktinfo *pktinfo, 
    struct pdr *pdr, uint64_t volume_mbqe)
{
    struct rtable *rt;
    struct flowi4 fl4;
    struct iphdr *iph = ip_hdr(skb);
    struct outer_header_creation *hdr_creation;
    struct qer *qer;

    int wred_result;

    u64 volume;

    if (!(pdr->far && pdr->far->fwd_param &&
        pdr->far->fwd_param->hdr_creation)) {
        GTP5G_ERR(dev, "Unknown RAN address\n");
        goto err;
    }

    hdr_creation = pdr->far->fwd_param->hdr_creation;
    rt = ip4_find_route(skb, 
        iph, 
        pdr->sk,
        dev, 
        pdr->role_addr_ipv4.s_addr, 
        hdr_creation->peer_addr_ipv4.s_addr, 
        &fl4);
    if (IS_ERR(rt))
        goto err;

    gtp5g_set_pktinfo_ipv4(pktinfo,
            pdr->sk, 
            iph, 
            hdr_creation,
            pdr->qfi, 
            rt, 
            &fl4, 
            dev);

    qer = pdr->qer;
    /*
    printk("qer id: %u\n", qer->id);
    printk("gbr: %u\n", qer->gbr.dl_high);
    printk("mbr: %u\n", qer->mbr.dl_high);
    */
    /*
    printk("pkt size: %u bytes\n", skb->len);
    printk("[Parameter] CIR: %llu bytes/sec, CBS: %llu bytes\n", qer->meter_param.cir, qer->meter_param.cbs);
    printk("[Parameter] PIR: %llu bytes/sec, PBS: %llu bytes\n", qer->meter_param.pir, qer->meter_param.pbs);
    printk("[Profile] CBS: %llu bytes\n", qer->meter_profile.cbs);
    printk("[Profile] PBS: %llu bytes\n", qer->meter_profile.pbs);
    printk("[Profile] CIR_PERIOD: %llu clks/each update, CIR_BYTES_PER_PERIOD: %llu bytes/each update\n", qer->meter_profile.cir_period, qer->meter_profile.cir_bytes_per_period);
    printk("[Profile] PIR_PERIOD: %llu clks/each update, PIR_BYTES_PER_PERIOD: %llu bytes/each update\n", qer->meter_profile.pir_period, qer->meter_profile.pir_bytes_per_period);
    printk("[Runtime] TC: %llu bytes available, TIME_TC: %llu clks\n", qer->meter_runtime.tc, qer->meter_runtime.time_tc);
    printk("[Runtime] TP: %llu bytes available, TIME_TP: %llu clks\n", qer->meter_runtime.tp, qer->meter_runtime.time_tp);
    */
    
    if(qer->qfi != 9){
        pktinfo->color = trtcm_color_blind_check(&(qer->meter_profile), &(qer->meter_runtime), get_tsc(), skb->len);
        if(pktinfo->color == 'G'){
            pdr->green_pkt_cnt++;
        }
        else if(pktinfo->color == 'Y'){
            pdr->yellow_pkt_cnt++;
        }
        else if(pktinfo->color == 'R'){
            pdr->red_pkt_cnt++; 
            return gtp5g_drop_skb_ipv4(skb, dev, pdr);
        }
        //else
            //printk("[ERROR] Meter color number out of range\n") 
    }
    else{
        pktinfo->color = 'W';
        pdr->white_pkt_cnt++;
    }
    
    wred_result = wred_processing(&(qer->queue_profile), &((qer->queue_profile).d_queue), skb, pktinfo);
    
    if(!wred_result)
        return gtp5g_drop_skb_ipv4(skb, dev, pdr);
    printk("dynamic queue length: %d\n", qer->queue_profile.d_queue.size);
    
    /*
    d_queue_push(&((qer->queue_profile).d_queue), skb, pktinfo);
    printk("1. dynamic queue length: %d\n", qer->queue_profile.d_queue.size);
    if(pdr->green_pkt_cnt != 0 && pdr->green_pkt_cnt % 5 == 0)
        d_queue_pop(&((qer->queue_profile).d_queue));
    printk("2. dynamic queue length: %d\n", qer->queue_profile.d_queue.size);
    */
    printk("pkt count:\nGREEN: %llu, YELLOW: %llu, RED: %llu\n", pdr->green_pkt_cnt, pdr->yellow_pkt_cnt, pdr->red_pkt_cnt);

    pdr->dl_pkt_cnt++;
    printk("pkt num: %llu\n", pdr->dl_pkt_cnt);
    pdr->dl_byte_cnt += skb->len;
    GTP5G_INF(NULL, "PDR (%u) DL_PKT_CNT (%llu) DL_BYTE_CNT (%llu)", pdr->id, pdr->dl_pkt_cnt, pdr->dl_byte_cnt);

    gtp5g_push_header(skb, pktinfo);

    volume = ip4_rm_header(skb, 0);

    if (pdr->urr_num != 0) {
        if (check_urr(pdr, volume, volume_mbqe, false) < 0)
            GTP5G_ERR(pdr->dev, "Fail to send Usage Report");
    }

    return FAR_ACTION_FORW;
err:
    return -EBADMSG;
}

static int gtp5g_buf_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct pdr *pdr)
{
    // TODO: handle nonlinear part
    if (unix_sock_send(pdr, skb->data, skb_headlen(skb), 0) < 0) {
        GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
        ++pdr->dl_drop_cnt;
    }

    dev_kfree_skb(skb);
    return FAR_ACTION_BUFF;
}

int gtp5g_handle_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct gtp5g_pktinfo *pktinfo)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);
    struct pdr *pdr;
    struct far *far;
    //struct gtp5g_qer *qer;
    struct iphdr *iph;
    u64 volume_mbqe = 0;

    /* Read the IP destination address and resolve the PDR.
     * Prepend PDR header with TEI/TID from PDR.
     */
    iph = ip_hdr(skb);
    if (gtp->role == GTP5G_ROLE_UPF)
        pdr = pdr_find_by_ipv4(gtp, skb, 0, iph->daddr);
    else
        pdr = pdr_find_by_ipv4(gtp, skb, 0, iph->saddr);

    if (!pdr) {
        GTP5G_ERR(dev, "no PDR found for %pI4, skip\n", &iph->daddr);
        return -ENOENT;
    }

    /* TODO: QoS rule have to apply before apply FAR 
     * */
    //qer = pdr->qer;
    //if (qer) {
    //    GTP5G_ERR(dev, "%s:%d QER Rule found, id(%#x) qfi(%#x) TODO\n", 
    //            __func__, __LINE__, qer->id, qer->qfi);
    //}
    
    far = pdr->far;
    
    if (far) {
        // One and only one of the DROP, FORW and BUFF flags shall be set to 1.
        // The NOCP flag may only be set if the BUFF flag is set.
        // The DUPL flag may be set with any of the DROP, FORW, BUFF and NOCP flags.
        switch (far->action & FAR_ACTION_MASK) {
        case FAR_ACTION_DROP:
            return gtp5g_drop_skb_ipv4(skb, dev, pdr);
        case FAR_ACTION_FORW:
            return gtp5g_fwd_skb_ipv4(skb, dev, pktinfo, pdr, volume_mbqe);
        case FAR_ACTION_BUFF:
            return gtp5g_buf_skb_ipv4(skb, dev, pdr);
        default:
            GTP5G_ERR(dev, "Unspec apply action(%u) in FAR(%u) and related to PDR(%u)",
                far->action, far->id, pdr->id);
        }
    }

    return -ENOENT;
}
