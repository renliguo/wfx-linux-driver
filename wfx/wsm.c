/*
 * Copyright (c) 2017, Silicon Laboratories
 *
 * based on:
 * Copyright (c) 2010, ST-Ericsson
 * Author: Dmitry Tarnyagin <dmitry.tarnyagin@lockless.no>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*========================================================================*/
/*                 Standard Linux Headers             					  */
/*========================================================================*/
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/random.h>

/*========================================================================*/
/*                 Local Header files             					      */
/*========================================================================*/
#include "wfx.h"
#include "wsm.h"
#include "bh.h"
#include "debug.h"
#include "sta.h"

#if defined(CONFIG_WF200_WSM_DEBUG)
#define wsm_printk(...)
#else
#define wsm_printk(...) printk(__VA_ARGS__)
#endif

/*========================================================================*/
/*                  PRIVATE  Constants/Macros/Types/Variables             */
/*========================================================================*/
#define WSM_GET(buf, ptr, size)						\
	do {								\
		if ((buf)->data + size > (buf)->end)			\
			goto underflow;					\
		memcpy(ptr, (buf)->data, size);				\
		(buf)->data += size;					\
	} while (0)

#define __WSM_GET(buf, type, type2, cvt)				\
	({								\
		type val;						\
		if ((buf)->data + sizeof(type) > (buf)->end)		\
			goto underflow;					\
		val = cvt(*(type2 *)(buf)->data);			\
		(buf)->data += sizeof(type);				\
		val;							\
	})

#define WSM_GET8(buf)  __WSM_GET(buf, u8, u8, (u8))
#define WSM_GET16(buf) __WSM_GET(buf, u16, __le16, __le16_to_cpu)
#define WSM_GET32(buf) __WSM_GET(buf, u32, __le32, __le32_to_cpu)

/*labels for REQUEST and CONFIRMATION msg*/
char *wsm_req_msg_label[NB_REQ_MSG]={
		/*00*/ "RESERVED_00     ",
		/*01*/ "RESERVED_01     ",
		/*02*/ "WSM_CONFIG      ",
		/*03*/ "WSM_GENERIC     ",
		/*04*/ "WSM_TRANSMIT    ",
		/*05*/ "WSM_READ_MIB    ",
		/*06*/ "WSM_WRITE_MIB   ",
		/*07*/ "WSM_START_SCAN  ",
		/*08*/ "WSM_STOP_SCAN   ",
		/*09*/ "WSM_CONFIG      ",
		/*0A*/ "WSM_RESET_LINK  ",
		/*0B*/ "WSM_JOIN_BSS    ",
		/*0C*/ "WSM_ADD_KEY     ",
		/*0D*/ "WSM_RM_KEY      ",
		/*0E*/ "RESERVED_0E     ",
		/*0F*/ "RESERVED_0F     ",
		/*10*/ "WSM_SET_PWR_MGT ",
		/*11*/ "WSM_SET_BSS_PARAM",
		/*12*/ "WSM_SET_TX_QOS  ",
		/*13*/ "WSM_SET_EDCA_PARM",
		/*14*/ "WSM_NOT_USED_14 ",
		/*15*/ "WSM_SET_SYS_INFO",
		/*16*/ "WSM_SWITCH_CHANNEL",
		/*17*/ "WSM_START_AP    ",
		/*18*/ "WSM_TX_BEACON   ",
		/*19*/ "REMOVED",
		/*1A*/ "REMOVED",
		/*1B*/ "WSM_UPDATE_IE   ",
		/*1C*/ "WSM_MAP_LINK/MAC",
		/*1D*/ "WSM_PTA_STAT    ",
		/*1E*/ "WSM_MULTI_TX    ",
		/*1F*/ "WSM_NOT_USED_1F ",
		/*20*/ "RESERVED_20     "
};

/*labels for INDICATION msg*/
char *wsm_ind_msg_label[NB_INDIC_MSG]={
		/*00*/ "WSM_EXCEPTION   ",
		/*01*/ "WSM_START_UP    ",
		/*02*/ "WSM_TRACE       ",
		/*03*/ "WSM_GENERIC     ",
		/*04*/ "WSM_RECEIVE     ",
		/*05*/ "WSM_EVENT       ",
		/*06*/ "WSM_SCAN_COMP   ",
		/*07*/ "WSM_MEASURE_COMP",
		/*08*/ "WSM_BLK_ACK_TIMEOUT",
		/*09*/ "WSM_SET_PM_COMP ",
		/*0A*/ "WSM_SWITCH_CH_COMP",
		/*0B*/ "REMOVED",
		/*0C*/ "WSM_TX_CTRL     ",
		/*0D*/ "WSM_RX_BEACON   ",
		/*0E*/ "WSM_DBG_INFO    ",
		/*0F*/ "WSM_JOINBSS_COMP"
};
/**/
/*========================================================================*/
/*                  Structures definitions                                */
/*========================================================================*/
struct wsm_mib {
	u16 mib_id;
	void *buf;
	size_t buf_size;
};

