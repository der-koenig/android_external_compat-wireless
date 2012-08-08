/*
 * Copyright (c) 2004-2012 Atheros Communications Inc.
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

#include "core.h"
#include "debug.h"

static inline enum ap_keepalive_adjust __ap_keepalive_adjust_txrx_time(struct ath6kl_sta *conn, 
						u16 last_txrx_time, unsigned long now)
{
	u32 diff_ms;
	enum ap_keepalive_adjust adjust_result = AP_KA_ADJ_ERROR;

	/* NEED to be locked by conn->lock for safe. */

	/* 
	 * FIXME : No knowledge of target's time and here just using 1st query as 
	 *         base time and correct it to host time in each updated.
	 */	
	if (conn->last_txrx_time_tgt) {
		if (last_txrx_time >= conn->last_txrx_time_tgt)
			diff_ms = (last_txrx_time - conn->last_txrx_time_tgt) << 10;
		else	/* wrap */
			diff_ms = (0xffff - (conn->last_txrx_time_tgt - last_txrx_time)) << 10; 
		
		/* Update to last one. */
		conn->last_txrx_time_tgt = last_txrx_time;
		conn->last_txrx_time += msecs_to_jiffies(diff_ms);

		/* 
		 * Time scale between host (1000/HZ ms.) and target (ms./1024) is different. 
		 * It will cause that target't time may larger than host's in 1024 ms. in 
		 * worst case. Here simply correct it to sync to host's time.
		 */
		if (conn->last_txrx_time > now) 
			conn->last_txrx_time = now;

		adjust_result = AP_KA_ADJ_ADJUST;
	} else {
		/* First updated, treat as base time. */
		conn->last_txrx_time_tgt = last_txrx_time;
		conn->last_txrx_time = now;

		adjust_result = AP_KA_ADJ_BASESET;
	}

	return adjust_result;
}

static int _ap_keepalive_update_check_txrx_time(struct ath6kl_vif *vif,
						struct ath6kl_sta *conn, 
						u16 last_txrx_time)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;
	unsigned long now = jiffies;
	enum ap_keepalive_adjust adjust_result;
	int action = AP_KA_ACTION_NONE;

	spin_lock_bh(&conn->lock);
	adjust_result = __ap_keepalive_adjust_txrx_time(conn, last_txrx_time, now);
	if (adjust_result == AP_KA_ADJ_ADJUST) {
		if (now - conn->last_txrx_time >= msecs_to_jiffies(ap_keepalive->ap_ka_interval))
			action = AP_KA_ACTION_POLL;

		if (now - conn->last_txrx_time >= msecs_to_jiffies(ap_keepalive->ap_ka_remove_time))
			action = AP_KA_ACTION_REMOVE;
	}
	spin_unlock_bh(&conn->lock);

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive check (aid %d mac %02x:%02x:%02x:%02x:%02x:%02x) tgt/hst/now %d %ld %ld %s\n",
		   conn->aid,
		   conn->mac[0], conn->mac[1], conn->mac[2],
		   conn->mac[3], conn->mac[4], conn->mac[5],
		   conn->last_txrx_time_tgt,
		   conn->last_txrx_time,
		   now,
		   (action == AP_KA_ACTION_NONE) ? "NONE" : 
			((action == AP_KA_ACTION_POLL) ? "POLL" : "REMOVE"));

	return action;
}

static int ap_keepalive_preload_txrx_time(struct ath6kl_vif *vif)
{
	struct ath6kl *ar = vif->ar;
	int ret = 0;

	/* Get AP stats only at least one station associated. */
	if (vif->sta_list_index)
		ret = ath6kl_wmi_get_stats_cmd(ar->wmi, vif->fw_vif_idx);

	return ret;
}

