/*
 * Copyright (c) 2010-2011 Atheros Communications Inc.
 * Copyright (c) 2011 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <linux/printk.h>
#include <asm/unaligned.h>

#include "core.h"
#include "cfg80211.h"
#include "debug.h"
#include "hif-ops.h"
#include "testmode.h"
#include "wmiconfig.h"
#include "wmi.h"

#include <net/netlink.h>

enum ath6kl_tm_attr {
	__ATH6KL_TM_ATTR_INVALID	= 0,
	ATH6KL_TM_ATTR_CMD		= 1,
	ATH6KL_TM_ATTR_DATA		= 2,

	/* keep last */
	__ATH6KL_TM_ATTR_AFTER_LAST,
	ATH6KL_TM_ATTR_MAX		= __ATH6KL_TM_ATTR_AFTER_LAST - 1,
};

enum ath6kl_tm_cmd {
	ATH6KL_TM_CMD_TCMD		= 0,
	ATH6KL_TM_CMD_RX_REPORT		= 1,	/* not used anymore */
	ATH6KL_TM_CMD_WMI_CMD		= 0xF000,
};

#define AP_ACS_NONE AP_ACS_POLICY_MAX

typedef struct {
    uint32_t cmd;

	uint32_t band_info_valid;
    uint32_t ul_freq;
    uint32_t ul_bw;
    uint32_t dl_freq;
    uint32_t dl_bw;

	uint32_t tdd_info_valid;
    uint32_t fr_offset;
    uint32_t tdd_cfg;
    uint32_t sub_fr_cfg;
    uint32_t ul_cfg;
    uint32_t dl_cfg;

	uint32_t off_period_valid;
    uint32_t off_period;
}qmi_coex_wwan_data_t;

#define COEX_DBG(fmt, ...) coex_dbg(__LINE__,__func__,fmt, ##__VA_ARGS__)
#define GET_COEX_MODE(coex_mode)  ( (coex_mode == WLAN_COEX_MODE_CHANNEL_AVOIDANCE) ? "CA" : \
						 		(coex_mode == WLAN_COEX_MODE_3WIRE) ? "3W" : \
								(coex_mode == WLAN_COEX_MODE_PWR_BACKOFF) ? "PB" : "XX" )

#define GET_WWAN_MODE(wwan_mode) ( (wwan_mode == WWAN_MODE_TDD_CONFIG) ? "TDD" : \
								(wwan_mode == WWAN_MODE_FDD_CONFIG) ? "FDD" : "XXX" )

#define GET_WWAN_STATE(wwan_state) ( (wwan_state == WWAN_STATE_CONNECTED) ? "CNT" : \
									(wwan_state == WWAN_STATE_IDLE) ? "IDL" : "XXX" )

#define GET_ACS_POLICY(acs)	( (acs==AP_ACS_NORMAL) ? "1&6&11":\
								(acs==AP_ACS_DISABLE_CH11) ? "1&6" :\
								(acs==AP_ACS_DISABLE_CH1) ? "6&11" :\
								(acs==AP_ACS_DISABLE_CH1_6) ? "11" :\
								(acs==AP_ACS_NONE) ? "X" :\
								"AP_ACS_INCLUDE_CH13")

#define GET_WWAN_BAND(band) ( (band & WWAN_B40) ? "TDD-B40" : \
							(band & WWAN_B41) ? "TDD-B41" :\
							(band & WWAN_B7) ? "FDD_B7" : \
							"NON-INF-BAND")

//TDD B40
#define WWAN_FREQ_2300 2300
#define WWAN_FREQ_2350 2350
#define WWAN_FREQ_2370 2370
#define WWAN_FREQ_2380 2380
#define WWAN_FREQ_2400 2400

//TDD B41 | FDD B7
#define WWAN_FREQ_2496 2496
#define WWAN_FREQ_2570 2570

//TDD B38
#define WWAN_FREQ_2570 2570
#define WWAN_FREQ_2620 2620

#define WWAN_TDD 0xF0
#define WWAN_B40 0x80
#define WWAN_B41 0x40
#define WWAN_B38 0x20

#define WWAN_FDD 0x0F
#define WWAN_B7	 0x08

#define WWAN_BAND 0xFF

#define CH1 2412
#define CH2 2417
#define CH3 2422
#define CH4 2427
#define CH5 2432
#define CH6 2437
#define CH7 2442
#define CH8 2447
#define CH9 2452
#define CH10 2457
#define CH11 2462
#define CH12 2467
#define CH13 2472
#define CH14 2484

