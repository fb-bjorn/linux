BUILD_IMAGE_X86_64_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/build_images/D116030286.c9.llvm21.x86_64"
BUILD_IMAGE_X86_64_SHA256="1db787c2de31b649dbacdfe1c86f22f4d02d241228a34fe5eaec1ac2833440c5"

BUILD_IMAGE_AARCH64_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/build_images/D116030286.c9.llvm21.aarch64"
BUILD_IMAGE_AARCH64_SHA256="76fdb550b6dc086dae234f6add1210419f60bed6ae70f9a866a2bc2b15a48b2c"

KPATCH_BUILD_RPM_URI="https://yum/yum/centos/9/backports/x86_64/RPMS/kpatch-build-0.9.11-0.4.hs.el9.x86_64.rpm"
KPATCH_BUILD_RPM_SHA256="054acc1dc50ff3c0943339642b52d0cb10c7c060ff459cd913d1fde94dc6acff"

CLANG_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v71/profdata.7.1.0-0_fbk0_clangtrain_rc8_0_gb58234f1b727.7200s.yhs.2026-07-08-11-34-58"
CLANG_TRAIN_DATA_SHA256="8316fad9036c1564a80891b6ea1485a2ae49b76620860e610c1f9a82e5efb506"

HARDENED_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v71/profdata.7.1.0-0_fbk0_clangtrain_rc8_0_gb58234f1b727.7200s.yhs.2026-07-08-11-34-58"
HARDENED_TRAIN_DATA_SHA256="8316fad9036c1564a80891b6ea1485a2ae49b76620860e610c1f9a82e5efb506"

AARCH64_TRAIN_DATA_URI="https://interncache-all.fbcdn.net/manifold/linux_kernel/tree/clang_train_data/v71/aarch64/profdata.7.1.0-0_fbk0_rc9_pgoscrossley_20_g6780d0b13eae.7200s.scrossley.2026-07-21-02-24-29"
AARCH64_TRAIN_DATA_SHA256="778c752789d44cb629f0eedee2053afedf2ad01a1dd8f8c908495906599f0467"

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
