#
# Flavours and architectures combinations should be added to this file.
#
# The config will be generated using facebook/scripts/prepareconfig.
#
# Architectures that can be built and have configs generated for them are
# listed here.  Additionally, mapping between the official architecture
# name and the Linux names (as referenced in our configs _and_ in the
# kernel ARCH variable) as expressed here.
#
# We also need arch->rpm target mappings so that we produce sensible RPMs.
#
ARCH_X86_64 = "x86_64"
ARCH_AARCH64 = "aarch64"

#
# Names as used in buck and target determinator.
#
ARCHITECTURES = [
    ARCH_X86_64,
    ARCH_AARCH64,
]

#
# Names used for config generation.
#
ARCHITECTURE_TO_CONFIG_ARCH = {
    ARCH_X86_64: "x86_64",
    ARCH_AARCH64: "arm64",
}

#
# Names used for cross-compilation.
#
ARCHITECTURE_TO_KERNEL_ARCH = {
    ARCH_X86_64: "x86",
    ARCH_AARCH64: "arm64",
}

#
# Targets passed to rpmbuild.
#
ARCHITECTURE_TO_RPMBUILD_TARGET = {
    ARCH_X86_64: "{}-unknown-linux".format(ARCH_X86_64),
    ARCH_AARCH64: "{}-unknown-linux".format(ARCH_AARCH64),
}

#
# Flavours are applied on a per-architecture basis using this map.
#
ARCH_FLAVORS = {
    ARCH_X86_64: [
        "asicedk",
        "clangtrain",
        "crackerjackhost",
        "cvmhost",
        "debug",
        "debugnightly",
        "hardened",
        "hardenedtrain",
        "kdump",
        "lol2",
        "npi",
        "vm2",
    ],
    ARCH_AARCH64: [
        "crackerjackhost",
        "cvmhost",
        "debug",
        "debugnightly",
        "dpu",
        "kdump",
    ],
}

#
# Flavors we only want to build and not extensively test
#
BUILD_ONLY_FLAVORS = [
    "asicedk",
    "clangtrain",
    "crackerjackhost",
    "cvhost",
    "hardenedtrain",
    "kdump",
    "lol2",
    "npi",
    "vm2",
]