struct _coex_chk {
	int wwan_min_freq;
	int	wwan_max_freq;
	int wlan_min_freq;
	int wlan_max_freq;
	uint8_t	sta_lte_coex_mode;
	uint8_t ap_lte_coex_mode;
	uint8_t ap_acs_ch;
	uint8_t wwan_band;
}coex_chk[]={
//wwan_min_freq		wwan_max_freq	wlan_freq		sta_lte_coex_mode		ap_lte_coex_mode					ap_acs
{WWAN_FREQ_2300,	WWAN_FREQ_2350,	CH1,CH5,	WLAN_COEX_MODE_3WIRE, 	 	WLAN_COEX_MODE_CHANNEL_AVOIDANCE,   AP_ACS_DISABLE_CH1,		WWAN_B40},
{WWAN_FREQ_2300,	WWAN_FREQ_2350,	CH6,CH14,	WLAN_COEX_MODE_DISABLED,	WLAN_COEX_MODE_DISABLED,            AP_ACS_DISABLE_CH1, 	WWAN_B40},

{WWAN_FREQ_2350,	WWAN_FREQ_2370,	CH1, CH10,	WLAN_COEX_MODE_3WIRE,    	WLAN_COEX_MODE_CHANNEL_AVOIDANCE,   AP_ACS_DISABLE_CH1_6,	WWAN_B40},
{WWAN_FREQ_2350,	WWAN_FREQ_2370,	CH11,CH14,	WLAN_COEX_MODE_DISABLED,    WLAN_COEX_MODE_DISABLED,            AP_ACS_DISABLE_CH1_6,	WWAN_B40},

{WWAN_FREQ_2370,	WWAN_FREQ_2380,	CH1,CH11,	WLAN_COEX_MODE_PWR_BACKOFF,	WLAN_COEX_MODE_PWR_BACKOFF,      	AP_ACS_DISABLE_CH1_6,	WWAN_B40},
{WWAN_FREQ_2370,	WWAN_FREQ_2380,	CH12,CH14,	WLAN_COEX_MODE_PWR_BACKOFF,	WLAN_COEX_MODE_PWR_BACKOFF,      	AP_ACS_DISABLE_CH1_6,	WWAN_B40},

{WWAN_FREQ_2380,	WWAN_FREQ_2400,	CH1,CH11,	WLAN_COEX_MODE_3WIRE,    	WLAN_COEX_MODE_3WIRE,               AP_ACS_DISABLE_CH1_6,	WWAN_B40},
{WWAN_FREQ_2380,	WWAN_FREQ_2400,	CH12,CH14,	WLAN_COEX_MODE_3WIRE,    	WLAN_COEX_MODE_3WIRE,               AP_ACS_DISABLE_CH1_6,	WWAN_B40},

//Coex mode same for TDD B41 and FDD B7
{WWAN_FREQ_2496, 	WWAN_FREQ_2570,	CH1,CH9,	WLAN_COEX_MODE_DISABLED,	WLAN_COEX_MODE_DISABLED,            AP_ACS_DISABLE_CH11,	WWAN_B41|WWAN_B7},
{WWAN_FREQ_2496,	WWAN_FREQ_2570,	CH10,CH14,	WLAN_COEX_MODE_3WIRE,    	WLAN_COEX_MODE_CHANNEL_AVOIDANCE,   AP_ACS_DISABLE_CH11,	WWAN_B41|WWAN_B7},

//No coex needed for TDD B38
//{WWAN_FREQ_2570, WWAN_FREQ_2620,	CH1,	CH14,	WLAN_COEX_MODE_DISABLED,    WLAN_COEX_MODE_DISABLED,            AP_ACS_NONE, 	WWAN_B38},
};

#define COEX_TX_PWR_MAX 20
const int8_t max_tx_pwr_arr_b40[15]={
//		2310	2330	2350	2370	2390	Other band
/*CH1*/	5,		5,		5,		5,		-10,
/*CH6*/	20,		20,		20,		10,		0,
/*CH11*/20,		20,		20,		15,		10};

const int8_t max_tx_pwr_arr_b41[15]={
//		2500	2520	2540	2560	2580	Other band
/*CH1*/	10,		15,		20,		20,		20,
/*CH6*/	0,		10,		20,		20,		20,
/*CH11*/-10,	5,		5,		5,		5};
const uint32_t lte_tdd_b40_freq_arr[] ={ 2310, 2330, 2350, 2370, 2390};
const uint32_t wlan_acs_ch_freq_arr[] ={ CH1, CH6, CH11, 0};