/*========================================================================*/
/*                       Functions                                        */
/*========================================================================*/
static int wsm_generic_confirm(struct wfx_common *priv,
			     void *arg,
			     struct wsm_buf *buf)
{
	/* Use configuration message confirmation as default structure*/
	if (WARN_ON(((WsmHiConfigurationCnf_t *)buf->begin)->Body.Status != WSM_STATUS_SUCCESS))
		return -EINVAL;
	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_configuration_confirm(struct wfx_common *priv,
				     struct wsm_configuration *arg,
				     struct wsm_buf *buf)
{

	if (WARN_ON(((WsmHiConfigurationCnf_t *)buf->begin)->Body.Status != WSM_STATUS_SUCCESS))
		return -EINVAL;

	memcpy(&arg->cnf_part, &((WsmHiConfigurationCnf_t *)buf->begin)->Body, sizeof(arg->cnf_part));

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_read_mib_confirm(struct wfx_common *priv,
				struct wsm_mib *arg,
				struct wsm_buf *buf)
{
	u16 size;
	WsmHiReadMibCnfBody_t *Body = &((WsmHiReadMibCnf_t *)buf->begin)->Body;
	if (WARN_ON(Body->Status != WSM_STATUS_SUCCESS))
		return -EINVAL;

	if (WARN_ON(Body->MibId != arg->mib_id))
		return -EINVAL;

	size = Body->Length;
	if (size > arg->buf_size)
		size = arg->buf_size;

	memcpy(arg->buf, &Body->MibData, size);
	arg->buf_size = size;
	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_write_mib_confirm(struct wfx_common *priv,
				struct wsm_mib *arg,
				struct wsm_buf *buf)
{
	int ret;

	ret = wsm_generic_confirm(priv, arg, buf);
	if (ret)
		return ret;

