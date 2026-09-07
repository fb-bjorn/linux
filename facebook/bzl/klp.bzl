# vim: set expandtab tabstop=4 shiftwidth=4 softtabstop=4 :
load(":flavors.td.bzl", "ARCH_X86_64", "ARCH_AARCH64", "ARCHITECTURE_TO_KERNEL_ARCH", "ARCHITECTURE_TO_CONFIG_ARCH", "ARCHITECTURE_TO_RPMBUILD_TARGET")
load(":container.bzl", "container_genrule", "default_container_image")
load(":kernel.bzl", "buildinfo")
load(":constants.bzl", "CLANG_TRAIN_DATA_URI", "CLANG_TRAIN_DATA_SHA256")
load(":constants.bzl", "AARCH64_TRAIN_DATA_URI", "AARCH64_TRAIN_DATA_SHA256")
load(":bootconfig.bzl", "bootconfig_name")

def klp():
    """
    Generate targets for creating kernel live patches. Callers may specify the
    following configuration parameters:
      - patch_to: A tag that we will be patching to. If this option is not
        specified, the current commit (HEAD) will be assumed. The tag being
        patched to must be suffixed with -hotfix<num>.
      - patch_from: A tag that we will be patching from. If this option is not
        specified, the from tag will be the tag used in patch_to, without the
        -hotfix<num> suffix.
      - flavor: The flavor for the kernel. This is the same flavor that may be
        specified in any other kernel build type.
      - label: A label to apply to the build.
      - sign: Whether the resulting klp module should be signed. Must be set to
        'true' for the module to be signed.
      - sign_key: The key to use when signing the KLP module.
    """
    # Parameters passed to BUCK.
    patch_to = native.read_config("klp", "patch_to", None)
    patch_from = native.read_config("klp", "patch_from", None)
    flavor=native.read_config("klp", "flavor", None)
    label=native.read_config("klp", "label", None)
    sign = native.read_config('kernel', 'sign_mod', 'false')
    sign_key = native.read_config('kernel', 'sign_mod_key', 'hsm-test-key')
    # If no `klp.arch` passed, uses x86
    arch = native.read_config("klp", "arch", ARCH_X86_64)
    if arch != ARCH_X86_64 and arch != ARCH_AARCH64:
      fail("Only {} and {} is supported for klp. You have {}".format(ARCH_X86_64, ARCH_AARCH64, arch))

    # Artifact-driven baseline (used by the in-build KLP canary): build a KLP
    # against a specific kernel commit + its already-built RPMs instead of a
    # published release tag. When these are unset, the classic tag + `kerctl
    # download` path below is used unchanged.
    #   baseline_everstore: @everstore:<handle> of the built kernel bundle; when
    #                       set, baseline kernel RPMs come from it instead of
    #                       `kerctl download <version>` (which needs a published
    #                       kernel).
    #   baseline_version:   short kernel version string for RPM/module naming;
    #                       when set, replaces the from_tag-derived version.
    #   hotfix_name:        hotfix label for naming; when set, replaces the
    #                       `-hotfix<num>` extracted from to_tag (a raw commit
    #                       has none).
    baseline_everstore = native.read_config("klp", "baseline_everstore", None)
    baseline_version = native.read_config("klp", "baseline_version", None)
    hotfix_name = native.read_config("klp", "hotfix", None)

    # Generate targets for the commit being patched to. The commit *must* have a
    # tag that is suffixed with -hotfix<num>, regardless of whether the patch_to
    # option is specified by the user when invoking BUCK.
    to_cmd = """
      HOME=/dev/null git show-ref --tags -d
      | grep hotfix
      | grep `HOME=/dev/null git rev-parse HEAD`
      | awk -F '[ /]' '{print $NF}'
      > $OUT"""
    if patch_to:
      to_cmd = "echo {} > $OUT".format(patch_to)

    notempty_cmd = " && [ -s $OUT ] || exit 1"

    native.genrule(
        name="to_tag",
        cmd=to_cmd + notempty_cmd,
        out="to_tag",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    # Extract the hotfix<num> portion of the tag. This will be stripped when
    # generating the target for the from_tag. When patching against a raw commit
    # (no `-hotfix<num>` tag), the caller passes `klp.hotfix` explicitly.
    hotfix_cmd = "cat $(location :to_tag) | sed -e 's|.*\\(hotfix[0-9]*\\).*|\\1|g' > $OUT"
    if hotfix_name:
      hotfix_cmd = "echo {} > $OUT".format(hotfix_name)
    native.genrule(
        name="hotfix",
        cmd=hotfix_cmd + notempty_cmd,
        out="hotfix",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    # Extract the baseline from_tag from which we're creating the patch. The
    # from_tag (unless specified by the user as a config option) is assumed to
    # be the exact same name as the tag we're patching to, without the "-hotfix"
    # suffix.
    from_cmd = "cat $(location :to_tag) | sed -e 's|-hotfix[0-9]*$||' > $OUT"
    if patch_from:
      from_cmd = "echo {} > $OUT".format(patch_from)
    native.genrule(
        name="from_tag",
        cmd=from_cmd + notempty_cmd,
        out="from_tag",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    # form patch
    native.genrule(
        name="patches",
        cmd="""mkdir -p $OUT
            HOME=/dev/null git format-patch -k `cat $(location :from_tag)`..`cat $(location :to_tag)` -o $OUT/
            # Drop patches klp-build cannot use:
            #   - hunkless ones (empty placeholder commits)
            #   - ones touching only build tooling.  facebook/ is read from the
            #     outer checkout and never compiled; scripts/livepatch and
            #     tools/objtool are already in /rw/compile via the :tooling
            #     overlay, so re-applying them fails as already applied.
            #     klp-build also refreshes each patch against a pristine tree,
            #     so a chain of commits to one file could not apply regardless.
            overlaid=' b/(facebook|scripts/livepatch|tools/objtool)/'
            for p in $OUT/*.patch; do
                grep -q '^diff --git' $p || { rm -f $p; continue; }
                all=`grep -c '^diff --git' $p`
                tool=`grep '^diff --git' $p | grep -cE "$overlaid" || true`
                [ "$tool" = 0 ] && continue
                [ "$tool" = "$all" ] && { rm -f $p; continue; }
                echo "ERROR: `basename $p` changes both build tooling and kernel" >&2
                echo "source; split it so the tooling half can come from the overlay." >&2
                exit 1
            done
            cp `HOME=/dev/null git rev-parse --show-toplevel`/facebook/9999-Dummy-patch-to-bump-hotfix-version.patch.template $OUT/9999-Dummy-patch-to-bump-hotfix-version.patch
        """,
        out="patches",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    # for download, we need to take in consideration the architecture.
    kerctl_download = "kerctl download --arch {} ".format(arch)

    # Baseline kernel RPMs: either copied out of a passed everstore bundle
    # (artifact mode) or `kerctl download`ed by version (classic path).
    if baseline_everstore:
        # Extract the already-built kernel bundle once; the devel/bin genrules
        # each pick their RPM out of it.
        native.genrule(
            name = "baseline-kernel-bundle",
            cmd = "mkdir -p $OUT && cp `kerctl -v everstore --extract '{handle}'`/*.rpm $OUT/".format(handle = baseline_everstore),
            out = "baseline-kernel-bundle",
            cacheable = False,
            labels = ["linux_kernel"],
        )
        devel_cmd = "mkdir -p $OUT && cp $(location :baseline-kernel-bundle)/kernel-devel-*.rpm $OUT/kernel-devel.rpm"
        bin_cmd = "mkdir -p $OUT && cp $(location :baseline-kernel-bundle)/kernel-[0-9]*.rpm $OUT/kernel-bin.rpm"
    else:
        devel_cmd = "mkdir -p $OUT && " + kerctl_download + "--devel --out-dir $OUT `cat $(location :baseline-rpm-version)` && mv $OUT/*.rpm $OUT/kernel-devel.rpm"
        bin_cmd = "mkdir -p $OUT && " + kerctl_download + "--kernel --out-dir $OUT `cat $(location :baseline-rpm-version)` && mv $OUT/*.rpm $OUT/kernel-bin.rpm"

    # download published rpms for baseline (or copy from the everstore bundle)
    native.genrule(
        name="kernel-devel-klp",
        cmd=devel_cmd,
        out="kernel-devel-klp",
        cacheable=False,
        labels = ["linux_kernel"],
    )
    native.genrule(
        name="kernel-bin-klp",
        cmd=bin_cmd,
        out="kernel-bin-klp",
        cacheable=False,
        labels = ["linux_kernel"],
    )
    bootconfig_target = "//facebook/config:" + bootconfig_name(
        arch = arch,
        flavor = flavor,
    )
    bind_ros = [
        ("$(location :kernel-devel-klp)", "/tmp/kernel-devel"),
        ("$(location :kernel-bin-klp)", "/tmp/kernel-bin"),
        ("$(location :patches)", "/tmp/patches"),
        ("$(location :to_tag)", "/tmp/to_tag"),
        ("$(location :config)", "/tmp/config"),
        ("$(location :uname-klp)", "/tmp/uname"),
        ("$(location :hotfix)", "/tmp/hotfix"),
        ("$(location :baseline-rpm-version)", "/tmp/baseline_rpm_version"),
        (bootconfig_target, "/tmp/bootconfig"),
    ]

    #checkout baseline
    # Clone then `checkout` (instead of `clone -b`) so from_tag/to_tag may be a
    # raw commit SHA, not just a branch/tag ref. A local clone copies the full
    # object store, so a commit that isn't on any branch (e.g. the synthesized
    # dummy-KLP commit) is still checkout-able by SHA.
    native.genrule(
        name = "baseline-sources",
        cmd = """sudo rm -rf $OUT; mkdir -p $OUT
            HOME=/dev/null git clone -n `HOME=/dev/null git rev-parse --show-toplevel` $OUT
            HOME=/dev/null git -C $OUT checkout `cat $(location :from_tag)`
            pushd $OUT
            make mrproper
            popd
        """,
        cacheable=False,
        out="baseline-sources",
        labels = ["linux_kernel"],
    )

    #build the rpm version
    #more details on versioning available in buildinfo()
    #i don't have better idea on how to map between git tag/branch and rpm
    #v5.6-fbk13-rc1 -> 5.6.13-0_fbk13_rc1
    flavor_ver = "_%s" % flavor if flavor else ""
    label_ver = "_%s" % label if label else ""

    # In artifact mode the caller passes the version directly (the base is a raw
    # commit, so the from_tag fbk/rc parse below doesn't apply).
    if baseline_version:
        baseline_rpm_version_cmd = "echo '{}' > $OUT".format(baseline_version)
    else:
        baseline_rpm_version_cmd = """
            pushd $(location :baseline-sources)
            majorver=`make -s kernelversion EXTRAVERSION=`
            popd
            rpm_n=0
            fbkv=`cat $(location :from_tag) | sed -e 's|.*-\\(fbk[0-9]*\\).*|\\1|g'`
            rc=`cat $(location :from_tag) | grep -oE '\\b-rc[0-9]+\\b$' | tr '-' '_'`
            flavor="%s"
            label="%s"
            echo "${majorver}-${rpm_n}_${fbkv}${flavor}${rc}${label}" > $OUT
        """ % (flavor_ver, label_ver)
    native.genrule(
        name = "baseline-rpm-version",
        cmd = baseline_rpm_version_cmd,
        out = "baseline-rpm-version",
        labels = ["linux_kernel"],
    )

    cfg_flavor = flavor

    # when dealing with config, uses ARCHITECTURE_TO_CONFIG_ARCH
    # i.e, arm64 is called arm64 not aarch64.
    _arch = ARCHITECTURE_TO_CONFIG_ARCH[arch]
    if not flavor:
        cfg_flavor = _arch

    if not cfg_flavor.startswith(_arch):
        # the config names aarch64 as arm64 instead of aarch64
        cfg_flavor = "%s-%s" % (_arch, cfg_flavor)

    native.genrule(
      name = "config",
      cmd =  "cp $(location //facebook/config:%s) $OUT" % cfg_flavor,
      out = "config",
      labels = ["linux_kernel"],
    )

    #uname of original kernel
    native.genrule(
        name = "uname-klp",
        cmd = "rpm -qp --queryformat '%{version}-%{release}' $(location :kernel-devel-klp)/kernel-devel.rpm > $OUT",
        out = "uname",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    train_data_args = ""
    compiler_args = "LLVM=1 ARCH={}".format(ARCHITECTURE_TO_KERNEL_ARCH[arch])

    if "lol" not in cfg_flavor:
        if arch == ARCH_X86_64:
            native.http_file(
                name = "target-train-data",
                urls = [CLANG_TRAIN_DATA_URI],
                sha256 = CLANG_TRAIN_DATA_SHA256,
            )
        else:
            native.http_file(
                name = "target-train-data",
                urls = [AARCH64_TRAIN_DATA_URI],
                sha256 = AARCH64_TRAIN_DATA_SHA256,
            )

        bind_ros.append(("$(location :target-train-data)", "/tmp/vmlinux.profdata"))
        train_data_args = "CFLAGS_PGO_CLANG=-fprofile-use=/tmp/vmlinux.profdata"


    # klp-build ships in the kernel tree, so /rw/compile would otherwise run
    # the baseline's copy and a tooling fix could not ride along in a hotfix.
    # Overlay it from to_tag so both builds use the same version.
    native.genrule(
      name = "tooling",
      cmd = """mkdir -p $OUT
          HOME=/dev/null git -C `HOME=/dev/null git rev-parse --show-toplevel` \
              archive `cat $(location :to_tag)` scripts/livepatch tools/objtool \
              > $OUT/tooling.tar
      """,
      out = "tooling",
      cacheable = False,
      labels = ["linux_kernel"],
    )
    bind_ros.append(("$(location :tooling)", "/tmp/tooling"))

    #feed artifacts to klp-build in a container
    bind_rws = [(":baseline-sources", "/rw/compile"), ("$OUT", "/rw/output")]
    container_genrule(
        name="klp-build",
        arch=arch,
        cmd="""
            rpm -ivh /tmp/kernel-bin/*.rpm /tmp/kernel-devel/*.rpm --ignorearch
            pushd /rw/compile
            cp /tmp/config .config
            cp /tmp/bootconfig .bootconfig
            tar -xf /tmp/tooling/tooling.tar

            # klp-build drives plain make, so these must be in the environment.
            export {compiler_args}
            make olddefconfig

            # A missing profile silently changes inlining, symbol names and
            # sympos.  kpatch-build refused to build without one.
            if grep -q "^CONFIG_PGO_CLANG=y" .config && [ ! -f /tmp/vmlinux.profdata ]; then
                echo "ERROR: CONFIG_PGO_CLANG=y but no profile data supplied." >&2
                exit 1
            fi

            # EXTRAVERSION must match the deployed uname or vermagic rejects the
            # module.  The Makefile assigns CFLAGS_PGO_CLANG with := so the
            # environment loses; assignments in MAKEFLAGS count as command line
            # and reach the recursive sub-makes.
            extraver=-`cat /tmp/uname | cut -d '-' -f 2`
            export MAKEFLAGS="EXTRAVERSION=$extraver {train_data_args}"

            export PATH=$PATH:/usr/libexec/git-core/
            name=klp_`cat /tmp/baseline_rpm_version`_`cat /tmp/hotfix`
            ./scripts/livepatch/klp-build -j `nproc` -o /rw/output/$name.ko \
                /tmp/patches/* 2>&1 | tee /tmp/klp.out
            rc=$PIPESTATUS
            popd

            # Keep the log on success too: it records which objects changed.
            cp /tmp/klp.out /rw/output/ 2>/dev/null || true

            if [ $rc -ne 0 ]; then
                exit $rc
            fi

            # Guard against a silent partial failure shipping a non-module .ko.
            if ! ls /rw/output/*.ko >/dev/null 2>&1 || ! readelf -h /rw/output/*.ko >/dev/null 2>&1; then
                echo "ERROR: klp-build reported success but produced no valid ELF .ko module." >&2
                exit 1
            fi
        """.format(train_data_args = train_data_args, compiler_args = compiler_args),
        bind_ro=bind_ros,
        bind_rw=bind_rws,
        cacheable=False,
        image_override="$(location {})".format(default_container_image(arch)),
    )
    # prepare packaging
    bind_ros.append(("$(location :klp-spec)", "/tmp/klp.spec"))
    bind_ros.append(("$(location :klp)", "/tmp/module"))
    native.genrule(
        name = "klp-spec",
        cmd = """
            SPECOUT=`pwd`/$OUT
            pushd `HOME=/dev/null git rev-parse --show-toplevel`
            cp -a facebook/build/klp.spec $SPECOUT
            popd
        """,
        out = "klp.spec",
        cacheable=False,
        labels = ["linux_kernel"],
    )
    if sign == "true":
        native.genrule(
            name = "klp",
            cmd = """
            chmod +x $(location //facebook:sign-kernel-build-sh)
            $(location //facebook:sign-kernel-build-sh) \
                $(location :klp-build) \
                {sign_key} \
                $(location //facebook:sign-sandcastle-spec) \
                $OUT
            """.format(sign_key = sign_key),
            out = "klp.ko",
            cacheable = False,
            labels = ["linux_kernel"],
        )
    else:
        native.genrule(
            name = "klp",
            cmd = """
            cp -a $(location :klp-build)/*.ko $OUT
            """,
            out = "klp.ko",
            cacheable = False,
            labels = ["linux_kernel"],
        )


    container_genrule(
        name="klp-rpm",
        arch=arch,
        cmd="""
            cat /tmp/to_tag
            rpmbuild --target={target} -ba /tmp/klp.spec --define "short_kernel_version `cat /tmp/baseline_rpm_version`" --define "rpm_kernel_version `cat /tmp/uname`" --define "module_path /tmp/module" --define "hf_name `cat /tmp/hotfix`"
            cp -vR /root/rpmbuild/RPMS/*/*.rpm /rw/output/
        """.format(target=ARCHITECTURE_TO_RPMBUILD_TARGET[arch]),
        bind_ro=bind_ros,
        bind_rw=bind_rws,
        cacheable=False,
        image_override="$(location {})".format(default_container_image(arch)),
    )