coex_priv *coex;
int wwan_operational =0;

static void coex_dbg(int line, const char *fn, const char *fmt, ...)
{
    struct va_format vaf;
    va_list args;

    va_start(args, fmt);
    vaf.fmt = fmt;
    vaf.va = &args;
	//printk(KERN_WARNING"lte_coex:%s:%d: %pV\n",fn, line, &vaf);
	printk(KERN_INFO"lte_coex: %pV\n",&vaf);
    va_end(args);
}

static struct sk_buff *coex_wmi_get_buf(u32 size)
{
	struct sk_buff *skb;

	skb = ath6kl_buf_alloc(size);
	if (!skb)
		return NULL;

	skb_put(skb, size);
	if (size)
		memset(skb->data, 0, size);

	return skb;
}

static int coex_wlan_get_max_tx_pwr(int wlan_freq)
{
	int8_t i,j;
	int8_t *max_tx_pwr_arr;

	if(wlan_freq == 0)
		return COEX_TX_PWR_MAX;

	if(coex->wwan_band & WWAN_B40)
		max_tx_pwr_arr = (int8_t*)max_tx_pwr_arr_b40;
	else if(coex->wwan_band & WWAN_B41)
		max_tx_pwr_arr = (int8_t*)max_tx_pwr_arr_b41;
	else //for B7 on ul band interferes and other band discard
		return COEX_TX_PWR_MAX;

	for (i=0;i<4;i++){
		if(!(coex->wwan_freq > lte_tdd_b40_freq_arr[i]))
			break;
	}

	for(j=0;j<2;j++){
		if(!(wlan_freq > wlan_acs_ch_freq_arr[j]))
			break;
	}

	return max_tx_pwr_arr[j*5+i];
}

static void coex_send_wmi_cmd(void)
{
	struct sk_buff *skb;
	struct ath6kl *ar = coex->ar;
    WMI_SET_LTE_COEX_STATE_CMD *fw_wmi_lte_data;

	skb  =(struct sk_buff *) coex_wmi_get_buf(sizeof(WMI_SET_LTE_COEX_STATE_CMD));
	fw_wmi_lte_data =(WMI_SET_LTE_COEX_STATE_CMD*) (skb->data);
	if (!fw_wmi_lte_data){
		COEX_DBG("wmi cmd send fail");
		return;
	}

	memcpy(fw_wmi_lte_data, &coex->wmi_lte_data,sizeof(WMI_SET_LTE_COEX_STATE_CMD));
	COEX_DBG("SCM:%s ACM: %s WWM:%s WWS:%s TDD:%d ATP:%d STP:%d OFP:%d",
				GET_COEX_MODE(fw_wmi_lte_data->sta_lte_coex_mode),GET_COEX_MODE(fw_wmi_lte_data->ap_lte_coex_mode),
				GET_WWAN_MODE(fw_wmi_lte_data->wwan_mode), GET_WWAN_STATE(fw_wmi_lte_data->wwan_state),
				fw_wmi_lte_data->wwan_tdd_cfg, fw_wmi_lte_data->ap_max_tx_pwr, fw_wmi_lte_data->sta_max_tx_pwr,
				fw_wmi_lte_data->wwan_off_period);

	ath6kl_wmi_cmd_send(ar->wmi, 0, skb, WMI_SET_LTE_COEX_STATE_CMDID, NO_SYNC_WMIFLAG);
}

static void coex_change_ap_acs(struct ath6kl *ar)
{
    struct ath6kl_vif *vif;

	spin_lock_bh(&ar->list_lock);
	list_for_each_entry(vif, &ar->vif_list, list) {
		if(vif->nw_type == AP_NETWORK){
			vif->profile.ch = cpu_to_le16(coex->ap_acs_ch);
			ath6kl_wmi_ap_profile_commit(ar->wmi, vif->fw_vif_idx,
						&vif->profile);
		}
	}
	spin_unlock_bh(&ar->list_lock);
}

