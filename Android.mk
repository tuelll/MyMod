LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := HUDOpacity

LOCAL_CFLAGS := -O2 -std=c++17 -fvisibility=hidden
LOCAL_CPPFLAGS := -std=c++17

LOCAL_SRC_FILES := main.cpp

LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)