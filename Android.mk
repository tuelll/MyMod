LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE     := RealisticDamage
LOCAL_SRC_FILES  := realistic_damage.cpp
LOCAL_CPPFLAGS   := -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_LDLIBS     := -llog -ldl

# --- Se for usar o AML (Alternative Mod Loader), descomente e ajuste o caminho do SDK:
# LOCAL_CPPFLAGS   += -DUSE_AML
# LOCAL_C_INCLUDES += $(LOCAL_PATH)/aml_sdk/include

include $(BUILD_SHARED_LIBRARY)