static void coex_setup_wlan_sta_coex_mode(coex_priv *coex, int send_wmi_cmd)
{
	int i,j;

	if(wwan_operational == 0)
		return ;

	coex->wmi_lte_data.sta_lte_coex_mode = WLAN_COEX_MODE_DISABLED;
	//Select wwan band
	for (i=0; i<10; i++){
		if(	coex->wwan_freq >= coex_chk[i].wwan_min_freq && coex->wwan_freq < coex_chk[i].wwan_max_freq){
			//select wlan band
			for(j=i; j<=i+1; j++){
				if(coex->sta_freq >= coex_chk[j].wlan_min_freq && coex->sta_freq <= coex_chk[j].wlan_max_freq){
					coex->wmi_lte_data.sta_lte_coex_mode = coex_chk[j].sta_lte_coex_mode;
					coex->wwan_band &= coex_chk[j].wwan_band;
					break;
				}
			}
			break;
		}
	}

	coex->wmi_lte_data.sta_max_tx_pwr = coex_wlan_get_max_tx_pwr(coex->sta_freq);

	if(send_wmi_cmd)
		coex_send_wmi_cmd();
}

static void coex_setup_wlan_ap_coex_mode(coex_priv *coex, int send_wmi_cmd)
{
	int i,j;

	if(wwan_operational == 0)
		return ;

	coex->wmi_lte_data.ap_lte_coex_mode = WLAN_COEX_MODE_DISABLED;
	coex->ap_acs_ch = AP_ACS_NONE;
	//Select wwan band
	for (i=0; i<10; i++){
		if(	coex->wwan_freq >= coex_chk[i].wwan_min_freq && coex->wwan_freq < coex_chk[i].wwan_max_freq){
			//select wlan band
			if(!coex->ap_freq){//AP not up, note down for future update
				coex->ap_acs_ch = coex_chk[i].ap_acs_ch;
				coex->wwan_band &= coex_chk[i].wwan_band;
				break;
			}
			for(j=i; j<=i+1; j++){
				if(coex->ap_freq >= coex_chk[j].wlan_min_freq && coex->ap_freq <= coex_chk[j].wlan_max_freq){ //AP up
                    coex->wmi_lte_data.ap_lte_coex_mode = coex_chk[j].ap_lte_coex_mode;
					coex->ap_acs_ch = coex_chk[j].ap_acs_ch;
					coex->wwan_band &= coex_chk[j].wwan_band;
					break;
				}
			}
			break;
		}
	}

	if(coex->ap_acs_ch != AP_ACS_NONE && coex->ap_acs_ch != coex->prev_ap_acs_ch && coex->ap_freq
		&& coex->wmi_lte_data.ap_lte_coex_mode != WLAN_COEX_MODE_DISABLED){
		COEX_DBG("Moving AP to ACS:%s",GET_ACS_POLICY(coex->ap_acs_ch));
		coex->prev_ap_acs_ch = coex->ap_acs_ch;
		//coex->ap_evt_acs = 1;
		coex_change_ap_acs(coex->ar);
		send_wmi_cmd = 0;
	}else COEX_DBG("Active AP ACS: %s %d %d %d %d",GET_ACS_POLICY(coex->ap_acs_ch), coex->ap_acs_ch, coex->prev_ap_acs_ch, coex->ap_freq, coex->wmi_lte_data.ap_lte_coex_mode);

	coex->wmi_lte_data.ap_max_tx_pwr = coex_wlan_get_max_tx_pwr(coex->ap_freq);

	if(send_wmi_cmd)
		coex_send_wmi_cmd();
}

