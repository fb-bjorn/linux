#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import errno
import os
import re
import shlex
import socket
import sys
from enum import Enum, auto
from pathlib import Path

from lib.py import bkg, bpf_prog_map_ids, bpftool, cmd, defer
from lib.py import ethtool, ip, ksft_exit, ksft_run
from lib.py import rand_ports
from lib.py import wait_port_listen
from lib.py import CmdExitFailure, CmdInitFailure, EthtoolFamily
from lib.py import KsftFailEx, KsftSkipEx
from lib.py import NetdevFamily
from lib.py import NetDrvEpEnv
from lib.py import KsftNamedVariant, ksft_disruptive, ksft_variants


DEFAULT_COUNT = 256
DEFAULT_LENGTH = 256
DEFAULT_SEED = 1
RX_INTERVAL_US = 100
BASE_MTU = 1500
MAX_PACKET_SIZE = 65535
MIN_QUEUES = 2
TEST_QUEUE = 1
XSK_BIND_ERROR = 2
XSK_LAYOUT_UNSUPPORTED = 3
XSK_FEATURE_UNSUPPORTED = 4
ETH_HEADER_LEN = 14
# Keep these in step with xp_assign_dev() AF_XDP pool geometry.
XDP_PACKET_HEADROOM = 256
SG_CHUNK_SIZE = 4096
SG_RX_LENGTH = SG_CHUNK_SIZE + 1
MAX_HELPER_SEGS = 64
XSK_FRAME_ALIGN = 128
XSK_MTU_OVERHEAD = 26
UNSUPPORTED_ERRNOS = {errno.EINVAL, errno.EOPNOTSUPP}
UNSUPPORTED_MESSAGES = ("invalid argument", "not supported",
                        "operation not supported")
RECONFIG_REJECTIONS = UNSUPPORTED_MESSAGES + ("device or resource busy",)


class Support(Enum):
    REQUIRED = auto()
    OPTIONAL = auto()


class MtuMode(Enum):
    BASE = auto()
    CONFIGURED = auto()


class PeerStart(Enum):
    NORMAL = auto()
    GATED = auto()


def _require_zc(cfg):
    _setup(cfg)

    info = cfg.netnl.dev_get({"ifindex": cfg.ifindex})
    features = info.get("xdp-features", [])

    if "xsk-zerocopy" not in features:
        raise KsftSkipEx("device does not advertise AF_XDP zero-copy")
    if "basic" not in features or "redirect" not in features:
        raise KsftFailEx("incoherent AF_XDP zero-copy feature flags")

    cfg.zc_max_segs = info.get("xdp-zc-max-segs", 1)
    cfg.xsk_features = info.get("xsk-features", [])


def _set_base_mtu(cfg):
    _set_mtu(cfg, BASE_MTU)


@ksft_disruptive
def _require_disruptive():
    pass


def _skip_unsupported(result, operation, messages=UNSUPPORTED_MESSAGES):
    output = f"{result.stdout or ''}\n{result.stderr or ''}".lower()

    if any(message in output for message in messages):
        raise KsftSkipEx(f"driver rejected {operation}")

    raise KsftFailEx(f"{operation} failed: {result}")


def _require_ntuple(cfg):
    features = ethtool(f"-k {cfg.ifname}", json=True)[0]
    ntuple = features.get("ntuple-filters")

    if not ntuple:
        raise KsftSkipEx("device does not report ntuple filters")

    if ntuple["active"]:
        return
    if ntuple["fixed"]:
        raise KsftSkipEx("device does not support ntuple filters")

    try:
        ethtool(f"-K {cfg.ifname} ntuple-filters on")
    except CmdExitFailure as exc:
        _skip_unsupported(exc.cmd, "ntuple enable")
    defer(ethtool, f"-K {cfg.ifname} ntuple-filters off")


def _setup_queues(cfg, count):
    channels = cfg.ethnl.channels_get({"header": {"dev-index": cfg.ifindex}})
    current = channels.get("combined-count", 0)
    maximum = channels.get("combined-max", 0)

    if maximum < count:
        raise KsftSkipEx(f"need {count} combined queues, max is {maximum}")
    if current < count:
        try:
            ethtool(f"-L {cfg.ifname} combined {count}")
        except CmdExitFailure as exc:
            _skip_unsupported(exc.cmd, f"configuration of {count} queues")
        defer(ethtool, f"-L {cfg.ifname} combined {current}")