static int ap_keepalive_update_check_txrx_time(struct ath6kl_vif *vif)
{
	struct ath6kl *ar = vif->ar;
	struct wmi_ap_mode_stat *ap_stats = &vif->ap_stats;
	struct wmi_per_sta_stat *per_sta_stat;
	struct ath6kl_sta *conn;
	int i, action;

	if (test_bit(STATS_UPDATE_PEND, &vif->flags)) {
		ath6kl_info("somebody still query now and ignore it this time.\n");
		return -EBUSY;
	}

	/* Now, tranfer to host time and update to vif->sta_list[]. */
	for (i = 0; i < AP_MAX_NUM_STA; i++) {
		per_sta_stat = &ap_stats->sta[i];
		if (per_sta_stat->aid) {
			conn = ath6kl_find_sta_by_aid(vif, per_sta_stat->aid);
			if (conn) {
				action = _ap_keepalive_update_check_txrx_time(vif, 
									      conn, 
									      per_sta_stat->last_txrx_time);

				if (action == AP_KA_ACTION_POLL) {
					ath6kl_wmi_ap_poll_sta(ar->wmi, 
							       vif->fw_vif_idx, 
							       conn->aid);
				} else if (action == AP_KA_ACTION_REMOVE) {
					ath6kl_wmi_ap_set_mlme(ar->wmi, 
							       vif->fw_vif_idx, 
							       WMI_AP_DEAUTH,
							       conn->mac, 
							       WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY);
				}
			} else {
				ath6kl_err("can't find this AID %d in STA list\n", per_sta_stat->aid);
			}
		}
	}

	return 0;
}

static void ap_keepalive_start(unsigned long arg)
{
	struct ap_keepalive_info *ap_keepalive = (struct ap_keepalive_info *) arg;
	struct ath6kl_vif *vif = ap_keepalive->vif;
	int ret;

	BUG_ON(!vif);

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive timer (vif idx %d) sta_list_index %x %s\n",
		   vif->fw_vif_idx,
		   vif->sta_list_index,
		   (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_PRELOAD_STAT) ? "preload" : "update check");

	if ((vif->nw_type == AP_NETWORK) &&
	    test_bit(CONNECTED, &vif->flags)) {		
	    	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_PRELOAD_STAT) {
			/* FIXME : get last_txrx_time pre N sec. and to check it in next run. */
			ret = ap_keepalive_preload_txrx_time(vif);
			if (ret) {
				ath6kl_err("preload last_txrx_time fail, ret %d\n", ret);
			}
		} else {
			/* Update and check last TXRX time each stations. */
			ret = ap_keepalive_update_check_txrx_time(vif);
			if (ret) {
				ath6kl_err("update and check last_txrx_time fail, ret %d\n", ret);
			}
	    	}

		if ((ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_START) &&
		    (ap_keepalive->ap_ka_interval)) {
		    	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_PRELOAD_STAT) {
				mod_timer(&ap_keepalive->ap_ka_timer,
					  jiffies + 
					  msecs_to_jiffies(ATH6KL_AP_KA_PRELOAD_STAT_TIME));
				ap_keepalive->flags &= ~ATH6KL_AP_KA_FLAGS_PRELOAD_STAT;
		    	} else {
				mod_timer(&ap_keepalive->ap_ka_timer,
					  jiffies + 
					  msecs_to_jiffies(ap_keepalive->ap_ka_interval) -
					  msecs_to_jiffies(ATH6KL_AP_KA_PRELOAD_STAT_TIME));
				ap_keepalive->flags |= ATH6KL_AP_KA_FLAGS_PRELOAD_STAT;
		    	}
		}
	}
	
	return;
}

struct ap_keepalive_info *ath6kl_ap_keepalive_init(struct ath6kl_vif *vif,
						   enum ap_keepalive_mode mode)
{
	struct ap_keepalive_info *ap_keepalive;

	ap_keepalive = kzalloc(sizeof(struct ap_keepalive_info), GFP_KERNEL);
	if (!ap_keepalive) {
		ath6kl_err("failed to alloc memory for ap_keepalive\n");
		return NULL;
	}

	ap_keepalive->vif = vif;
	ap_keepalive->ap_ka_interval = 0;
	ap_keepalive->ap_ka_reclaim_cycle = 0;
	if (mode == AP_KA_MODE_ENABLE) {
		ap_keepalive->flags |= ATH6KL_AP_KA_FLAGS_ENABLED;
		ap_keepalive->ap_ka_interval = ATH6KL_AP_KA_INTERVAL_DEFAULT;
		ap_keepalive->ap_ka_reclaim_cycle = ATH6KL_AP_KA_RECLAIM_CYCLE_DEFAULT;

#ifdef CONFIG_ANDROID
		if (vif->ar->p2p) {
			ap_keepalive->ap_ka_interval = ATH6KL_AP_KA_INTERVAL_DEFAULT_P2P;
			ap_keepalive->ap_ka_reclaim_cycle = ATH6KL_AP_KA_RECLAIM_CYCLE_DEFAULT_P2P;
		}
#endif
	} else if (mode == AP_KA_MODE_BYSUPP)
		ap_keepalive->flags |= ATH6KL_AP_KA_FLAGS_BY_SUPP;

