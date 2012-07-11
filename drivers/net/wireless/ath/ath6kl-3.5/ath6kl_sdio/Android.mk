ifeq ($(BOARD_HAS_ATH_WLAN_AR6004), true)
LOCAL_PATH := $(call my-dir)
DLKM_DIR := build/dlkm
include $(CLEAR_VARS)
LOCAL_MODULE             := ath6kl_sdio.ko
LOCAL_MODULE_TAGS        := debug
LOCAL_MODULE_KBUILD_NAME := wlan.ko
KBUILD_OPTIONS := KBUILD_EXTRA_SYMBOLS=../external/compat-wireless/Module.symvers BUILD_ATH6KL_TYPE=sdio
LOCAL_MODULE_PATH        := $(TARGET_OUT)/lib/modules/ath6kl-3.5
include $(DLKM_DIR)/AndroidKernelModule.mk
endif