def _isolate_rx(cfg):
    try:
        ethtool(f"-X {cfg.ifname} equal 1")
    except CmdExitFailure as exc:
        _skip_unsupported(exc.cmd, "RSS isolation")
    defer(ethtool, f"-X {cfg.ifname} default")


def _add_rule(cfg, port, queue):
    ipver = cfg.addr_ipver
    flow = [f"flow-type udp{ipver}",
            f"src-ip {cfg.remote_addr}",
            f"dst-ip {cfg.addr}",
            f"src-port {port + 1}",
            f"dst-port {port}",
            f"action {queue}"]
    try:
        output = ethtool(f"-N {cfg.ifname} {' '.join(flow)}").stdout
    except CmdExitFailure as exc:
        _skip_unsupported(exc.cmd, "ntuple steering")
    match = re.search(r"(?:ID|id)\s+(\d+)", output)
    if not match:
        raise KsftFailEx(f"cannot parse ntuple rule ID: {output.strip()}")

    rule_id = int(match.group(1))
    defer(ethtool, f"-N {cfg.ifname} delete {rule_id}")


def _add_flow_rule(cfg, port, queue):
    _setup_queues(cfg, queue + 1)
    _require_ntuple(cfg)
    _isolate_rx(cfg)

    _add_rule(cfg, port, queue)


def _direct_mac(addr, ifname, direct, host=None):
    ipver = "6" if ":" in addr else "4"
    try:
        route = ip(f"-{ipver} route get {addr}", json=True, host=host)[0]
    except CmdExitFailure as exc:
        raise KsftSkipEx("test requires directly connected endpoints") \
            from exc

    if route.get("gateway") or route.get("dev") != ifname:
        raise KsftSkipEx("test requires directly connected endpoints")

    return direct


def _set_mtu(cfg, mtu):
    local_dev = ip(f"-d link show dev {cfg.ifname}", json=True)[0]
    local_mtu = local_dev["mtu"]
    if local_mtu != mtu:
        _require_disruptive()
        try:
            ip(f"link set dev {cfg.ifname} mtu {mtu}")
        except CmdExitFailure as exc:
            _skip_unsupported(exc.cmd, "local MTU configuration",
                              RECONFIG_REJECTIONS)
        defer(ip, f"link set dev {cfg.ifname} mtu {local_mtu}")

    remote_dev = ip(f"-d link show dev {cfg.remote_ifname}",
                    host=cfg.remote, json=True)[0]
    remote_mtu = remote_dev["mtu"]
    if remote_mtu < mtu:
        _require_disruptive()
        try:
            ip(f"link set dev {cfg.remote_ifname} mtu {mtu}",
               host=cfg.remote)
        except CmdExitFailure as exc:
            _skip_unsupported(exc.cmd, "peer MTU configuration",
                              RECONFIG_REJECTIONS)
        defer(ip, f"link set dev {cfg.remote_ifname} mtu {remote_mtu}",
              host=cfg.remote)


def _max_mtu(cfg):
    local = cfg.dev.get("max_mtu", MAX_PACKET_SIZE - ETH_HEADER_LEN)
    remote = cfg.remote_dev.get("max_mtu", local)

    return min(local, remote)


def _load_bpf(cfg, port, port2=None):
    obj = Path(__file__).with_name("xsk.bpf.o")
    pin_dir = f"/sys/fs/bpf/xsk-{os.getpid()}-{port}"

    cmd(f"rm -rf {pin_dir}", shell=True, fail=False)
    cmd(f"mkdir -p {pin_dir}", shell=True)
    defer(cmd, f"rm -rf {pin_dir}", shell=True, fail=False)

    bpftool(f"prog loadall {obj} {pin_dir} type xdp")
    pin = f"{pin_dir}/xsk_redirect"
    prog = bpftool(f"prog show pinned {pin}", json=True)
    prog_id = prog["id"]
    maps = bpf_prog_map_ids(prog_id)
    key = " ".join(f"{byte:02x}" for byte in
                   (0).to_bytes(4, sys.byteorder))
    value = " ".join(f"{byte:02x}" for byte in
                     port.to_bytes(4, sys.byteorder))
    bpftool(f"map update id {maps['xsk_port']} key hex {key} "
            f"value hex {value}")
    if port2 is not None:
        key = " ".join(f"{byte:02x}" for byte in
                       (1).to_bytes(4, sys.byteorder))
        value = " ".join(f"{byte:02x}"
                         for byte in port2.to_bytes(4, sys.byteorder))
        bpftool(f"map update id {maps['xsk_port']} key hex {key} "
                f"value hex {value}")

    return pin, maps["xsk_map"]


