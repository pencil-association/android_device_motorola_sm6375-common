#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import os

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'hardware/qcom-caf/sm6375-6.1',
    'device/motorola/sm6375-common',
    'hardware/qcom-caf/wlan',
    'hardware/motorola',
    'vendor/qcom/opensource/commonsys/display',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/qcom/opensource/dataservices',
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'vendor.qti.diaghal@1.0',
        'vendor.qti.hardware.wifidisplaysession@1.0',
        'vendor.qti.ImsRtpService-V1-ndk',
        'vendor.qti.imsrtpservice@3.0',
        'vendor.qti.imsrtpservice@3.1',
    ): lib_fixup_vendor_suffix,
}


blob_fixups: blob_fixups_user_type = {
    # Keep the generated vendor incremental version instead of rewriting it.
    'vendor/bin/init.oem.hw.sh': blob_fixup()
        .regex_replace(r'(?ms)^set_ro_vendor_incremental\(\)\n\{\n.*?^\}\n\n', '')
        .regex_replace(r'(?m)^set_ro_vendor_incremental &\n\n', ''),
    # The stock sensor HAL keeps the stock transport and protobuf libraries;
    # the malmo sensor services use their own libssc and libsnsapi.
    'vendor/lib64/sensors.ssc.so': blob_fixup()
        .replace_needed('libsnsapi.so', 'libsnsapi-stock.so')
        .replace_needed('libssc.so', 'libssc-stock.so')
        .add_needed('libcompiler_rt.so'),
    'vendor/lib64/libssc-stock.so': blob_fixup()
        .fix_soname()
        .replace_needed('libsnsapi.so', 'libsnsapi-stock.so'),
    'vendor/lib64/libsnsapi-stock.so': blob_fixup()
        .fix_soname(),
    # SoftDev's unused installd cpuset scan requires forbidden cross-process access.
    'vendor/bin/thermal-engine-v2': blob_fixup()
        .sig_replace(
            '00 ff ff b0 00 d8 09 91 b3 39 00 94 88 2e 00 d0 00 61 05 b9 20 02 f8 37 a0 18 00 d0 00 20 29 91 a4 f4 ff 97 a0 03 00 34',
            '00 ff ff b0 00 d8 09 91 00 00 80 12 88 2e 00 d0 00 61 05 b9 20 02 f8 37 a0 18 00 d0 00 20 29 91 a4 f4 ff 97 a0 03 00 34'),
    # Open the client sockets to rild and the camera HAL, which are not in
    # oem_2907, matching the modes Motorola uses for vendor.thermal-com.
    'vendor/etc/init/init_thermal-engine-v2.rc': blob_fixup()
        .regex_replace(r'(socket thermal-(?:send|recv-passive)-client stream) 0660', r'\1 0666'),
    # The QHDC client blocks in AServiceManager_waitForService() for
    # vendor.qti.qhcp.IQHDC/default, which no sm6375 build provides. libbinder
    # retries every second for as long as the GNSS HAL runs, so the HAL,
    # servicemanager and init all wake once a second, screen off included.
    # Return "no service" instead; the caller already handles a null binder.
    'vendor/lib64/libizat_core.so': blob_fixup()
        .sig_replace(
            '89 46 40 f9 1f 01 00 72 a0 02 89 9a 56 29 00 94 e0 0f 00 f9 e8 83 00 91',
            '89 46 40 f9 1f 01 00 72 a0 02 89 9a e0 03 1f aa e0 0f 00 f9 e8 83 00 91'),
    'vendor/etc/media_profiles_blair.xml': blob_fixup()
        .regex_replace(r'\s+maxHFR(?:FrameWidth|FrameHeight|Mode)="[^"]*"', '')
        .regex_replace(r'\s*<(?:VideoEditorCap|ExportVideoProfile)\b[^>]*/>', ''),
    'system_ext/lib64/libwfdmmsrc_system.so': blob_fixup()
        .add_needed('libaudiobase.so')
        .add_needed('libgui_shim.so'),
    'system_ext/lib64/libwfdservice.so': blob_fixup()
        .replace_needed('android.media.audio.common.types-V4-cpp.so', 'android.media.audio.common.types-V5-cpp.so')
        .add_needed('libaudiobase.so'),
    'system_ext/lib64/libwfdnative.so': blob_fixup()
        .add_needed('libinput_shim.so'),
    ('vendor/lib64/libdpps.so', 'vendor/lib64/libsnapdragoncolor-manager.so'): blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),
    ('vendor/bin/qcc-vendor', 'vendor/bin/xtra-daemon', 'vendor/lib64/libqcc_sdk.so'): blob_fixup()
        .add_needed('libbinder_shim.so'),
    'vendor/etc/seccomp_policy/qcrilnr@2.0.policy': blob_fixup()
        .add_line_if_missing('getppid: 1')
        .add_line_if_missing('uname: 1'),
    # debuggerd's in-process dump needs these; without them any crash or
    # backtrace request (bugreport) kills the Codec2 HAL with SIGSYS.
    'vendor/etc/seccomp_policy/codec2.vendor.ext-arm64.policy': blob_fixup()
        .add_line_if_missing('getppid: 1')
        .add_line_if_missing('setsockopt: 1')
        .add_line_if_missing('uname: 1'),
    # Load the veux Codec2 store through libqcodec2_shim, which adds the
    # DMA-BUF heap usage parameter the ION-era store lacks.
    'vendor/bin/hw/vendor.qti.media.c2@1.0-service': blob_fixup()
        .binary_regex_replace(rb'libqcodec2_core\.so', b'libqcodec2_shim.so'),
    # Stats::status() turns the span between two buffer timestamps into a
    # bitrate for the periodic stats log. A bogus timestamp makes span << 10
    # overflow and the ubsan trap aborts media.hwcodec mid-playback. Take the
    # zero-span path (bitrate 0) instead of the trap.
    'vendor/lib64/libqcodec2_core.so': blob_fixup()
        .sig_replace(
            'c9 01 00 b4 ff db 49 eb c1 1b 00 54 80 42 40 fd',
            'c9 01 00 b4 ff db 49 eb 81 01 00 54 80 42 40 fd'),
    # The poll thread's frame rate estimate subtracts the previous output
    # timestamp under a ubsan trap, so an EOS or post-seek buffer with an
    # earlier timestamp aborts media.hwcodec. Clamp the delta to 0 instead,
    # which the estimate already treats as unknown.
    'vendor/lib64/libqcodec2_v4l2codec.so': blob_fixup()
        .sig_replace(
            '6c b9 40 f9 5f 01 00 71 ea 17 9f 1a cc 02 0c eb 43 8b 00 54',
            '6c b9 40 f9 5f 01 00 71 ea 17 9f 1a cc 02 0c eb ec 33 8c 9a')
        .sig_replace(
            '89 05 00 34 0a b9 40 f9 c9 02 0a eb a3 84 00 54',
            '89 05 00 34 0a b9 40 f9 c9 02 0a eb e9 33 89 9a'),
    # Advertise portrait sizes within the 1080p macroblock budget the legacy
    # driver enforces. Screen recording sizes itself from the decoder and
    # then encodes at that size, so both need it to keep the panel
    # resolution instead of scaling to 1088 lines.
    'vendor/etc/media_codecs_blair.xml': blob_fixup()
        .regex_replace(
            r'(<MediaCodec name="c2\.qti\.(?:avc|hevc)\.encoder" type="[^"]+">\n'
            r'(\s*)<Alias [^\n]*\n)'
            r'\s*<Limit name="size" min="128x128" max="1920x1088" />\n',
            r'\1\2<Limit name="size" min="128x128" max="1920x1920" />\n'
            r'\2<Limit name="block-count" range="64-8160" />\n')
        .regex_replace(
            r'(<MediaCodec name="c2\.qti\.(?:(?:avc|hevc)\.decoder(?:\.low_latency)?'
            r'|vp9\.decoder)" type="[^"]+" ?>\n'
            r'(\s*)<Alias [^\n]*\n)'
            r'\s*<Limit name="size" min="96x96" max="1920x1088" />\n',
            r'\1\2<Limit name="size" min="96x96" max="1920x1920" />\n'
            r'\2<Limit name="block-count" range="36-8160" />\n'),
    # Older libbase exported Trim(const std::string&); the multihal cannot
    # dlopen the Moto sub-HAL without it.
    'vendor/lib64/sensors.moto.so': blob_fixup()
        .add_needed('libbase_shim.so'),
    'vendor/lib64/libsensorndkbridge.so': blob_fixup()
        .replace_needed('android.hardware.sensors-V2-ndk.so', 'android.hardware.sensors-V3-ndk.so'),
    'system_ext/etc/permissions/moto-telephony.xml': blob_fixup()
        .regex_replace('/system/', '/system_ext/'),
}  # fmt: skip


module = ExtractUtilsModule(
    'sm6375-common',
    'motorola',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    add_generated_carriersettings=False,
    skip_main_proprietary_file=True,
)

source = os.environ.get('SM6375_BLOB_SOURCE', 'all')
if source not in ('all', 'stock', 'malmo'):
    raise ValueError('SM6375_BLOB_SOURCE must be all, stock, or malmo')
if source in ('all', 'stock'):
    module.add_proprietary_file('proprietary-files.txt')
if source in ('all', 'malmo'):
    module.add_proprietary_file('proprietary-files-malmo.txt')
# Pinned veux Codec2 for the legacy video driver; kept on every extraction.
module.add_proprietary_file('proprietary-files-veux.txt')

if __name__ == '__main__':
    ExtractUtils.device(module).run()
