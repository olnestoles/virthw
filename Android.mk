LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := virt_hw
LOCAL_SRC_FILES :=  stralloc.c \
	gsm.c \
	sms.c \
	sim_card.c \
	remote_call.c \
	sysdeps_posix.c \
	modem_driver.c \
	android_modem.c \
	virt_hw.c \
	console.c \
	gps_client.c

LOCAL_SHARED_LIBRARIES := liblog

include $(BUILD_EXECUTABLE)