def _attach_xdp(cfg, pin):
    ip(f"link set dev {cfg.ifname} xdpdrv pinned {pin}")
    return defer(ip, f"link set dev {cfg.ifname} xdpdrv off")


def _common_args(cfg, role, port, count=DEFAULT_COUNT,
                 length=DEFAULT_LENGTH, seed=DEFAULT_SEED):
    local = role in ("dut-rx", "dut-tx", "shared")
    if role in ("dut-rx", "peer-tx", "shared"):
        src_ip = cfg.remote_addr
        dst_ip = cfg.addr
        src_mac = cfg.peer_mac
        dst_mac = cfg.rx_dmac
    else:
        src_ip = cfg.addr
        dst_ip = cfg.remote_addr
        src_mac = cfg.dut_mac
        dst_mac = cfg.tx_dmac

    binary = cfg.bin_local if local else cfg.bin_remote
    args = [str(binary), role, "--interface",
            cfg.ifname if local else cfg.remote_ifname,
            f"--ipv{cfg.addr_ipver}", "--src-ip", src_ip, "--dst-ip",
            dst_ip, "--src-mac", src_mac, "--dst-mac", dst_mac,
            "--port", str(port), "--count", str(count), "--length",
            str(length), "--seed", str(seed)]

    return args


def _probe(cfg, extra=None, support=Support.REQUIRED, expect_errno=None,
           length=DEFAULT_LENGTH, count=DEFAULT_COUNT):
    args = [str(cfg.bin_local), "probe", "--interface", cfg.ifname,
            "--queue", str(TEST_QUEUE), "--length", str(length),
            "--count", str(count)]
    if extra:
        args += extra

    result = cmd(shlex.join(args), fail=False)
    match = re.search(r"bind_errno=(\d+)", result.stderr)
    bind_errno = int(match.group(1)) if match else None

    if result.ret == XSK_BIND_ERROR and bind_errno is None:
        raise KsftFailEx(f"AF_XDP setup failed: {result}")
    if expect_errno is not None:
        if result.ret != XSK_BIND_ERROR or bind_errno != expect_errno:
            raise KsftFailEx(f"bind returned {bind_errno}, expected "
                             f"{expect_errno}")
        return
    if result.ret == 0:
        return
    if result.ret != XSK_BIND_ERROR:
        raise KsftFailEx(f"AF_XDP probe failed: {result}")
    if support == Support.OPTIONAL and bind_errno in UNSUPPORTED_ERRNOS:
        raise KsftSkipEx(f"zero-copy layout rejected: errno={bind_errno}")

    raise KsftFailEx(f"advertised zero-copy bind failed: errno={bind_errno}")


def _helper_failure(exc, support):
    match = re.search(r"bind_errno=(\d+)", exc.cmd.stderr or "")
    bind_errno = int(match.group(1)) if match else None

    if (support == Support.OPTIONAL and
            exc.cmd.ret == XSK_LAYOUT_UNSUPPORTED):
        raise KsftSkipEx("zero-copy layout is unusable on this host") from exc
    if (support == Support.OPTIONAL and
            exc.cmd.ret == XSK_BIND_ERROR and
            bind_errno in UNSUPPORTED_ERRNOS):
        raise KsftSkipEx(f"zero-copy layout rejected: errno={bind_errno}") \
            from exc
    if exc.cmd.ret == XSK_FEATURE_UNSUPPORTED:
        raise KsftSkipEx("required interface setup is unsupported") from exc

    raise KsftFailEx(f"AF_XDP helper failed: {exc.cmd}") from exc


def _run_peer(cfg, args):
    return cmd(shlex.join(args), host=cfg.remote, timeout=60)