	ap_keepalive->ap_ka_remove_time = ap_keepalive->ap_ka_interval * ap_keepalive->ap_ka_reclaim_cycle;

	/* Init. periodic scan timer. */
	init_timer(&ap_keepalive->ap_ka_timer);
	ap_keepalive->ap_ka_timer.function = ap_keepalive_start;
	ap_keepalive->ap_ka_timer.data = (unsigned long) ap_keepalive;

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive init (vif idx %d) interval %d cycle %d %s\n",
		   vif->fw_vif_idx,
		   ap_keepalive->ap_ka_interval,
		   ap_keepalive->ap_ka_reclaim_cycle,
		   (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_ENABLED) ? "ON" : 
		    ((ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_BY_SUPP) ? "SUPP" : "OFF"));
		
	return ap_keepalive;
}

void ath6kl_ap_keepalive_deinit(struct ath6kl_vif *vif)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;

	if (ap_keepalive) {
		if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_START)
			del_timer(&ap_keepalive->ap_ka_timer);

		kfree(ap_keepalive);
	}

	vif->ap_keepalive_ctx = NULL;

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive deinit (vif idx %d)\n",
		   vif->fw_vif_idx);

	return;
}

int ath6kl_ap_keepalive_start(struct ath6kl_vif *vif)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;

	BUG_ON(!ap_keepalive);
	BUG_ON(vif->nw_type != AP_NETWORK);

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive start (vif idx %d) flags %x\n",
		   vif->fw_vif_idx,
		   ap_keepalive->flags);

	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_ENABLED) {
		mod_timer(&ap_keepalive->ap_ka_timer, 
				jiffies + 
				msecs_to_jiffies(ap_keepalive->ap_ka_interval) - 
				msecs_to_jiffies(ATH6KL_AP_KA_PRELOAD_STAT_TIME));
		ap_keepalive->flags |= (ATH6KL_AP_KA_FLAGS_START | 
					ATH6KL_AP_KA_FLAGS_PRELOAD_STAT);		
	}

	return 0;
}

void ath6kl_ap_keepalive_stop(struct ath6kl_vif *vif)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;

	if (!ap_keepalive)
		return;

	/*
	 * Called by .del_beacon call-back (cfg80211) if the driver is controlled 
	 * by supplicant/hostapd, and called by .stop call-back (net_device)if the 
	 * driver is not controlled by supplicant/hostad.
	 */
	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive stop (vif idx %d) flags %x\n",
		   vif->fw_vif_idx,
		   ap_keepalive->flags);

	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_START) {
		del_timer(&ap_keepalive->ap_ka_timer);
		ap_keepalive->flags &= ~(ATH6KL_AP_KA_FLAGS_START | 
					 ATH6KL_AP_KA_FLAGS_PRELOAD_STAT);
	}

	return;
}