void ath6kl_coex_update_wwan_data(void * wmi_buf)
{
	qmi_coex_wwan_data_t * wwan = (qmi_coex_wwan_data_t *)wmi_buf;

	COEX_DBG("QMI LTE band info:%d %d %d",wwan->band_info_valid, wwan->ul_freq, wwan->dl_freq);
	COEX_DBG("QMI TDD CFG:%d %d", wwan->tdd_info_valid, wwan->tdd_cfg);
	COEX_DBG("QMI off period:%d %d",wwan->off_period_valid, wwan->off_period);

	if(!(wwan->band_info_valid == 1 || wwan->tdd_info_valid==1 ||
		wwan->off_period_valid == 1))
		return;

	wwan_operational = 1;

	if(wwan->band_info_valid == 1){
		coex->wwan_freq = wwan->ul_freq;

		if(wwan->ul_freq ==0) {
			if(wwan->dl_freq ==0){
				COEX_DBG("LTE deactivated. Disabling coex");
				coex->wmi_lte_data.wwan_mode = WWAN_MODE_INVALID;
				coex->wmi_lte_data.wwan_state = WWAN_STATE_DEACTIVATED;
				coex->wwan_band = 0;
			}else{
				COEX_DBG("LTE Idle Rx");
				coex->wwan_freq = wwan->dl_freq;
				coex->wmi_lte_data.wwan_state = WWAN_STATE_IDLE;
				coex->wwan_band = WWAN_BAND;
			}
		} else {
			COEX_DBG("LTE Active");
			if(wwan->ul_freq == wwan->dl_freq){
				coex->wmi_lte_data.wwan_mode = WWAN_MODE_TDD_CONFIG;
				coex->wwan_band = WWAN_TDD;
			}else{
				coex->wmi_lte_data.wwan_mode = WWAN_MODE_FDD_CONFIG;
				coex->wwan_band = WWAN_FDD;
			}
			coex->wmi_lte_data.wwan_state = WWAN_STATE_CONNECTED;
		}
	}

	if(wwan->tdd_info_valid == 1)
		coex->wmi_lte_data.wwan_tdd_cfg = wwan->tdd_cfg;

	if(wwan->off_period_valid ==1)
		coex->wmi_lte_data.wwan_off_period = wwan->off_period;


	coex_setup_wlan_sta_coex_mode(coex,0); //send wmi cmd only once
	coex_setup_wlan_ap_coex_mode(coex,1);
	COEX_DBG("WWAN BAND: %s",GET_WWAN_BAND(coex->wwan_band));

	coex->wmi_lte_data.wwan_off_period = 0;
}

void ath6kl_coex_update_wlan_data(struct ath6kl_vif *vif, uint32_t chan)
{
	if(vif->nw_type == INFRA_NETWORK){
		coex->sta_freq = chan;
		if(chan !=0)
			COEX_DBG("Station connected at %d Mhz",chan);
		else
			COEX_DBG("Station disconnected");

		coex_setup_wlan_sta_coex_mode(coex,1);
	}else if(vif->nw_type == AP_NETWORK){
		coex->ap_freq = chan;
		if(chan !=0){
			COEX_DBG("AP Enabled at freq %d Mhz",chan);

			if(coex->ap_evt_acs == 1){
				coex->ap_evt_acs = 0;
				return;
			}
		}else
			COEX_DBG("AP Shutdown");

		coex_setup_wlan_ap_coex_mode(coex, 1);
	}

}

void ath6kl_coex_init(struct ath6kl *ar)
{
	COEX_DBG("WWAN Coex Module Init");
	coex = (coex_priv *) kzalloc(sizeof(coex_priv) , GFP_KERNEL);
	coex->ar = ar;
	ar->coex = coex;
	coex->ap_acs_ch = AP_ACS_NONE;
}

void ath6kl_coex_deinit(struct ath6kl *ar)
{
	COEX_DBG("WWAN Coex Module Deinit");
	ar->coex = NULL;
	kfree(coex);
}

struct sk_buff *ath6kl_wmi_get_buf(u32 size)
{
	struct sk_buff *skb;

	skb = ath6kl_buf_alloc(size);
	if (!skb)
		return NULL;

	skb_put(skb, size);
	if (size)
		memset(skb->data, 0, size);

	return skb;
}
void ath6kl_tm_rx_wmi_event(struct ath6kl *ar, void *buf, size_t buf_len)
{
	struct sk_buff *skb;


	if (!buf || buf_len == 0)
		return;

	skb = cfg80211_testmode_alloc_event_skb(ar->wiphy, buf_len, GFP_KERNEL);
	if (!skb) {
		ath6kl_warn("failed to allocate testmode rx skb!\n");
		return;
	}
	NLA_PUT_U32(skb, ATH6KL_TM_ATTR_CMD, ATH6KL_TM_CMD_WMI_CMD);
	NLA_PUT(skb, ATH6KL_TM_ATTR_DATA, buf_len, buf);
	cfg80211_testmode_event(skb, GFP_KERNEL);
	return;

nla_put_failure:
	kfree_skb(skb);
	ath6kl_warn("nla_put failed on testmode rx skb!\n");
}


void ath6kl_wmicfg_send_stats(struct ath6kl_vif *vif,
			      struct target_stats *stats)
{
	u32 *buff = kzalloc(sizeof(*stats) + 4, GFP_KERNEL);

	buff[0] = WMI_REPORT_STATISTICS_EVENTID;
	memcpy(buff+1, stats, sizeof(struct target_stats));
	ath6kl_tm_rx_wmi_event(vif->ar->wmi->parent_dev, buff,
			       sizeof(struct target_stats)+4);
	kfree(buff);
}