def _release(cfg, target, port):
    family = socket.AF_INET6 if cfg.addr_ipver == "6" else socket.AF_INET
    address = (target, port)
    if family == socket.AF_INET6:
        address += (0, cfg.ifindex)

    try:
        with socket.socket(family, socket.SOCK_STREAM) as control:
            control.settimeout(2)
            control.connect(address)
    except OSError as exc:
        raise KsftFailEx("cannot release gated helper") from exc


def _release_peer(cfg, port):
    _release(cfg, cfg.remote_addr, port)


def _run_rx(cfg, *, count=DEFAULT_COUNT, length=DEFAULT_LENGTH,
            seed=DEFAULT_SEED, extra=None, support=Support.REQUIRED,
            peer_extra=None, peer_count=None, mtu_mode=MtuMode.BASE,
            peer_start=PeerStart.NORMAL):
    port, ready_port = rand_ports(2)

    _require_zc(cfg)
    if mtu_mode == MtuMode.BASE:
        _set_base_mtu(cfg)
    _add_flow_rule(cfg, port, TEST_QUEUE)
    pin, map_id = _load_bpf(cfg, port)
    xdp_cleanup = _attach_xdp(cfg, pin)

    try:
        _probe(cfg, extra, support, length=length, count=count)
        dut = _common_args(cfg, "dut-rx", port, count, length, seed)
        dut += ["--queue", str(TEST_QUEUE), "--map-id", str(map_id)]
        peer = _common_args(cfg, "peer-tx", port,
                            count if peer_count is None else peer_count,
                            length, seed)
        if extra:
            dut += extra
        if peer_extra:
            peer += peer_extra
        else:
            peer += ["--interval-us", str(RX_INTERVAL_US)]

        if peer_start == PeerStart.GATED:
            peer += ["--ready-port", str(ready_port)]
            with bkg(shlex.join(peer), host=cfg.remote, exit_wait=True):
                wait_port_listen(ready_port, host=cfg.remote)
                with bkg(shlex.join(dut), ksft_ready=True,
                         exit_wait=True):
                    _release_peer(cfg, ready_port)
            return

        with bkg(shlex.join(dut), ksft_ready=True, exit_wait=True):
            _run_peer(cfg, peer)
    except (CmdExitFailure, CmdInitFailure) as exc:
        _helper_failure(exc, support)
    finally:
        xdp_cleanup.exec()


def _run_tx(cfg, *, count=DEFAULT_COUNT, length=DEFAULT_LENGTH,
            seed=DEFAULT_SEED, extra=None, peer_count=None,
            support=Support.REQUIRED, peer_extra=None,
            mtu_mode=MtuMode.BASE):
    port, ready_port = rand_ports(2)
    watch_silence = peer_count == 0

    _require_zc(cfg)
    if mtu_mode == MtuMode.BASE:
        _set_base_mtu(cfg)
    _setup_queues(cfg, MIN_QUEUES)
    pin, _ = _load_bpf(cfg, port)
    xdp_cleanup = _attach_xdp(cfg, pin)

    try:
        _probe(cfg, extra, support, length=length, count=count)
        dut = _common_args(cfg, "dut-tx", port, count, length, seed)
        dut += ["--queue", str(TEST_QUEUE)]
        peer = _common_args(cfg, "peer-rx", port,
                            count if peer_count is None else peer_count,
                            length, seed)
        if extra:
            dut += extra
        if peer_extra:
            peer += peer_extra

        command = shlex.join(peer + ["--ready-port", str(ready_port)])
        with bkg(command, host=cfg.remote, exit_wait=True):
            wait_port_listen(ready_port, host=cfg.remote)
            try:
                cmd(shlex.join(dut), timeout=60)
            finally:
                if watch_silence:
                    _release_peer(cfg, ready_port)
    except (CmdExitFailure, CmdInitFailure) as exc:
        _helper_failure(exc, support)
    finally:
        xdp_cleanup.exec()


def test_rx_4k(cfg):
    _run_rx(cfg)


def test_tx_4k(cfg):
    _run_tx(cfg)


def test_rx_2k(cfg):
    _run_rx(cfg, length=1514, extra=["--chunk-size", "2048"],
            support=Support.OPTIONAL)


def test_rx_fill_mask(cfg):
    _run_rx(cfg, length=1514,
            extra=["--chunk-size", "2048", "--fill-offset", "64"],
            support=Support.OPTIONAL)


