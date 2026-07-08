BUILD_IMAGE_X86_64_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/build_images/update-libbpf1.6.c9.llvm21.x86_64"
BUILD_IMAGE_X86_64_SHA256="53348fa4c06084cda3dc2274ed1a167effd09bbd060179ac93df8e896969e1a8"

BUILD_IMAGE_AARCH64_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/build_images/update-libbpf1.6.c9.llvm21.aarch64"
BUILD_IMAGE_AARCH64_SHA256="091fd520adcd47fba8cd161ce2598ed84abe775e9a216e29e3c0ff8e52cf9dbe"

KPATCH_BUILD_RPM_URI="http://yum/yum/centos/9/backports/x86_64/RPMS/kpatch-build-0.9.11-0.4.hs.el9.x86_64.rpm"
KPATCH_BUILD_RPM_SHA256="054acc1dc50ff3c0943339642b52d0cb10c7c060ff459cd913d1fde94dc6acff"

CLANG_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v71/profdata.7.1.0-0_fbk0_clangtrain_rc8_0_gb58234f1b727.7200s.yhs.2026-07-08-11-34-58"
CLANG_TRAIN_DATA_SHA256="8316fad9036c1564a80891b6ea1485a2ae49b76620860e610c1f9a82e5efb506"

HARDENED_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v71/profdata.7.1.0-0_fbk0_clangtrain_rc8_0_gb58234f1b727.7200s.yhs.2026-07-08-11-34-58"
HARDENED_TRAIN_DATA_SHA256="8316fad9036c1564a80891b6ea1485a2ae49b76620860e610c1f9a82e5efb506"

AARCH64_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v613/aarch64/vmlinux.profdata.aarch64.1050"
AARCH64_TRAIN_DATA_SHA256="cfad06ec875e343cbee20dacd121d460de50ae455c39c5f3a6050a6ecae57851"

SelftestsType = enum("none", "all", "bpf")

SELFTESTS = [
    # Currently bpf selftests do not build in a way which is meaningful, plus
    # they rely on enabling `CONFIG_PROVE_LOCKING`, which in turn clashes with
    # proprietary modules: https://fburl.com/diff/vdprmlzf
    # "bpf",
    "cachestat",
    "capabilities",
    "cgroup",
    "clone3",
    "core",
    "drivers",
    "exec",
    "filesystems",
    "firmware",
    "fpu",
    "ftrace",
    "futex",
    "hid",
    "iommu",
    "ipc",
    "kcmp",
    "kexec",
    # NOTE: kmod sets CONFIG_BTRFS_FS=m, which prevents us from booting VMs.
    # "kmod",
    "lib",
    "livepatch",
    "locking",
    "membarrier",
    "memfd",
    "mincore",
    "mm",
    "mount",
    "mount_setattr",
    "mqueue",
    "net",
    "openat2",
    "perf_events",
    "pidfd",
    "pid_namespace",
    "prctl",
    "proc",
    "ptp",
    "ptrace",
    # we should run rcutorture, but probably in its own test?
    "rlimits",
    "rseq",
    "sched",
    "seccomp",
    "sgx",
    # 6.13 renamed "sigaltstack" to "signal"
    "signal",
    "size",
    "splice",
    "static_keys",
    "sync",
    "sysctl",
    "timers",
    "tmpfs",
    "uevent",
    # 6.12 removed "user" selftest
    # "user",
    "x86",
]
