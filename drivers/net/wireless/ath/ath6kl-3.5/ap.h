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

#ifndef AP_H
#define AP_H

/* Time defines */
#define ATH6KL_AP_KA_INTERVAL_DEFAULT		(15 * 1000)	/* in ms. */
#define ATH6KL_AP_KA_INTERVAL_MIN		(5 * 1000)	/* in ms. */
#define ATH6KL_AP_KA_RECLAIM_CYCLE_DEFAULT	(4)		/* Reclaim STA after 60 sec. */
#define ATH6KL_AP_KA_RECLAIM_TIME_MAX		((15 * 60) * 1000) /* in ms., means (ap_ka_interval * ap_ka_reclaim_cycle) */

#define ATH6KL_AP_KA_PRELOAD_STAT_TIME		(2 * 1000)	/* in ms., equal to WMI_TIMEOUT */ 

/* For P2P case, use the lower values for Android platform. */
#define ATH6KL_AP_KA_INTERVAL_DEFAULT_P2P	(10 * 1000)	/* in ms. */
#define ATH6KL_AP_KA_RECLAIM_CYCLE_DEFAULT_P2P	(2)		/* Reclaim STA after 20 sec. */

/* flags */
#define ATH6KL_AP_KA_FLAGS_ENABLED		BIT(0)
#define ATH6KL_AP_KA_FLAGS_BY_SUPP		BIT(1)		/* Offload to supplicant/hostapd, decided when loading module. */
#define ATH6KL_AP_KA_FLAGS_START		BIT(2)
#define ATH6KL_AP_KA_FLAGS_PRELOAD_STAT		BIT(3)		/* means this fired timer is for preload stat purpose. */

/* Next action */
#define AP_KA_ACTION_NONE			(0)
#define AP_KA_ACTION_POLL			(1)
#define AP_KA_ACTION_REMOVE			(2)

/* Operation mode */
enum ap_keepalive_mode {
	AP_KA_MODE_DISABLE = 0,
	AP_KA_MODE_ENABLE,
	AP_KA_MODE_BYSUPP,
};

enum ap_keepalive_adjust {
	AP_KA_ADJ_ERROR = -1,
	AP_KA_ADJ_BASESET = 0,
	AP_KA_ADJ_ADJUST,
};

struct ap_keepalive_info {
	struct ath6kl_vif *vif;
	u32 flags;

	struct timer_list ap_ka_timer;
	u32 ap_ka_interval;		/* In ms., run checking per ap_ka_interval */
	u32 ap_ka_reclaim_cycle;	/* Reclaim STA after N checking fails */
	u32 ap_ka_remove_time;		/* Remove this station after (ap_ka_interval * ap_ka_reclaim_cycle) */
};

struct ap_keepalive_info *ath6kl_ap_keepalive_init(struct ath6kl_vif *vif, 
						   enum ap_keepalive_mode mode);
void ath6kl_ap_keepalive_deinit(struct ath6kl_vif *vif);
int ath6kl_ap_keepalive_start(struct ath6kl_vif *vif);
void ath6kl_ap_keepalive_stop(struct ath6kl_vif *vif);
int ath6kl_ap_keepalive_config(struct ath6kl_vif *vif, 
			       u32 ap_ka_interval, 
			       u32 ap_ka_reclaim_cycle);
u32 ath6kl_ap_keepalive_get_inactive_time(struct ath6kl_vif *vif, u8 *mac);
bool ath6kl_ap_keepalive_by_supp(struct ath6kl_vif *vif);
#endif

