LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := ImprovedIdles
LOCAL_SRC_FILES := ImprovedIdles.cpp Audio.cpp
LOCAL_CPPFLAGS += -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_LDLIBS    := -llog -lOpenSLES
include $(BUILD_SHARED_LIBRARY)