@ksft_variants([
    KsftNamedVariant("1", 1),
    KsftNamedVariant("128", 128),
    KsftNamedVariant("256", 256),
])
def test_rx_headroom(cfg, headroom):
    _run_rx(cfg, length=1514,
            extra=["--chunk-size", "2048", "--headroom", str(headroom)],
            support=Support.OPTIONAL)


def test_rx_headroom_limit(cfg):
    _require_zc(cfg)
    _set_base_mtu(cfg)
    _setup_queues(cfg, MIN_QUEUES)
    _probe(cfg, ["--chunk-size", "2048", "--headroom", "257"],
           expect_errno=errno.EINVAL)


@ksft_disruptive
def test_rx_single_boundary(cfg):
    chunk_size = 2048
    headroom = 256

    _require_zc(cfg)

    capacity = chunk_size - XDP_PACKET_HEADROOM - headroom
    capacity &= -XSK_FRAME_ALIGN
    mtu = capacity - XSK_MTU_OVERHEAD
    length = mtu + ETH_HEADER_LEN

    if mtu > _max_mtu(cfg):
        raise KsftSkipEx(f"test needs MTU {mtu}")
    _set_mtu(cfg, mtu)
    _run_rx(cfg, length=length,
            extra=["--chunk-size", str(chunk_size),
                   "--headroom", str(headroom)],
            support=Support.OPTIONAL, mtu_mode=MtuMode.CONFIGURED)


def test_rx_unaligned(cfg):
    _run_rx(cfg, length=1514,
            extra=["--chunk-size", "2048", "--fill-offset", "128",
                   "--unaligned"], support=Support.OPTIONAL)


def test_rx_unaligned_3k(cfg):
    _run_rx(cfg, length=1514,
            extra=["--chunk-size", "3072", "--fill-offset", "128",
                   "--unaligned"], support=Support.OPTIONAL)


def test_rx_cross_page(cfg):
    # Prove steering before treating cross-page silence as unsupported.
    _run_rx(cfg, count=1)
    _run_rx(cfg, count=8, length=512,
            extra=["--chunk-size", "2048", "--frames", "64",
                   "--ring-size", "64", "--cross-page",
                   "--probe-layout", "--unaligned", "--allow-gaps",
                   "--timeout-ms", "2000"], support=Support.OPTIONAL,
            peer_count=64, peer_extra=["--interval-us", "100"])


def test_tx_offset(cfg):
    _run_tx(cfg, length=256,
            extra=["--tx-offset", str(4096 - 256)])


def test_tx_invalid(cfg):
    _run_tx(cfg, count=1, length=256,
            extra=["--tx-offset", str(4096 - 256), "--invalid-tx",
                   "--timeout-ms", "1000"],
            peer_count=0, peer_extra=["--timeout-ms", "1000"])


def test_tx_unaligned(cfg):
    _run_tx(cfg, length=256,
            extra=["--chunk-size", "3072", "--tx-offset", "128",
                   "--unaligned"], support=Support.OPTIONAL)


def test_tx_unaligned_end(cfg):
    _run_tx(cfg, length=256,
            extra=["--chunk-size", "3072", "--tx-offset",
                   str(3072 - 256), "--unaligned"],
            support=Support.OPTIONAL)


def test_rx_reuse(cfg):
    _run_rx(cfg, count=100000,
            extra=["--frames", "64", "--ring-size", "64",
                   "--allow-gaps"],
            peer_count=200000, peer_extra=["--interval-us", "20"])


def test_tx_wrap(cfg):
    _run_tx(cfg, count=4096,
            extra=["--frames", "8", "--ring-size", "8"])


def test_tx_ring_full(cfg):
    ring_size = 8

    _run_tx(cfg, count=ring_size + 1,
            extra=["--frames", str(ring_size), "--ring-size",
                   str(ring_size), "--tx-full"])


def test_rx_refill(cfg):
    _run_rx(cfg, count=128,
            extra=["--frames", "64", "--ring-size", "64",
                   "--starve-after", "64", "--hold-ms", "100",
                   "--refill-batch", "16", "--allow-gaps"],
            peer_count=2048, peer_extra=["--interval-us", "1000"])