	if (arg->mib_id == WSM_MIB_ID_OPERATIONAL_POWER_MODE) {
		/* OperationalMode: update PM status. */
		const char *p = arg->buf;
		wfx_enable_powersave(priv, (p[0] & 0x0F) ? true : false);
	}
	return 0;
}

/*-----------------------------------------------------------------------*/
static int wsm_tx_confirm(struct wfx_common *priv,
			  struct wsm_buf *buf,
			  int link_id)
{


	wfx_tx_confirm_cb(priv, link_id, &((WsmHiTxCnf_t *)buf->begin)->Body);
	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_multi_tx_confirm(struct wfx_common *priv,
				struct wsm_buf *buf, int link_id)
{
	int ret;
	int count;
	int i;

	count = ((WsmHiMultiTransmitCnf_t *)buf->begin)->Body.NumTxConfs;
	if (WARN_ON(count <= 0))
		return -EINVAL;

	if (count > 1) {
		ret = wsm_release_tx_buffer(priv, count - 1);
		if (ret < 0)
			return ret;
		else if (ret > 0)
			wfx_bh_wakeup(priv);
	}

	wfx_debug_txed_multi(priv, count);
	for (i = 0; i < count; ++i) {
		ret = wsm_tx_confirm(priv, buf, link_id);
		if (ret)
			return ret;
	}
	return ret;

}

/*-----------------------------------------------------------------------*/
static int wsm_join_confirm(struct wfx_common *priv,
		WsmHiJoinCnfBody_t *arg,
			    struct wsm_buf *buf)
{
	if (WARN_ON(((WsmHiJoinCnf_t *)buf->begin)->Body.Status != WSM_STATUS_SUCCESS))
		return -EINVAL;

	memcpy( arg , &((WsmHiJoinCnf_t *)buf->begin)->Body, sizeof(WsmHiJoinCnfBody_t));

	return 0;

}

/*-----------------------------------------------------------------------*/
int wfx_unmap_link(struct wfx_common *priv, int sta_id)
{

	WsmHiMapLinkReqBody_t maplink = {
			.PeerStaId = sta_id,
			.Flags = true,
	};
	if (sta_id)
	{
		memcpy(&maplink.MacAddr[0],priv->link_id_db[sta_id - 1].old_mac, ETH_ALEN);
	}

	return wsm_map_link(priv, &maplink);
}

/*-----------------------------------------------------------------------*/
int wsm_set_probe_responder(struct wfx_common *priv, bool enable)
{
	priv->rx_filter.probeResponder = enable;
	return wsm_set_rx_filter(priv, &priv->rx_filter);
}

/*-----------------------------------------------------------------------*/
/* WSM indication events implementation									 */
const char * const wfx_fw_types[] = {
	"ETF",
	"WFM",
	"WSM",
	"HI test",
	"Platform test"
};

/*-----------------------------------------------------------------------*/
static int wsm_startup_indication(struct wfx_common *priv,
					struct wsm_buf *buf)
{

	memcpy( &priv->wsm_caps , &((HiStartupInd_t *)buf->begin)->Body, sizeof(HiStartupIndBody_t));

	/*Make sure the text label ends with a zero */
	priv->wsm_caps.FirmwareLabel[sizeof(priv->wsm_caps.FirmwareLabel) - 1] = 0; /* Do not trust FW too much... */

	if (WARN_ON(priv->wsm_caps.InitStatus)) {
		return -EINVAL;
	}

	if (WARN_ON(priv->wsm_caps.FirmwareType > 4)) {
		return -EINVAL;
	}

	pr_info("wfx WSM init done.\n"
		"   Input buffers: %d x %d bytes\n"
		"   Hardware: %d.%d\n"
		"   %s firmware [%s], ver: %d, build: %d %d,"
		"   api: %d, cap: 0x%.4X\n",
		priv->wsm_caps.NumInpChBufs,
		priv->wsm_caps.SizeInpChBuf,
		priv->wsm_caps.HardwareId, priv->wsm_caps.HardwareSubId,
		wfx_fw_types[priv->wsm_caps.FirmwareType],
		priv->wsm_caps.FirmwareLabel, priv->wsm_caps.FirmwareVersion.FonctionalityNumber,
		priv->wsm_caps.FirmwareVersion.MajorNumber,
		priv->wsm_caps.FirmwareBuildNumber,
		priv->wsm_caps.FirmwareApiVer.HIApiVersion, priv->wsm_caps.FirmwareCap);

	/* Disable unsupported frequency bands */
	if (!(priv->wsm_caps.FirmwareCap & 0x1)) {
		priv->hw->wiphy->bands[IEEE80211_BAND_2GHZ] = NULL;
	}
	if (!(priv->wsm_caps.FirmwareCap & 0x2)) {
		priv->hw->wiphy->bands[IEEE80211_BAND_5GHZ] = NULL;
	}

	priv->firmware_ready = 1;
	wake_up(&priv->wsm_startup_done);
	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_receive_indication(struct wfx_common *priv,
				  int if_id,
				  struct wsm_buf *buf,
				  struct sk_buff **skb_p)
{
	WsmHiRxIndBody_t rx;

	struct ieee80211_hdr *hdr;
	size_t hdr_len;
	__le16 fctl;
	int sta_id;

	rx.Status = WSM_GET32(buf);
	rx.ChannelNumber = WSM_GET16(buf);
	rx.RxedRate = WSM_GET8(buf);
	rx.RcpiRssi = WSM_GET8(buf);
	WSM_GET(buf, &rx.RxFlags, sizeof(rx.RxFlags));
	hdr = (struct ieee80211_hdr *)(*skb_p)->data;

	if (!rx.RcpiRssi &&
	    (ieee80211_is_probe_resp(hdr->frame_control) ||
	     ieee80211_is_beacon(hdr->frame_control))) 
		return 0;
	if (!priv->cqm_use_rssi)
		rx.RcpiRssi = rx.RcpiRssi / 2 - 110;

	fctl = *(__le16 *)buf->data;
	pr_debug("[WSM] \t\t rx_flags=0x%.8X, frame_ctrl=0x%.4X\n", *((uint32 *)&rx.RxFlags), __le16_to_cpu(fctl) );

	hdr_len = buf->data - buf->begin;
	skb_pull(*skb_p, hdr_len);
	sta_id = rx.RxFlags.PeerStaId;

	wfx_rx_cb(priv, &rx, sta_id, skb_p);
	if (*skb_p)
		skb_push(*skb_p, hdr_len);

	return 0;

underflow:
	return -EINVAL;
}

/*-----------------------------------------------------------------------*/
static int wsm_event_indication(struct wfx_common *priv, struct wsm_buf *buf)
{
	int first;
	struct wfx_wsm_event *event;

	if (priv->mode == NL80211_IFTYPE_UNSPECIFIED) {
		/* STA is stopped. */
		return 0;
	}

	event = kzalloc(sizeof(struct wfx_wsm_event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	memcpy( &event->evt , &((WsmHiEventInd_t *)buf->begin)->Body, sizeof(WsmHiEventIndBody_t));

	pr_debug("[WSM] Event: %d(%d)\n",
		 event->evt.EventId, *((uint32*)&event->evt.EventData));

	spin_lock(&priv->event_queue_lock);
	first = list_empty(&priv->event_queue);
	list_add_tail(&event->link, &priv->event_queue);
	spin_unlock(&priv->event_queue_lock);

	if (first)
		queue_work(priv->workqueue, &priv->event_handler);

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_channel_switch_indication(struct wfx_common *priv,
					 struct wsm_buf *buf)
{
	WARN_ON(((WsmHiSwitchChannelCnf_t *)buf->begin)->Body.Status);

	priv->channel_switch_in_progress = 0;
	wake_up(&priv->channel_switch_done);

	wsm_unlock_tx(priv);

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_set_pm_indication(struct wfx_common *priv,
				 struct wsm_buf *buf)
{
	if (priv->ps_mode_switch_in_progress) {
		priv->ps_mode_switch_in_progress = 0;
		wake_up(&priv->ps_mode_switch_done);
	}
	return 0;
}

/*-----------------------------------------------------------------------*/
static int wsm_scan_started(struct wfx_common *priv, void *arg,
			    struct wsm_buf *buf)
{
	if (WARN_ON(((WsmHiStartScanCnf_t *)buf->begin)->Body.Status != WSM_STATUS_SUCCESS)){
		wfx_scan_failed_cb(priv);
		return -EINVAL;
	}
	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_scan_complete_indication(struct wfx_common *priv,
					struct wsm_buf *buf)
{
	WsmHiScanCmplIndBody_t arg;
	memcpy( &arg , &((WsmHiScanCmplInd_t *)buf->begin)->Body, sizeof(WsmHiScanCmplIndBody_t));
	wfx_scan_complete_cb(priv, &arg);

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_join_complete_indication(struct wfx_common *priv,
					struct wsm_buf *buf)
{
	WsmHiJoinCompleteIndBody_t arg;
	memcpy( &arg , &((WsmHiJoinCompleteInd_t *)buf->begin)->Body, sizeof(WsmHiJoinCompleteIndBody_t));
	pr_debug("[WSM] Join complete indication, status: %d\n", arg.Status);
	wfx_join_complete_cb(priv, &arg);

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_dbg_info_indication(struct wfx_common *priv,
					struct wsm_buf *buf, __le16 msgLen)
{

        WsmHiDebugIndBody_t *Body = &((WsmHiDebugInd_t *)buf->begin)->Body;
      switch (Body->DbgId)
      {
		case 6:
			  pr_warn("dbg msg CPU profiling : cpu_load=%d\n", Body->DbgData.EptaRtStats.MsgStartIdentifier);
	  		break;
#ifdef CONFIG_WF200_TESTMODE 
		case 7:
			  bs_buffer_add((uint8_t *)&Body->DbgData, msgLen - 8);
	  		break;
#endif /* CONFIG_WF200_TESTMODE */
	  default:
	  		break;
	}

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_ba_timeout_indication(struct wfx_common *priv,
				     struct wsm_buf *buf)
{


	pr_info("BlockACK timeout, tid %d, addr %pM\n",
		((WsmHiBaTimeoutInd_t *)buf->begin)->Body.TID,
		((WsmHiBaTimeoutInd_t *)buf->begin)->Body.TransmitAddress);

	return 0;

}

/*-----------------------------------------------------------------------*/
static int wsm_suspend_resume_indication(struct wfx_common *priv,
					 int link_id, struct wsm_buf *buf)
{
	WsmHiSuspendResumeTxIndBody_t arg;
	memcpy( &arg , &((WsmHiSuspendResumeTxInd_t *)buf->begin)->Body,sizeof(WsmHiSuspendResumeTxIndBody_t));

	wfx_suspend_resume(priv, link_id, &arg);

	return 0;

}


/*--------------------WSM TX port control------------------------------*/
void wsm_lock_tx(struct wfx_common *priv)
{
	wsm_cmd_lock(priv);
	if (atomic_add_return(1, &priv->tx_lock) == 1) {
		if (wsm_flush_tx(priv))
			pr_debug("[WSM] TX is locked.\n");
	}
	wsm_cmd_unlock(priv);
}

/*-----------------------------------------------------------------------*/
void wsm_lock_tx_async(struct wfx_common *priv)
{
	if (atomic_add_return(1, &priv->tx_lock) == 1)
		pr_debug("[WSM] TX is locked (async).\n");
}

/*-----------------------------------------------------------------------*/
bool wsm_flush_tx(struct wfx_common *priv)
{
	unsigned long timestamp = jiffies;
	bool pending = false;
	long timeout;
	int i;
	BUG_ON(!atomic_read(&priv->tx_lock));
	if (!priv->hw_bufs_used)
		return true;

	if (priv->bh_error) {
		pr_err("[WSM] Fatal error occurred, will not flush TX.\n");
		return false;
	} else {
		for (i = 0; i < 4; ++i)
			pending |= wfx_queue_get_xmit_timestamp(
					&priv->tx_queue[i],
					&timestamp, 0xffffffff);
		if (!pending)
			return true;

		timeout = timestamp + WSM_CMD_LAST_CHANCE_TIMEOUT - jiffies;
		if (timeout < 0 || wait_event_timeout(priv->bh_evt_wq,
						      !priv->hw_bufs_used,
						      timeout) <= 0) {
			priv->bh_error = 1;
			wiphy_err(priv->hw->wiphy, "[WSM] TX Frames (%d) stuck in firmware, killing BH\n", priv->hw_bufs_used);
			wake_up(&priv->bh_wq);
			return false;
		}
		return true;
	}
}

/*-----------------------------------------------------------------------*/
void wsm_unlock_tx(struct wfx_common *priv)
{
	int tx_lock;
	tx_lock = atomic_sub_return(1, &priv->tx_lock);
	BUG_ON(tx_lock < 0);

	if (tx_lock == 0) {
		if (!priv->bh_error)
			wfx_bh_wakeup(priv);
		pr_debug("[WSM] TX is unlocked.\n");
	}
}

/*-----------------------------WSM RX------------------------------------*/
int wsm_handle_exception(struct wfx_common *priv, u8 *data, size_t len)
{
	struct wsm_buf buf;
	u32 reason;
	u32 reg[18];
	char fname[48];
	unsigned int i;

	static const char * const reason_str[] = {
		"undefined instruction",
		"prefetch abort",
		"data abort",
		"unknown error",
	};

	buf.begin = buf.data = data;
	buf.end = &buf.begin[len];

	reason = WSM_GET32(&buf);
	for (i = 0; i < ARRAY_SIZE(reg); ++i)
		reg[i] = WSM_GET32(&buf);
	WSM_GET(&buf, fname, sizeof(fname));

	if (reason < 4)
		wiphy_err(priv->hw->wiphy,
			  "Firmware exception: %s.\n",
			  reason_str[reason]);
	else
		wiphy_err(priv->hw->wiphy,
			  "Firmware assert at %.*s, id %d\n",
			  (int) sizeof(fname), fname, reg[1]);

	for (i = 0; i < 12; i += 4)
		wiphy_err(priv->hw->wiphy,
			  "R%d: 0x%.8X, R%d: 0x%.8X, R%d: 0x%.8X, R%d: 0x%.8X,\n",
			  i + 0, reg[i + 0], i + 1, reg[i + 1],
			  i + 2, reg[i + 2], i + 3, reg[i + 3]);
	wiphy_err(priv->hw->wiphy,
		  "R12: 0x%.8X, SP: 0x%.8X, LR: 0x%.8X, PC: 0x%.8X,\n",
		  reg[i + 0], reg[i + 1], reg[i + 2], reg[i + 3]);
	i += 4;
	wiphy_err(priv->hw->wiphy,
		  "CPSR: 0x%.8X, SPSR: 0x%.8X\n",
		  reg[i + 0], reg[i + 1]);

	print_hex_dump_bytes("R1: ", DUMP_PREFIX_NONE,
			     fname, sizeof(fname));
	return 0;

underflow:
	wiphy_err(priv->hw->wiphy, "Firmware exception.\n");
	print_hex_dump_bytes("Exception: ", DUMP_PREFIX_NONE,
			     data, len);
	return -EINVAL;
}

/*-----------------------------------------------------------------------*/
int wsm_handle_rx(struct wfx_common *priv, HiMsgHdr_t *wsm, struct sk_buff **skb_p)
{
	int ret = 0;
	u8 wsm_id  = wsm->s.t.MsgId;
	u8 msg_id = wsm->s.b.Id;
	int msg_type = wsm->s.b.MesgType;
	int link_id =  wsm->s.b.IntId;
	struct wsm_buf wsm_buf;

	wsm_buf.begin = (u8 *)&wsm[0];
	wsm_buf.data = (u8 *)&wsm[1];
	wsm_buf.end = &wsm_buf.begin[__le16_to_cpu(wsm->MsgLen)];


	if (msg_id<NB_INDIC_MSG && msg_type==1) { //indication
		pr_debug("[WSM] link %d <<< indication %s : status=0x%.8X\n",
				link_id,
				wsm_ind_msg_label[msg_id],
				__le32_to_cpu(((__le32 *)wsm_buf.begin)[1]));
	} else if (msg_id<NB_REQ_MSG) { //confirmation
		if (msg_id==4)
			pr_debug("[WSM] link %d <<< confirmation %s : packet_id=0x%.8X status=0x%.8X\n",
					link_id, wsm_req_msg_label[msg_id],
					__le32_to_cpu(((__le32 *)wsm_buf.begin)[1]),
					__le32_to_cpu(((__le32 *)wsm_buf.begin)[2]) );
		else
			pr_debug("[WSM] link %d <<< confirmation %s : status=0x%.8X\n",
					link_id,
					wsm_req_msg_label[msg_id],
					__le32_to_cpu(((__le32 *)wsm_buf.begin)[1]) );
	} else {
		pr_debug("[WSM] <<< ERROR : wrong RX msg id 0x%.4X\n", wsm_id);
	}

	if (wsm_id == WSM_HI_TX_CNF_ID) {
		ret = wsm_tx_confirm(priv, &wsm_buf, link_id);
	} else if (wsm_id == WSM_HI_MULTI_TRANSMIT_CNF_ID) {
		ret = wsm_multi_tx_confirm(priv, &wsm_buf, link_id);
	} else if (!msg_type) { //confirmation msg
		void *wsm_arg;
		u8 wsm_cmd;
#ifdef CONFIG_WF200_TESTMODE
		/* Add ID to testmode's buffer */
		wfx_tm_hif_buffer_add(wsm_id);
#endif /* CONFIG_WF200_TESTMODE */

		spin_lock(&priv->wsm_cmd.lock);
		wsm_arg = priv->wsm_cmd.arg;
		wsm_cmd = priv->wsm_cmd.cmd;
		priv->wsm_cmd.cmd = 0xFF;
		spin_unlock(&priv->wsm_cmd.lock);

		if (WARN_ON(wsm_id != wsm_cmd)) {
			ret = -EINVAL;
			goto out;
		}

		switch (wsm_id) {
		case WSM_HI_READ_MIB_CNF_ID:
			if (wsm_arg)
				ret = wsm_read_mib_confirm(priv, wsm_arg,
								&wsm_buf);
			break;
		case WSM_HI_WRITE_MIB_CNF_ID:
			if (wsm_arg)
				ret = wsm_write_mib_confirm(priv, wsm_arg,
							    &wsm_buf);
			break;
		case WSM_HI_START_SCAN_CNF_ID:
			if (wsm_arg)
				ret = wsm_scan_started(priv, wsm_arg, &wsm_buf);
			break;
		case WSM_HI_CONFIGURATION_CNF_ID:
			if (wsm_arg)
				ret = wsm_configuration_confirm(priv, wsm_arg,
								&wsm_buf);
			break;
		case WSM_HI_JOIN_CNF_ID:
			if (wsm_arg)
				ret = wsm_join_confirm(priv, wsm_arg, &wsm_buf);
			break;
		case WSM_HI_SET_PM_MODE_CNF_ID:
			if (-ETIMEDOUT == priv->scan.status)
				priv->scan.status = 1;
		case WSM_HI_STOP_SCAN_CNF_ID:
		case WSM_HI_RESET_CNF_ID:
		case WSM_HI_ADD_KEY_CNF_ID:
		case WSM_HI_REMOVE_KEY_CNF_ID:
		case WSM_HI_SET_BSS_PARAMS_CNF_ID:
		case WSM_HI_TX_QUEUE_PARAMS_CNF_ID: /* set_tx_queue_params */
		case WSM_HI_EDCA_PARAMS_CNF_ID:
		case WSM_HI_SWITCH_CHANNEL_CNF_ID:
		case WSM_HI_START_CNF_ID:
		case WSM_HI_BEACON_TRANSMIT_CNF_ID:
		case WSM_HI_UPDATE_IE_CNF_ID: /* update_ie */
		case WSM_HI_MAP_LINK_CNF_ID: /* map_link */
			WARN_ON(wsm_arg != NULL);
			ret = wsm_generic_confirm(priv, wsm_arg, &wsm_buf);
			if (ret) {
				wiphy_warn(priv->hw->wiphy,
					   "wsm_generic_confirm failed for request 0x%02x.\n", wsm_id);
				if (priv->join_status >= WFX_JOIN_STATUS_JOINING) {
					wsm_lock_tx(priv);
					if (queue_work(priv->workqueue, &priv->unjoin_work) <= 0)
						wsm_unlock_tx(priv);
				}
			}
			break;
		default:
			wiphy_warn(priv->hw->wiphy,
				   "Unrecognized confirmation 0x%02x\n", wsm_id);
		}

		spin_lock(&priv->wsm_cmd.lock);
		priv->wsm_cmd.ret = ret;
		priv->wsm_cmd.done = 1;
		spin_unlock(&priv->wsm_cmd.lock);

		ret = 0;

		wake_up(&priv->wsm_cmd_wq);
	} else {

#ifdef CONFIG_WF200_TESTMODE
		/* Add ID to testmode's buffer */
		wfx_tm_hif_buffer_add(wsm_id);
#endif /* CONFIG_WF200_TESTMODE */

		switch (wsm_id) {
		case HI_STARTUP_IND_ID:
			ret = wsm_startup_indication(priv, &wsm_buf);
			break;
		case WSM_HI_RX_IND_ID:
			ret = wsm_receive_indication(priv, link_id,
						     &wsm_buf, skb_p);
			break;
		case WSM_HI_EVENT_IND_ID:
			ret = wsm_event_indication(priv, &wsm_buf);
			break;
		case WSM_HI_SCAN_CMPL_IND_ID:
			ret = wsm_scan_complete_indication(priv, &wsm_buf);
			break;
		case WSM_HI_BA_TIMEOUT_IND_ID:
			ret = wsm_ba_timeout_indication(priv, &wsm_buf);
			break;
		case WSM_HI_SET_PM_MODE_CMPL_IND_ID:
			ret = wsm_set_pm_indication(priv, &wsm_buf);
			break;
		case WSM_HI_SWITCH_CHANNEL_IND_ID:
			ret = wsm_channel_switch_indication(priv, &wsm_buf);
			break;
		case WSM_HI_SUSPEND_RESUME_TX_IND_ID:
			ret = wsm_suspend_resume_indication(priv,
					link_id, &wsm_buf);
			break;
		case WSM_HI_DEBUG_IND_ID:
			ret = wsm_dbg_info_indication(priv, &wsm_buf,wsm->MsgLen);
			break;
		case WSM_HI_JOIN_COMPLETE_IND_ID:
			ret = wsm_join_complete_indication(priv, &wsm_buf);
			break;
		default:
			pr_warn("Unrecognised WSM ID %02x\n", wsm_id);
		}
	}

out:
	return ret;
}

/*-----------------------------------------------------------------------*/
static bool wsm_handle_tx_data(struct wfx_common *priv,
		WsmHiTxReq_t *wsm,
			       const struct ieee80211_tx_info *tx_info,
			       const struct wfx_txpriv *txpriv,
			       struct wfx_queue *queue)
{
	bool handled = false;
	const struct ieee80211_hdr *frame =
		(struct ieee80211_hdr *)&((u8 *)wsm)[txpriv->offset];
	__le16 fctl = frame->frame_control;
	enum {
		do_probe,
		do_drop,
		do_wep,
		do_tx,
	} action = do_tx;

	switch (priv->mode) {
	case NL80211_IFTYPE_STATION:
		if (priv->join_status == WFX_JOIN_STATUS_MONITOR)
			action = do_tx;
		else if (priv->join_status < WFX_JOIN_STATUS_PRE_STA)
			action = do_drop;
		break;
	case NL80211_IFTYPE_AP:
		if (!priv->join_status) {
			action = do_drop;
		} else if (!(BIT(txpriv->raw_link_id) &
			     (BIT(0) | priv->link_id_map))) {
			wiphy_warn(priv->hw->wiphy,
				   "A frame with expired link id is dropped.\n");
			action = do_drop;
		}
		if (wfx_queue_get_generation(wsm->Body.PacketId) >
				WFX_MAX_REQUEUE_ATTEMPTS) {
			wiphy_warn(priv->hw->wiphy,
				   "Too many attempts to requeue a frame; dropped.\n");
			action = do_drop;
		}
		break;
	case NL80211_IFTYPE_ADHOC:
		if (priv->join_status != WFX_JOIN_STATUS_IBSS)
			action = do_drop;
		break;
	case NL80211_IFTYPE_MESH_POINT:
		action = do_tx;
		break;
	case NL80211_IFTYPE_MONITOR:
	default:
		action = do_drop;
		break;
	}

	if (action == do_tx) {
		if (ieee80211_is_nullfunc(fctl)) {
			spin_lock(&priv->bss_loss_lock);
			if (priv->bss_loss_state) {
				priv->bss_loss_confirm_id = wsm->Body.PacketId;
				wsm->Body.QueueId.QueueId = WSM_QUEUE_ID_VOICE;
			}
			spin_unlock(&priv->bss_loss_lock);
		} else if (ieee80211_is_probe_req(fctl)) {
			action = do_probe;
		} else if (ieee80211_has_protected(fctl) &&
			   tx_info->control.hw_key &&
			   tx_info->control.hw_key->keyidx != priv->wep_default_key_id &&
			   (tx_info->control.hw_key->cipher == WLAN_CIPHER_SUITE_WEP40 ||
			    tx_info->control.hw_key->cipher == WLAN_CIPHER_SUITE_WEP104)) {
			action = do_wep;
		}
	}

	switch (action) {
	case do_probe:
		pr_debug("[WSM] Convert probe request to scan.\n");
		wsm_lock_tx_async(priv);
		priv->pending_frame_id = wsm->Body.PacketId;
		if (queue_delayed_work(priv->workqueue,
				       &priv->scan.probe_work, 0) <= 0)
			wsm_unlock_tx(priv);
		handled = true;
		break;
	case do_drop:
		pr_debug("[WSM] Drop frame (0x%.4X).\n", fctl);
		BUG_ON(wfx_queue_remove(queue, wsm->Body.PacketId));
		handled = true;
		break;
	case do_wep:
		pr_debug("[WSM] Issue set_default_wep_key.\n");
		wsm_lock_tx_async(priv);
		priv->wep_default_key_id = tx_info->control.hw_key->keyidx;
		priv->pending_frame_id = wsm->Body.PacketId;
		if (queue_work(priv->workqueue, &priv->wep_key_work) <= 0)
			wsm_unlock_tx(priv);
		handled = true;
		break;
	case do_tx:
		pr_debug("[WSM] Transmit frame.\n");
		break;
	default:
		/* Do nothing */
		break;
	}
	return handled;
}

/*-----------------------------------------------------------------------*/
static int wfx_get_prio_queue(struct wfx_common *priv,
				 u32 link_id_map, int *total)
{
	static const int urgent = BIT(WFX_LINK_ID_AFTER_DTIM) |
		BIT(WFX_LINK_ID_UAPSD);
	WsmHiEdcaParamsReqBody_t *edca;
	unsigned score, best = -1;
	int winner = -1;
	int queued;
	int i;

	edca = &priv->edca.params;
	for (i = 0; i < 4; ++i) {
		queued = wfx_queue_get_num_queued(&priv->tx_queue[i],
				link_id_map);
		if (!queued)
			continue;
		*total += queued;
		score = ((edca->AIFSN[i] + edca->CwMin[i]) << 16) +
			((edca->CwMax[i] - edca->CwMin[i]) *
			 (get_random_int() & 0xFFFF));
		if (score < best && (winner < 0 || i != 3)) {
			best = score;
			winner = i;
		}
	}

	/* override winner if bursting */
	if (winner >= 0 && priv->tx_burst_idx >= 0 &&
	    winner != priv->tx_burst_idx &&
	    !wfx_queue_get_num_queued(
		    &priv->tx_queue[winner],
		    link_id_map & urgent) &&
	    wfx_queue_get_num_queued(
		    &priv->tx_queue[priv->tx_burst_idx],
		    link_id_map))
		winner = priv->tx_burst_idx;

	return winner;
}

/*-----------------------------------------------------------------------*/
static int wsm_get_tx_queue_and_mask(struct wfx_common *priv,
				     struct wfx_queue **queue_p,
				     u32 *tx_allowed_mask_p,
				     bool *more)
{
	int idx;
	u32 tx_allowed_mask;
	int total = 0;

	/* Search for a queue with multicast frames buffered */
	if (priv->tx_multicast) {
		tx_allowed_mask = BIT(WFX_LINK_ID_AFTER_DTIM);
		idx = wfx_get_prio_queue(priv,
				tx_allowed_mask, &total);
		if (idx >= 0) {
			*more = total > 1;
			goto found;
		}
	}

	/* Search for unicast traffic */
	tx_allowed_mask = ~priv->sta_asleep_mask;
	tx_allowed_mask |= BIT(WFX_LINK_ID_UAPSD);
	if (priv->sta_asleep_mask) {
		tx_allowed_mask |= priv->pspoll_mask;
		tx_allowed_mask &= ~BIT(WFX_LINK_ID_AFTER_DTIM);
	} else {
		tx_allowed_mask |= BIT(WFX_LINK_ID_AFTER_DTIM);
	}
	idx = wfx_get_prio_queue(priv,
			tx_allowed_mask, &total);
	if (idx < 0)
		return -ENOENT;

found:
	*queue_p = &priv->tx_queue[idx];
	*tx_allowed_mask_p = tx_allowed_mask;
	return 0;
}

/*-----------------------------------------------------------------------*/
int wsm_get_tx(struct wfx_common *priv, u8 **data,
	       size_t *tx_len, int *burst)
{
	WsmHiTxReq_t *wsm = NULL;
	struct ieee80211_tx_info *tx_info;
	struct wfx_queue *queue = NULL;
	int queue_num;
	u32 tx_allowed_mask = 0;
	const struct wfx_txpriv *txpriv = NULL;
	int count = 0;

	/* More is used only for broadcasts. */
	bool more = false;

	if (priv->wsm_cmd.ptr) { /* CMD request */
		++count;
		spin_lock(&priv->wsm_cmd.lock);
		BUG_ON(!priv->wsm_cmd.ptr);
		*data = priv->wsm_cmd.ptr;
		*tx_len = priv->wsm_cmd.len;
		*burst = 1;
		spin_unlock(&priv->wsm_cmd.lock);
	} else {
		for (;;) {
			int ret;

			if (atomic_add_return(0, &priv->tx_lock))
				break;

			spin_lock_bh(&priv->ps_state_lock);

			ret = wsm_get_tx_queue_and_mask(priv, &queue,
							&tx_allowed_mask, &more);
			queue_num = queue - priv->tx_queue;

			if (priv->buffered_multicasts &&
			    (ret || !more) &&
			    (priv->tx_multicast || !priv->sta_asleep_mask)) {
				priv->buffered_multicasts = false;
				if (priv->tx_multicast) {
					priv->tx_multicast = false;
					queue_work(priv->workqueue,
						   &priv->multicast_stop_work);
				}
			}

			spin_unlock_bh(&priv->ps_state_lock);

			if (ret)
				break;

			if (wfx_queue_get(queue,
					     tx_allowed_mask,
					     &wsm, &tx_info, &txpriv))
				continue;

			if (wsm_handle_tx_data(priv, wsm,
					       tx_info, txpriv, queue))
				continue;  /* Handled by WSM */

			wsm->Header.s.b.IntId = 0;
			priv->pspoll_mask &= ~BIT(txpriv->raw_link_id);

			*data = (u8 *)wsm;
			*tx_len = __le16_to_cpu(wsm->Header.MsgLen);

			/* allow bursting if txop is set */
			if (priv->edca.params.TxOpLimit[queue_num])
				*burst = min(*burst,
					     (int)wfx_queue_get_num_queued(queue, tx_allowed_mask) + 1);
			else
				*burst = 1;

			/* store index of bursting queue */
			if (*burst > 1)
				priv->tx_burst_idx = queue_num;
			else
				priv->tx_burst_idx = -1;

			if (more) {
				struct ieee80211_hdr *hdr =
					(struct ieee80211_hdr *)
					&((u8 *)wsm)[txpriv->offset];
				hdr->frame_control |=
					cpu_to_le16(IEEE80211_FCTL_MOREDATA);
			}
			{
			struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)&((u8 *)wsm)[txpriv->offset];
			pr_debug("[WSM] Tx sta_id=%d >>> frame_ctrl=0x%.4x  tx_len=%zu, %p %c\n",
					txpriv->link_id,
					hdr->frame_control,
					*tx_len, *data,
					wsm->Body.More ? 'M' : ' ');
			}
			++count;
			break;
		}
	}

	return count;
}

/*-----------------------------------------------------------------------*/
void wsm_txed(struct wfx_common *priv, u8 *data)
{
	if (data == priv->wsm_cmd.ptr) {
		spin_lock(&priv->wsm_cmd.lock);
		priv->wsm_cmd.ptr = NULL;
		spin_unlock(&priv->wsm_cmd.lock);
	}
}

/*-----------------------------------------------------------------------*/