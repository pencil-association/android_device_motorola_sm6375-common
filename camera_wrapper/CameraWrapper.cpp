/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// Camera HAL module wrapper around the CamX camera.qcom.so.
//
// Motorola's CHI override advertises the sensors' real manual exposure range
// (up to 32 s) only in its private com.lenovo.moto.sensor.info.exposureTimeRange
// tag, which MotoCam reads. The standard android.sensor.info.exposureTimeRange
// keeps the longest normal sensor frame (~250 ms), so every other app caps its
// shutter at 1/4 s. Requests beyond that already work for any app: the CHI
// override routes every request whose exposure exceeds the HAL's own standard
// maximum into its long-exposure path.
//
// So the framework gets a copy of each camera's static metadata with the
// standard exposure maximum (and the maximum frame duration) raised to the
// Moto maximum. The HAL keeps its own unmodified metadata, which it still uses
// as the long-exposure threshold. Everything else is forwarded unchanged.

#define LOG_TAG "CameraWrapper"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

#include <map>
#include <mutex>

#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/camera_metadata.h>

namespace {

constexpr const char* kRealHal = "/vendor/lib64/hw/camera.qcom.so";
constexpr const char* kMotoSection = "com.lenovo.moto.sensor.info";
constexpr const char* kMotoRangeTag = "exposureTimeRange";

camera_module_t* gReal;
std::mutex gLock;

struct Patched {
    const camera_metadata_t* source = nullptr;
    camera_metadata_t* copy = nullptr;
};
std::map<int, Patched> gLogical;
std::map<int, Patched> gPhysical;

bool gMotoTagResolved;
uint32_t gMotoTag;
bool gHaveMotoTag;

// Caller holds gLock.
bool ResolveMotoTag() {
    if (gMotoTagResolved) return gHaveMotoTag;
    gMotoTagResolved = true;
    if (!gReal->get_vendor_tag_ops) return false;

    vendor_tag_ops_t ops = {};
    gReal->get_vendor_tag_ops(&ops);
    if (!ops.get_tag_count || !ops.get_all_tags || !ops.get_section_name || !ops.get_tag_name)
        return false;
    int count = ops.get_tag_count(&ops);
    if (count <= 0) return false;

    uint32_t* tags = new uint32_t[count];
    ops.get_all_tags(&ops, tags);
    for (int i = 0; i < count; i++) {
        const char* section = ops.get_section_name(&ops, tags[i]);
        const char* name = ops.get_tag_name(&ops, tags[i]);
        if (section && name && !strcmp(section, kMotoSection) && !strcmp(name, kMotoRangeTag)) {
            gMotoTag = tags[i];
            gHaveMotoTag = true;
            break;
        }
    }
    delete[] tags;
    if (!gHaveMotoTag) ALOGI("%s.%s not found; leaving metadata as is", kMotoSection, kMotoRangeTag);
    return gHaveMotoTag;
}

// Returns a patched copy, or nullptr when the camera needs no change.
camera_metadata_t* PatchExposureRange(const camera_metadata_t* src) {
    if (!ResolveMotoTag()) return nullptr;

    camera_metadata_ro_entry_t moto, standard;
    if (find_camera_metadata_ro_entry(src, gMotoTag, &moto) || moto.type != TYPE_INT64 ||
        moto.count != 2)
        return nullptr;
    if (find_camera_metadata_ro_entry(src, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, &standard) ||
        standard.count != 2)
        return nullptr;

    int64_t range[2] = {standard.data.i64[0], moto.data.i64[1]};
    if (range[1] <= standard.data.i64[1]) return nullptr;

    camera_metadata_t* copy = clone_camera_metadata(src);
    if (!copy) return nullptr;

    camera_metadata_entry_t entry;
    if (find_camera_metadata_entry(copy, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, &entry) ||
        update_camera_metadata_entry(copy, entry.index, range, 2, nullptr)) {
        free_camera_metadata(copy);
        return nullptr;
    }
    // A frame can't be shorter than its exposure.
    if (!find_camera_metadata_entry(copy, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &entry) &&
        entry.count == 1 && entry.data.i64[0] < range[1])
        update_camera_metadata_entry(copy, entry.index, &range[1], 1, nullptr);

    ALOGI("exposure time range [%lld, %lld] -> [%lld, %lld] ns",
          static_cast<long long>(standard.data.i64[0]), static_cast<long long>(standard.data.i64[1]),
          static_cast<long long>(range[0]), static_cast<long long>(range[1]));
    return copy;
}

// Swaps in the patched copy for @meta; caches it per camera.
void Substitute(std::map<int, Patched>& cache, int id, camera_metadata_t** meta) {
    if (!meta || !*meta) return;
    std::lock_guard<std::mutex> lock(gLock);
    Patched& p = cache[id];
    if (p.source != *meta) {
        if (p.copy) free_camera_metadata(p.copy);
        p.source = *meta;
        p.copy = PatchExposureRange(*meta);
    }
    if (p.copy) *meta = p.copy;
}

int GetNumberOfCameras() {
    if (!gReal) return 0;
    return gReal->get_number_of_cameras();
}

int GetCameraInfo(int id, struct camera_info* info) {
    if (!gReal) return -ENODEV;
    int ret = gReal->get_camera_info(id, info);
    if (!ret && info)
        Substitute(gLogical, id, const_cast<camera_metadata_t**>(&info->static_camera_characteristics));
    return ret;
}

int GetPhysicalCameraInfo(int id, camera_metadata_t** meta) {
    if (!gReal) return -ENODEV;
    int ret = gReal->get_physical_camera_info(id, meta);
    if (!ret) Substitute(gPhysical, id, meta);
    return ret;
}

int SetCallbacks(const camera_module_callbacks_t* callbacks) {
    if (!gReal) return -ENODEV;
    return gReal->set_callbacks(callbacks);
}

void GetVendorTagOps(vendor_tag_ops_t* ops) {
    if (!gReal) return;
    gReal->get_vendor_tag_ops(ops);
}

int OpenLegacy(const struct hw_module_t*, const char* id, uint32_t version,
               struct hw_device_t** device) {
    if (!gReal) return -ENODEV;
    return gReal->open_legacy(&gReal->common, id, version, device);
}

int SetTorchMode(const char* id, bool enabled) {
    if (!gReal) return -ENODEV;
    return gReal->set_torch_mode(id, enabled);
}

int Init() {
    if (!gReal) return -ENODEV;
    return gReal->init();
}

int IsStreamCombinationSupported(int id, const camera_stream_combination_t* streams) {
    if (!gReal) return -ENODEV;
    return gReal->is_stream_combination_supported(id, streams);
}

void NotifyDeviceStateChange(uint64_t state) {
    if (!gReal) return;
    gReal->notify_device_state_change(state);
}

int Open(const struct hw_module_t*, const char* id, struct hw_device_t** device) {
    if (!gReal) return -ENODEV;
    return gReal->common.methods->open(&gReal->common, id, device);
}

struct hw_module_methods_t gMethods = {
        .open = Open,
};

}  // namespace

