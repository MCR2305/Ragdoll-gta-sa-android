LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE     := GTA4RagdollAndroid
LOCAL_SRC_FILES  := main.cpp
# Las cabeceras de AML se descargan en ./aml (lo hace el workflow de GitHub)
LOCAL_C_INCLUDES := $(LOCAL_PATH)/aml $(LOCAL_PATH)/aml/include
LOCAL_LDLIBS     := -llog
include $(BUILD_SHARED_LIBRARY)
