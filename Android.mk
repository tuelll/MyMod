LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := OilLeaking
LOCAL_SRC_FILES := main.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_CPPFLAGS  += -std=c++17 -O2 -fvisibility=hidden -fno-exceptions -fno-rtti
LOCAL_LDLIBS    := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
