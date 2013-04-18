/*
 * Copyright (c) 2011 Atheros Communications Inc.
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
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <../arch/arm/mach-msm/include/mach/oem_rapi_client.h>
#define MAC_FILE "ath6k/AR6003/hw2.1.1/softmac"

typedef char            A_CHAR;
extern int android_readwrite_file(const A_CHAR *filename, A_CHAR *rbuf, const A_CHAR *wbuf, size_t length);
void get_macaddress(u8 *macaddr);

/* Bleh, same offsets. */
#define AR6003_MAC_ADDRESS_OFFSET 0x16
#define AR6004_MAC_ADDRESS_OFFSET 0x16
/*-- FelexChing - 20120509 Add for r/w nv wlan mac --*/
#define Buff_Lenght 32
#define Buff_Size (Buff_Lenght*sizeof(u8))
/*-- FelexChin - 20120509 Add for r/w nv wlan mac--*/

/* Global variables, sane coding be damned. */
u8 *ath6kl_softmac;
size_t ath6kl_softmac_len;

static void ath6kl_calculate_crc(u32 target_type, u8 *data, size_t len)
{
	u16 *crc, *data_idx;
	u16 checksum;
	int i;

	if (target_type == TARGET_TYPE_AR6003) {
		crc = (u16 *)(data + 0x04);
	} else if (target_type == TARGET_TYPE_AR6004) {
		len = 1024;
		crc = (u16 *)(data + 0x04);
	} else {
		ath6kl_err("Invalid target type\n");
		return;
	}

	ath6kl_dbg(ATH6KL_DBG_BOOT, "Old Checksum: %u\n", *crc);

	*crc = 0;
	checksum = 0;
	data_idx = (u16 *)data;

	for (i = 0; i < len; i += 2) {
		checksum = checksum ^ (*data_idx);
		data_idx++;
	}

	*crc = cpu_to_le16(checksum);

	ath6kl_dbg(ATH6KL_DBG_BOOT, "New Checksum: %u\n", checksum);
}

#ifdef CONFIG_MACH_PX
static int ath6kl_fetch_nvmac_info(struct ath6kl *ar)
{
	char softmac_filename[256];
	int ret = 0;

	do {
		snprintf(softmac_filename, sizeof(softmac_filename),
			 "/data/.nvmac.info");

		if ( (ret = android_readwrite_file(
			softmac_filename, NULL, NULL, 0)) < 0) {
			break;
		} else {
			ath6kl_softmac_len = ret;
		}
		ath6kl_softmac = vmalloc(ath6kl_softmac_len);
		if (!ath6kl_softmac) {
			ath6kl_dbg(ATH6KL_DBG_BOOT,
			   "%s: Cannot allocate buffer for nvmac.info"
			   "(%d)\n", __func__,ath6kl_softmac_len);
			ret = -ENOMEM;
			break;
		}

		if ( (ret = android_readwrite_file(softmac_filename,
			 (char*)ath6kl_softmac, NULL, ath6kl_softmac_len)) !=
				 ath6kl_softmac_len) {
			ath6kl_dbg(ATH6KL_DBG_BOOT,"%s: file read error, length %d\n",
					 __func__, ath6kl_softmac_len);
			vfree(ath6kl_softmac);
			ret = -1;
			break;
		}

		if (!strncmp(ath6kl_softmac,"00:00:00:00:00:00",17)) {
			ath6kl_dbg(ATH6KL_DBG_BOOT,"%s: mac address is zero\n",
					 __func__);
			vfree(ath6kl_softmac);
			ret = -1;
			break;
		}

		ret = 0;
	} while (0);

	return ret;
}

#else
static int ath6kl_fetch_mac_file(struct ath6kl *ar)
{
	const struct firmware *fw_entry;
	int ret = 0;


	ret = request_firmware(&fw_entry, MAC_FILE, ar->dev);
	if (ret)
		return ret;

	ath6kl_softmac_len = fw_entry->size;
	ath6kl_softmac = kmemdup(fw_entry->data, fw_entry->size, GFP_KERNEL);

	if (ath6kl_softmac == NULL)
		ret = -ENOMEM;

	release_firmware(fw_entry);

	return ret;
}
#endif

