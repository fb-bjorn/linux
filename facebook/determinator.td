load("@builtins//:test_utils.td.bzl", "true")
load("@builtins//:schedule_type.td.bzl", "schedule_type_is")
load("@builtins//:skycastle.td.bzl", "skycastle")
load("//bzl:flavors.td.bzl", "ARCH_FLAVORS", "ARCHITECTURES", "BUILD_ONLY_FLAVORS", "ARCH_X86_64")

TD_FLAVORS = {
    "x86_64": ["debug", "hardened", "kdump", "lol2"],
    "aarch64": ["debug", "kdump"],
}


SKYCASTLE_WORKFLOW = "//tools/skycastle/workflows2/kernel/build/kernel-build.sky"
SKYCASTLE_WORKFLOW_FLAGS = "//tools/skycastle/workflows2/kernel/build/flags.sky"
ONCALL = "kernel_infra"
GLOBAL_PREFIX="schedule-build"
SLP_PROVIDER_ID = "257456667334183"  # kernel_root
BPF_SELFTESTS_ARCH = "x86_64" # for now the only suported arch for bpf selftests

# Suites to run on the combined selftests kernel, and the one architecture they
# have been validated on: 15 runs across v7.1-fb, v6.16-fb and v6.13-fb, all
# x86_64. Other arches keep the previous behaviour of building a selftests
# kernel and running nothing, rather than being switched on untested.
#
# mm is deliberately listed on this branch only, unlike cgroup and cachestat
# which are shared with build_machinery/master. It needs numactl-devel at build
# time (ksm_tests, migration and rmap link -lnuma), which only the appliance
# v7.1-fb pins carries, so on older branches it does not compile and is dropped
# from the bundle entirely. Its runner gates on the kernel version and exits
# green below 7.1, so adding it to build_machinery/master would be safe but
# would boot a 16 GiB VM on every older branch's diffs only to skip.
SELFTESTS_ARCH = "x86_64"
SELFTEST_SUITES = ["cgroup", "cachestat", "mm"]

# Values of the deprecated `selftests` flag that mean "not set". Mirrors
# NO_SELFTESTS in the workflow's flags.sky, which cannot be loaded from here.
NO_SELFTESTS = ["none", "", False, None]

# This is a copy-paste from https://www.internalfb.com/code/fbsource/tools/utd/migrated_nbtd_jobs/extensions/skycastle.td.bzl
# I don't have a good understanding of what's going on here, but I assume we can use these values as a safe defaults
def _gen_slp_integration_spec(user_input):
    slp_integration_spec = {
        "event_environment": "PROD",
        "integration_mode": "AUTOMATIC_OBSERVER",
        "provider_id": SLP_PROVIDER_ID,
        "validation_session_id": user_input.values.get("slp_validation_session_id")
    }
    return str({k: v for k, v in slp_integration_spec.items() if v})

def _skycastle_impl(
    run_context,
    user_input,
    alias,
    arch,
    build_only = False,
    selftests = "none",
    selftest_suites = None,
    flavor = None,
    continuous = False,
    affected_files = None,
):
    args = {
        "arch": arch,
        "build-only": build_only,
        "selftests": selftests,
        "notify": False,
        "authenticated-build": continuous,
    }

    # The two selftest flags are mutually exclusive. The workflow enforces that
    # too, but only once the build is running -- catching it here fails the
    # determinator instead, before a workflow is created.
    if selftest_suites:
        if selftests not in NO_SELFTESTS:
            fail(
                "Pass selftest_suites or the deprecated selftests flag, not both (got {} and {}) for {}".format(
                    repr(selftests),
                    repr(selftest_suites),
                    alias,
                )
            )
        args["selftest_suites"] = selftest_suites

    if flavor:
        args["flavor"] = flavor

    if flavor in BUILD_ONLY_FLAVORS:
        args["skip-kexec-sanity"] = True
        args["skip-coldstart-sanity"] = True

    flags = {
        "{}:{}".format(SKYCASTLE_WORKFLOW_FLAGS, arg): value
        for arg, value in args.items()
    }

    flags[
        "//tools/skycastle/lib2/sandcastle/flags.sky:commit_hash"
    ] = user_input.values["vcs_revision"]

    tags = ["kernel", "linux-kernel", "utd"]
    sl_workloads = user_input.values.get("servicelab_workloads", None)
    if user_input.values.get("run_servicelab", False):
        entry_point = "servicelab"
        flags["%s:servicelab_workloads" % SKYCASTLE_WORKFLOW_FLAGS] = sl_workloads
        tags += ["continuous"]
    # We only run build + vmtest on continuous runs
    elif continuous:
        entry_point = "vmtest"
        tags += ["continuous"]
        flags[
            "//tools/skycastle/workflows2/kernel/lib/vmtest.sky:is_trusted_run"
        ] = True
    else:
        entry_point = "main"
        tags += ["diff"]

    return skycastle.schedule(
        alias = alias,
        workflow=SKYCASTLE_WORKFLOW,
        entry_point=entry_point,
        oncall=ONCALL,
        repo_name="linux-kernel",
        flags = flags,
        tags = tags,
        skycastle_keyword_options = {
            "slp-integration-spec": _gen_slp_integration_spec(user_input),
        },
    )