def test_rx_ring_full(cfg):
    _run_rx(cfg, count=64,
            extra=["--frames", "128", "--ring-size", "128",
                   "--rx-ring-size", "64", "--hold-ms", "100"],
            peer_count=512, peer_extra=["--interval-us", "100"],
            peer_start=PeerStart.GATED)


@ksft_disruptive
def test_rx_sg(cfg):
    _require_zc(cfg)
    if cfg.zc_max_segs < 2:
        raise KsftSkipEx("device does not advertise multi-buffer zero-copy")
    if cfg.zc_max_segs > MAX_HELPER_SEGS:
        raise KsftSkipEx(f"segment limit {cfg.zc_max_segs} is too large")

    length = SG_RX_LENGTH
    mtu = length - ETH_HEADER_LEN
    if mtu > _max_mtu(cfg):
        raise KsftSkipEx(f"test needs MTU {mtu}")

    _set_mtu(cfg, mtu)
    _run_rx(cfg, length=length,
            extra=["--sg", "--frags", str(cfg.zc_max_segs),
                   "--min-frags", "2"],
            mtu_mode=MtuMode.CONFIGURED)


@ksft_disruptive
def test_tx_sg(cfg):
    _require_zc(cfg)
    if cfg.zc_max_segs < 2:
        raise KsftSkipEx("device does not advertise multi-buffer zero-copy")
    if cfg.zc_max_segs > MAX_HELPER_SEGS:
        raise KsftSkipEx(f"segment limit {cfg.zc_max_segs} is too large")

    for segs in sorted({2, cfg.zc_max_segs}):
        _run_tx(cfg, count=16, extra=["--sg", "--frags", str(segs)])


@ksft_disruptive
def test_rx_sg_required(cfg):
    _require_zc(cfg)
    if cfg.zc_max_segs < 2:
        raise KsftSkipEx("device does not advertise multi-buffer zero-copy")

    length = SG_RX_LENGTH
    mtu = length - ETH_HEADER_LEN
    if mtu > _max_mtu(cfg):
        raise KsftSkipEx(f"test needs MTU {mtu}")

    _set_mtu(cfg, mtu)
    _setup_queues(cfg, MIN_QUEUES)
    _probe(cfg, expect_errno=errno.EINVAL)


@ksft_disruptive
def test_tx_sg_limit(cfg):
    _require_zc(cfg)
    if cfg.zc_max_segs < 2:
        raise KsftSkipEx("device does not advertise multi-buffer zero-copy")

    segs = cfg.zc_max_segs + 1
    if segs > MAX_HELPER_SEGS:
        raise KsftSkipEx(f"segment limit {cfg.zc_max_segs} is too large")
    _set_base_mtu(cfg)

    _run_tx(cfg, count=1, length=DEFAULT_LENGTH,
            extra=["--sg", "--frags", str(segs), "--split-submit"],
            peer_count=0, peer_extra=["--timeout-ms", "1000"])


def _setup(cfg):
    if getattr(cfg, "_xsk_setup_done", False):
        return

    cfg.ethnl = EthtoolFamily()
    cfg.netnl = NetdevFamily()
    cfg.bin_local = Path(__file__).with_name("xsk")

    cfg.dut_mac = cfg.dev["address"]
    cfg.peer_mac = cfg.remote_dev["address"]
    cfg.rx_dmac = _direct_mac(cfg.addr, cfg.remote_ifname, cfg.dut_mac,
                              host=cfg.remote)
    cfg.tx_dmac = _direct_mac(cfg.remote_addr, cfg.ifname, cfg.peer_mac)
    cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)
    cfg._xsk_setup_done = True


def main():
    env = NetDrvEpEnv(__file__)

    with env as cfg:
        ksft_run([
            test_rx_4k,
            test_tx_4k,
            test_rx_2k,
            test_rx_fill_mask,
            test_rx_headroom,
            test_rx_headroom_limit,
            test_rx_single_boundary,
            test_rx_unaligned,
            test_rx_unaligned_3k,
            test_rx_cross_page,
            test_tx_offset,
            test_tx_invalid,
            test_tx_unaligned,
            test_tx_unaligned_end,
            test_rx_reuse,
            test_tx_wrap,
            test_tx_ring_full,
            test_rx_refill,
            test_rx_ring_full,
            test_rx_sg,
            test_tx_sg,
            test_rx_sg_required,
            test_tx_sg_limit,
        ], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