void ath6kl_mangle_mac_address(struct ath6kl *ar, u8 locally_administered_bit)
{
	u8 *ptr_mac;
	int i, ret;
#ifdef CONFIG_MACH_PX
	unsigned int softmac[6];
#endif

	switch (ar->target_type) {
	case TARGET_TYPE_AR6003:
		ptr_mac = ar->fw_board + AR6003_MAC_ADDRESS_OFFSET;
		break;
	case TARGET_TYPE_AR6004:
		ptr_mac = ar->fw_board + AR6004_MAC_ADDRESS_OFFSET;
		break;
	default:
		ath6kl_err("Invalid Target Type\n");
		return;
	}

	ath6kl_dbg(ATH6KL_DBG_BOOT,
		   "MAC from EEPROM %02X:%02X:%02X:%02X:%02X:%02X\n",
		   ptr_mac[0], ptr_mac[1], ptr_mac[2],
		   ptr_mac[3], ptr_mac[4], ptr_mac[5]);

#ifdef CONFIG_MACH_PX
	ret = ath6kl_fetch_nvmac_info(ar);

	if (ret) {
		ath6kl_err("MAC address file not found\n");
		return;
	}

	if (sscanf(ath6kl_softmac, "%02x:%02x:%02x:%02x:%02x:%02x",
				&softmac[0], &softmac[1], &softmac[2],
				&softmac[3], &softmac[4], &softmac[5])==6) {

		for (i=0; i<6; ++i) {
			ptr_mac[i] = softmac[i] & 0xff;
		}
	}

	ath6kl_dbg(ATH6KL_DBG_BOOT,
			"MAC from SoftMAC %02X:%02X:%02X:%02X:%02X:%02X\n",
			ptr_mac[0], ptr_mac[1], ptr_mac[2],
			ptr_mac[3], ptr_mac[4], ptr_mac[5]);
	vfree(ath6kl_softmac);
#else
	ret = ath6kl_fetch_mac_file(ar);
	if (ret) {
		ath6kl_err("MAC address file not found\n");
		return;
	}

    get_macaddress(ath6kl_softmac);
	for (i = 0; i < ETH_ALEN; ++i) {
	   ptr_mac[i] = ath6kl_softmac[i] & 0xff;
	}

	kfree(ath6kl_softmac);
#endif

	if (locally_administered_bit)
		ptr_mac[0] |= 0x02;

	if (locally_administered_bit)
		ptr_mac[0] |= 0x02;

	ath6kl_calculate_crc(ar->target_type, ar->fw_board, ar->fw_board_len);
}

/*-- FelexChing - 20120509 Add for r/w nv wlan mac --*/
void get_macaddress(u8 *macaddr)
{
	struct oem_rapi_client_streaming_func_arg arg;
	struct oem_rapi_client_streaming_func_ret ret;	
	char* input;
	char* output;
	int out_len;
	int i;
	uint mac[6];

	struct msm_rpc_client *mrc;

	input = kzalloc(Buff_Size, GFP_KERNEL);
	output = kzalloc(Buff_Size, GFP_KERNEL);

	arg.event = OEM_RAPI_CLIENT_EVENT_WLAN_MAC_GET;
	arg.cb_func = NULL;
	arg.handle = (void *)0;
	arg.in_len = strlen(input) + 1;
	arg.input = input;
	arg.output_valid = 1;
	arg.out_len_valid = 1;
	arg.output_size = Buff_Size;

	ret.output = output;
	ret.out_len = &out_len;

	mrc = oem_rapi_client_init();
	oem_rapi_client_streaming_function(mrc, &arg, &ret);
	oem_rapi_client_close();
	
    sscanf(ret.output, "%u:%u:%u:%u:%u:%u",
	    &mac[0], &mac[1], &mac[2],
	    &mac[3], &mac[4], &mac[5]);
	if(mac[0] == 0x00 && mac[1] == 0x03 && mac[2] == 0x7F && mac[3] == 0x04 && mac[4] == 0x02 && mac[5] == 0x12){

           mac[0] = 0x90;
	    mac[1] = 0xc1;
	    mac[2] = 0x15;
	    get_random_bytes(&i, sizeof(uint));
           mac[3] = (i & 0xff);
	    get_random_bytes(&i, sizeof(uint));
           mac[4] = (i & 0xff);
	    get_random_bytes(&i, sizeof(uint));
           mac[5] = (i & 0xff);
       }
		
	for (i = 0; i < 6; ++i) 
	    macaddr[i] = mac[i] & 0xff;
	
	kfree(input);
	kfree(output);
}
/*-- FelexChing - 20120509 Add for r/w nv wlan mac --*/