[
    [
        td_rule(
            name = "{}-{}".format(GLOBAL_PREFIX, arch),
            predicate = schedule_type_is("diff"),
            output = derive(
                impl = _skycastle_impl,
                args = {
                    "alias": "{}-{}".format(GLOBAL_PREFIX, arch),
                    "run_context": source.run_context(),
                    "user_input": source.user_input(),
                    "arch": arch,
                    "affected_files": source.affected_files(),
                },
            )
        ),
        td_rule(
            name = "{}-{}-selftests".format(GLOBAL_PREFIX, arch),
            predicate = schedule_type_is("diff"),
            output = derive(
                impl = _skycastle_impl,
                args = {
                    "alias": "{}-{}-selftests".format(GLOBAL_PREFIX, arch),
                    "run_context": source.run_context(),
                    "user_input": source.user_input(),
                    "arch": arch,
                    "selftests": "none" if arch == SELFTESTS_ARCH else "all",
                    "selftest_suites": SELFTEST_SUITES if arch == SELFTESTS_ARCH else None,
                    "affected_files": source.affected_files(),
                },
            )
        ),
    ] + [
        td_rule(
            name = "{}-{}-{}".format(GLOBAL_PREFIX, arch, flavor),
            predicate = schedule_type_is("diff"),
            output = derive(
                impl = _skycastle_impl,
                args = {
                    "alias": "{}-{}-{}".format(GLOBAL_PREFIX, arch, flavor),
                    "run_context": source.run_context(),
                    "user_input": source.user_input(),
                    "arch": arch,
                    "flavor": flavor,
                    "build_only": False if flavor in ['debug'] else True,
                    "affected_files": source.affected_files(),
                },
            )
        )
        for flavor in TD_FLAVORS.get(arch, [])
    ]
    for arch in ARCHITECTURES
] + [
    td_rule(
        name = "{}-{}-bpf-selftests".format(GLOBAL_PREFIX, BPF_SELFTESTS_ARCH),
        predicate = schedule_type_is("diff"),
        output = derive(
            impl = _skycastle_impl,
            args = {
                "alias": "{}-{}-bpf-selftests".format(GLOBAL_PREFIX, BPF_SELFTESTS_ARCH),
                "run_context": source.run_context(),
                "user_input": source.user_input(),
                "arch": BPF_SELFTESTS_ARCH,
                "selftests": "bpf",
                "affected_files": source.affected_files(),
            },
        )
    ),
] if BPF_SELFTESTS_ARCH in ARCHITECTURES else []

# For continuous runs we run default x86_64 build
def _suffix(flavor):
  if flavor:
    return "-{}".format(flavor)
  return ""

[
  td_rule(
      name = "{}-continuous-{}{}".format(GLOBAL_PREFIX, ARCH_X86_64, _suffix(flavor)),
      predicate = schedule_type_is("continuous"),
      output = derive(
          impl = _skycastle_impl,
          args = {
              "alias": "{}-continuous-{}{}".format(GLOBAL_PREFIX, ARCH_X86_64, _suffix(flavor)),
              "run_context": source.run_context(),
              "user_input": source.user_input(),
              "arch": ARCH_X86_64,
              "continuous": True,
              "flavor": flavor,
          },
      )
  )
  for flavor in [None, "debug"]
]
