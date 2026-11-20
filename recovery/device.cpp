// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <fs_mgr/roots.h>
#include <private/android_filesystem_config.h>
#include <recovery_ui/device.h>
#include <recovery_ui/screen_ui.h>
#include <selinux/android.h>

namespace {

bool EnsureOtaDirectory(const char* path) {
    if (mkdir(path, 0750) == 0) {
        if (chown(path, AID_ROOT, AID_SYSTEM) == 0) {
            return true;
        }
        PLOG(ERROR) << "Failed to set ownership of " << path;
        return false;
    }
    if (errno == EEXIST) {
        struct stat info;
        if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            return true;
        }
        LOG(ERROR) << path << " is not an accessible directory";
        return false;
    }
    PLOG(ERROR) << "Failed to create " << path;
    return false;
}

bool PrepareOtaMetadata() {
    android::fs_mgr::Fstab fstab;
    if (!android::fs_mgr::ReadDefaultFstab(&fstab) ||
        !android::fs_mgr::EnsurePathMounted(&fstab, "/metadata")) {
        LOG(ERROR) << "Failed to mount /metadata for OTA state";
        return false;
    }
    // A fresh ext4 filesystem has no label on its root inode yet.
    if (selinux_android_restorecon("/metadata", 0) != 0) {
        PLOG(ERROR) << "Failed to label /metadata";
        return false;
    }
    return EnsureOtaDirectory("/metadata/ota") && EnsureOtaDirectory("/metadata/ota/snapshots");
}

class MotoRecoveryDevice : public Device {
  public:
    MotoRecoveryDevice() : Device(new ScreenRecoveryUI) {}

    void StartRecovery() override { PrepareOtaMetadata(); }
    bool PostWipeData() override { return PrepareOtaMetadata(); }
};

}  // namespace

Device* make_device() {
    return new MotoRecoveryDevice;
}