extern "C" {

__attribute__((visibility("default"))) camera_module_t HAL_MODULE_INFO_SYM = {
        .common =
                {
                        .tag = HARDWARE_MODULE_TAG,
                        .module_api_version = CAMERA_MODULE_API_VERSION_2_5,
                        .hal_api_version = HARDWARE_HAL_API_VERSION,
                        .id = CAMERA_HARDWARE_MODULE_ID,
                        .name = "sm6375 camera HAL wrapper",
                        .author = "The LineageOS Project",
                        .methods = &gMethods,
                        .dso = nullptr,
                        .reserved = {},
                },
        .get_number_of_cameras = GetNumberOfCameras,
        .get_camera_info = GetCameraInfo,
        .set_callbacks = SetCallbacks,
        .get_vendor_tag_ops = GetVendorTagOps,
        .open_legacy = OpenLegacy,
        .set_torch_mode = SetTorchMode,
        .init = Init,
        .get_physical_camera_info = GetPhysicalCameraInfo,
        .is_stream_combination_supported = IsStreamCombinationSupported,
        .notify_device_state_change = NotifyDeviceStateChange,
        .reserved = {},
};

}  // extern "C"

namespace {

// hw_get_module() runs this before it reads HMI, so the provider sees the
// real module's API version and only the entry points the real module has.
__attribute__((constructor)) void LoadRealHal() {
    void* handle = dlopen(kRealHal, RTLD_NOW);
    if (!handle) {
        ALOGE("dlopen %s: %s", kRealHal, dlerror());
        return;
    }
    gReal = static_cast<camera_module_t*>(dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
    if (!gReal) {
        ALOGE("%s has no %s", kRealHal, HAL_MODULE_INFO_SYM_AS_STR);
        dlclose(handle);
        return;
    }

    camera_module_t& hmi = HAL_MODULE_INFO_SYM;
    hmi.common.module_api_version = gReal->common.module_api_version;
    hmi.common.hal_api_version = gReal->common.hal_api_version;
    if (!gReal->get_vendor_tag_ops) hmi.get_vendor_tag_ops = nullptr;
    if (!gReal->open_legacy) hmi.open_legacy = nullptr;
    if (!gReal->set_torch_mode) hmi.set_torch_mode = nullptr;
    if (!gReal->init) hmi.init = nullptr;
    if (!gReal->get_physical_camera_info) hmi.get_physical_camera_info = nullptr;
    if (!gReal->is_stream_combination_supported) hmi.is_stream_combination_supported = nullptr;
    if (!gReal->notify_device_state_change) hmi.notify_device_state_change = nullptr;
}

}  // namespace
