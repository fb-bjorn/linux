# vim: set expandtab tabstop=4 shiftwidth=4 softtabstop=4 :
load(":flavors.td.bzl", "ARCH_X86_64", "ARCH_AARCH64", "ARCHITECTURE_TO_KERNEL_ARCH", "ARCHITECTURE_TO_CONFIG_ARCH", "ARCHITECTURE_TO_RPMBUILD_TARGET")
load(":container.bzl", "container_genrule", "default_container_image")
load(":kernel.bzl", "buildinfo")
load(":constants.bzl", "CLANG_TRAIN_DATA_URI", "CLANG_TRAIN_DATA_SHA256", "KPATCH_BUILD_RPM_URI")
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
    # generating the target for the from_tag.
    native.genrule(
        name="hotfix",
        cmd="cat $(location :to_tag) | sed -e 's|.*\\(hotfix[0-9]*\\).*|\\1|g' > $OUT" + notempty_cmd,
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
            # Drop hunkless patches (e.g. empty placeholder commits like "kdump: new empty commit"); kpatch-build rejects them as unapplicable.
            for p in $OUT/*.patch; do grep -q '^diff --git' $p || rm -f $p; done
            export hotfix=`cat $(location :hotfix)`
            cat `HOME=/dev/null git rev-parse --show-toplevel`/facebook/9999-Dummy-patch-to-bump-hotfix-version.patch.template | envsubst > $OUT/9999-Dummy-patch-to-bump-hotfix-version.patch
        """,
        out="patches",
        cacheable=False,
        labels = ["linux_kernel"],
    )

    # for download, we need to take in consideration the architecture.
    kerctl_download = "kerctl download --arch {} ".format(arch)
    # download published rpms for baseline
    native.genrule(
        name="kernel-devel-klp",
        cmd="mkdir -p $OUT && " + kerctl_download + "--devel --out-dir $OUT `cat $(location :baseline-rpm-version)` && mv $OUT/*.rpm $OUT/kernel-devel.rpm",
        out="kernel-devel-klp",
        cacheable=False,
        labels = ["linux_kernel"],
    )
    native.genrule(
        name="kernel-bin-klp",
        cmd="mkdir -p $OUT && " + kerctl_download + "--kernel --out-dir $OUT `cat $(location :baseline-rpm-version)` && mv $OUT/*.rpm $OUT/kernel-bin.rpm",
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
    native.genrule(
        name = "baseline-sources",
        cmd = """sudo rm -rf $OUT; mkdir -p $OUT
            HOME=/dev/null git clone -b `cat $(location :from_tag)` `HOME=/dev/null git rev-parse --show-toplevel` $OUT
            pushd $OUT
            make mrproper
            popd
        """,
        cacheable=False,
        out="baseline-sources",
        labels = ["linux_kernel"],
    )

    #checkout target
    native.genrule(
        name = "target-sources",
        cmd = """sudo rm -rf $OUT; mkdir -p $OUT
            HOME=/dev/null git clone -b `cat $(location :to_tag)` `HOME=/dev/null git rev-parse --show-toplevel` $OUT
            pushd $OUT
            make mrproper
            popd
        """,
        cacheable = False,
        out = "target-sources",
        labels = ["linux_kernel"],
    )

    #build the rpm version
    #more details on versioning available in buildinfo()
    #i don't have better idea on how to map between git tag/branch and rpm
    #v5.6-fbk13-rc1 -> 5.6.13-0_fbk13_rc1
    flavor_ver = "_%s" % flavor if flavor else ""
    label_ver = "_%s" % label if label else ""

    native.genrule(
        name = "baseline-rpm-version",
        cmd = """
            pushd $(location :baseline-sources)
            majorver=`make -s kernelversion EXTRAVERSION=`
            popd
            rpm_n=0
            fbkv=`cat $(location :from_tag) | sed -e 's|.*-\\(fbk[0-9]*\\).*|\\1|g'`
            rc=`cat $(location :from_tag) | grep -oE '\\b-rc[0-9]+\\b$' | tr '-' '_'`
            flavor="%s"
            label="%s"
            echo "${majorver}-${rpm_n}_${fbkv}${flavor}${rc}${label}" > $OUT
        """ % (flavor_ver, label_ver),
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
        train_data_args = "-p /tmp/vmlinux.profdata"


    native.genrule(
      name = "kpatch-rpm-dir",
      cmd = """
          mkdir -p $OUT
          pushd $OUT
          curl -k -LO {kpatch_rpm_uri}
          popd
      """.format(kpatch_rpm_uri=KPATCH_BUILD_RPM_URI),
      out = "kpatch-rpm-dir",
      labels = ["linux_kernel"],
    )
    bind_ros.append(("$(location :kpatch-rpm-dir)", "/tmp/kpatch-build-rpm"))

    #feed artifacts to kpatch-build in a container
    bind_rws = [(":baseline-sources", "/rw/compile"), ("$OUT", "/rw/output"), (":target-sources", "/rw/target")]
    container_genrule(
        name="klp-build",
        arch=arch,
        cmd="""
            rpm -ivh /tmp/kernel-bin/*.rpm /tmp/kernel-devel/*.rpm --ignorearch
            test -f /tmp/kpatch-build-rpm/*.rpm && rpm -Uvh /tmp/kpatch-build-rpm/*.rpm
            pushd /rw/compile
            cp /tmp/config .config
            cp /tmp/bootconfig .bootconfig
            make {compiler_args} olddefconfig
            if grep -q CONFIG_LTO_CLANG=y /rw/compile/.config ; then
                ver=`cat /tmp/uname`
                # e.g., 6.4.3-0_fbk2_rc7_778_g4b661353af47 => -0_fbk2_rc7_778_g4b661353af47
                extraver=-`echo $ver | cut -d '-' -f 2`
                sed "s|EXTRAVERSION =|EXTRAVERSION = $extraver|g" -i Makefile
            fi
            popd

            # Save the original kpatch-build just in case that we need the original later.
            cp /bin/kpatch-build /tmp

            # ./scripts/setlocalversion signature changed in 6.4 compared to 5.19, the 5.19 option
            # '--save-scmversion' is not supported any more.
            #   f6e09b07cc12  kbuild: do not put .scmversion into the source tarball
            # Make a change to kpatch-build file itself to remove the unsupported option.
            sed "s|setlocalversion --save-scmversion|setlocalversion|g" -i /bin/kpatch-build

            # init/version-timestamp.c is introduced after 5.19.
            #   2df8220cc511  kbuild: build init/built-in.a just once
            # The init/version-timestamp.o is built after some compilation/build has been
            # done, so it can have accurate version information. The init/version-timestamp.o
            # is used in final linking. Different build environment, including machine or
            # build time can make init/version-timestamp.o different from each other.
            # Make two changes in kpatch-build to avoid init/version-timestamp.o introduced
            # build failures.
            sed "s|lib\/lib\.a)|init\/version-timestamp\.o \| lib\/lib\.a)|g" -i /bin/kpatch-build
            sed "s|usr\/initramfs_data\.o|usr\/initramfs_data\.o \|\| \"\$i\" = init\/version-timestamp\.o |g" -i /bin/kpatch-build

            # ./scripts/setlocalversion has the following change in 6.4 compared to 5.19,
            #   ec31f868ec674e  setlocalversion: absorb \$(KERNELVERSION)
            # it expects a non-empty KERNELVERSION string. Let us add it before calling kpatch-build.
            pushd /rw/compile
            export KERNELVERSION=`make kernelversion`
            popd

            export PATH=$PATH:/usr/libexec/git-core/
            TARGET_ARCH={arch} kpatch-build {train_data_args} -s /rw/compile -c /rw/compile/.config -v /boot/vmlinux* -o /rw/output -n klp_`cat /tmp/baseline_rpm_version`_`cat /tmp/hotfix` /tmp/patches/* || (cp /root/.kpatch/build.log /rw/output/ && exit 1)
            if readelf -S -W /rw/output/*.ko | grep -q '.klp.rela.vmlinux.__jump_table'; then
                echo "ERROR: .klp.rela.vmlinux.__jump_table found in KLP module." | tee -a /root/.kpatch/build.log
                echo "Patched functions reference static keys via static_branch_*()." | tee -a /root/.kpatch/build.log
                echo "Rewrite to use static_key_enabled() instead." | tee -a /root/.kpatch/build.log
                cp /root/.kpatch/build.log /rw/output/
                rm /rw/output/*.ko
                exit 1
            fi
        """.format(train_data_args = train_data_args, compiler_args = compiler_args, arch = arch),
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
            cp -a $(location :klp-build)/* $OUT
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