int ath6kl_ap_keepalive_config(struct ath6kl_vif *vif, 
			       u32 ap_ka_interval, 
			       u32 ap_ka_reclaim_cycle)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;
	int restart = 0;

	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_BY_SUPP) {
		ath6kl_dbg(ATH6KL_DBG_INFO,
			"already offlad to supplicant/hostapd, bypass it.\n");
		return 0;
	}

	/* FIXME : the better code protection by the spin-lock. */

	if ((ap_ka_interval != 0) &&
	    (ap_ka_interval < ATH6KL_AP_KA_INTERVAL_MIN))
		ap_ka_interval = ATH6KL_AP_KA_INTERVAL_MIN;

	if (ap_ka_reclaim_cycle == 0)
		ap_ka_reclaim_cycle = 1;

	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_START) {
		del_timer(&ap_keepalive->ap_ka_timer);
		ap_keepalive->flags &= ~(ATH6KL_AP_KA_FLAGS_START | 
					 ATH6KL_AP_KA_FLAGS_PRELOAD_STAT);
		restart = 1;
	}

	if (ap_ka_interval == 0) {
		ap_keepalive->ap_ka_interval = 0;
		ap_keepalive->ap_ka_reclaim_cycle = 0;
		ap_keepalive->flags &= ~ATH6KL_AP_KA_FLAGS_ENABLED;
	} else {
		if (ap_ka_interval * ap_ka_reclaim_cycle < ATH6KL_AP_KA_RECLAIM_TIME_MAX) {
			ap_keepalive->ap_ka_interval = ap_ka_interval;
			ap_keepalive->ap_ka_reclaim_cycle = ap_ka_reclaim_cycle;
		} else {
			ap_keepalive->ap_ka_interval = ATH6KL_AP_KA_INTERVAL_DEFAULT;
			ap_keepalive->ap_ka_reclaim_cycle = ATH6KL_AP_KA_RECLAIM_CYCLE_DEFAULT;
		}

		ap_keepalive->ap_ka_remove_time = ap_keepalive->ap_ka_interval * ap_keepalive->ap_ka_reclaim_cycle;
		ap_keepalive->flags |= ATH6KL_AP_KA_FLAGS_ENABLED;	
			
		if (restart) {
			mod_timer(&ap_keepalive->ap_ka_timer, 
					jiffies + 
					msecs_to_jiffies(ap_keepalive->ap_ka_interval) -
					msecs_to_jiffies(ATH6KL_AP_KA_PRELOAD_STAT_TIME * 1000));
			ap_keepalive->flags |= (ATH6KL_AP_KA_FLAGS_START | 
						ATH6KL_AP_KA_FLAGS_PRELOAD_STAT);
		}
	}

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive config (vif idx %d interval %d cycle %d %s restart %d)\n",
		   vif->fw_vif_idx,
		   ap_keepalive->ap_ka_interval,
		   ap_keepalive->ap_ka_reclaim_cycle,
		   (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_ENABLED) ? "ON" : "OFF",
		   restart);


	return 0;
}

/* Offload to supplicant/hostapd */
static u32 ap_keepalive_get_inactive_time(struct ath6kl_vif *vif, struct ath6kl_sta *conn)
{
	struct wmi_ap_mode_stat *ap_stats = &vif->ap_stats;
	u32 inact_time = 0;
	int i;

	for (i = 0; i < AP_MAX_NUM_STA; i++) {
		struct wmi_per_sta_stat *per_sta_stat = &ap_stats->sta[i];
		
		if (per_sta_stat->aid == conn->aid) {
			unsigned long now = jiffies;

			spin_lock_bh(&conn->lock);
			__ap_keepalive_adjust_txrx_time(conn, per_sta_stat->last_txrx_time, now);

			/* get inactive time. */
			inact_time = jiffies_to_msecs(now - conn->last_txrx_time);
			spin_unlock_bh(&conn->lock);

			ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
				   "ap_keepalive inact tgt/hst/now %d %ld %ld \n",
				   conn->last_txrx_time_tgt,
				   conn->last_txrx_time,
				   now);
		}
	}

	return inact_time;
}

u32 ath6kl_ap_keepalive_get_inactive_time(struct ath6kl_vif *vif, u8 *mac)
{
	struct ath6kl_sta *conn;
	u32 inact_time;

	conn = ath6kl_find_sta(vif, mac);

	if (conn) {
		inact_time = ap_keepalive_get_inactive_time(vif, conn);
	} else {
		inact_time = 0;		/* FIXME : return -1 ? */

		ath6kl_err("can't find sta %02x:%02x:%02x:%02x:%02x:%02x vif-idx %d\n", 
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 
			vif->fw_vif_idx);
	}

	ath6kl_dbg(ATH6KL_DBG_KEEPALIVE,
		   "ap_keepalive inact aid %d inact_time %d ms \n",
		   (conn ? (conn->aid) : 0),
		   inact_time);

	return inact_time;
}

bool ath6kl_ap_keepalive_by_supp(struct ath6kl_vif *vif)
{
	struct ap_keepalive_info *ap_keepalive = vif->ap_keepalive_ctx;

	if (!ap_keepalive)
		return false;

	if (ap_keepalive->flags & ATH6KL_AP_KA_FLAGS_BY_SUPP)
		return true;
	else
		return false;
}

