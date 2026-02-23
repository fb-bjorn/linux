#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

source ethtool-common.sh

NSIM_NETDEV=$(make_netdev)

set -o pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
YNL=$SCRIPT_DIR/../../../../../net/ynl/pyynl/cli.py
SPEC=$SCRIPT_DIR/../../../../../../Documentation/netlink/specs/ethtool.yaml

if [ ! -f "$YNL" ]; then
    echo "SKIP: YNL CLI not found at $YNL"
    exit 4
fi

if ! command -v jq &>/dev/null; then
    echo "SKIP: jq not found"
    exit 4
fi

MOD_DFS=$NSIM_DEV_DFS/ethtool/module

# SFF-8024 physical ID for QSFP-DD (CMIS module)
SFF8024_ID_QSFP_DD='\x18'
# Page 01h Byte 142 bit 5: Page 13h supported
CMIS_PAGE13_SUPPORTED='\x20'

ynl_get() {
    $YNL --spec $SPEC --do loopback-get \
	 --json '{"header":{"dev-name":"'$NSIM_NETDEV'"}}' --output-json
}

ynl_set() {
    $YNL --spec $SPEC --do loopback-set \
	 --json '{"header":{"dev-name":"'$NSIM_NETDEV'"}, '"$1"'}'
}

seed_eeprom() {
    # Page 0, byte 0: physical ID
    printf $1 | dd of=$MOD_DFS/pages/0 bs=1 seek=0 conv=notrunc 2>/dev/null
    # Page 1, byte 14 (offset 142-128): diagnostic support flags
    printf $2 | dd of=$MOD_DFS/pages/1 bs=1 seek=14 conv=notrunc 2>/dev/null
    # Page 19 (0x13), byte 0 (offset 128-128): loopback capability
    printf $3 | dd of=$MOD_DFS/pages/19 bs=1 seek=0 conv=notrunc 2>/dev/null
}

clear_eeprom() {
    dd if=/dev/zero of=$MOD_DFS/pages/0 bs=128 count=1 2>/dev/null
    dd if=/dev/zero of=$MOD_DFS/pages/1 bs=128 count=1 2>/dev/null
    dd if=/dev/zero of=$MOD_DFS/pages/19 bs=128 count=1 2>/dev/null
}

# Default state: all zeros, not a CMIS module.
# loopback-get should return empty (no entries).
s=$(ynl_get | jq '.entry | length')
check $? "$s" "0"

# Seed CMIS module with all 4 loopback capabilities (cap byte = 0x0f)
seed_eeprom $SFF8024_ID_QSFP_DD $CMIS_PAGE13_SUPPORTED '\x0f'

# Verify all 4 entries appear with enabled=0
s=$(ynl_get | jq '.entry | length')
check $? "$s" "4"

s=$(ynl_get | jq '[.entry[] | .enabled] | all(. == 0)')
check $? "$s" "true"

# SET requires device DOWN
ip link set dev $NSIM_NETDEV down

# Enable host-side input loopback (PMD-host, host-loop-host)
ynl_set '"entry": [{"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1}]' >/dev/null
check $?

s=$(ynl_get | jq '[.entry[] | select(.layer == "pmd-host" and .direction == "host-loop-host")] | .[0].enabled')
check $? "$s" "1"

# Enable media-side output loopback (PMD-media, line-loop-line)
ynl_set '"entry": [{"layer": "pmd-media", "direction": "line-loop-line", "enabled": 1}]' >/dev/null
check $?

s=$(ynl_get | jq '[.entry[] | select(.layer == "pmd-media" and .direction == "line-loop-line")] | .[0].enabled')
check $? "$s" "1"

# Enable all 4 at once
ynl_set '"entry": [
    {"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1},
    {"layer": "pmd-host", "direction": "line-loop-line", "enabled": 1},
    {"layer": "pmd-media", "direction": "host-loop-host", "enabled": 1},
    {"layer": "pmd-media", "direction": "line-loop-line", "enabled": 1}
]' >/dev/null
check $?

s=$(ynl_get | jq '[.entry[] | .enabled] | all(. == 1)')
check $? "$s" "true"

# Disable all
ynl_set '"entry": [
    {"layer": "pmd-host", "direction": "host-loop-host", "enabled": 0},
    {"layer": "pmd-host", "direction": "line-loop-line", "enabled": 0},
    {"layer": "pmd-media", "direction": "host-loop-host", "enabled": 0},
    {"layer": "pmd-media", "direction": "line-loop-line", "enabled": 0}
]' >/dev/null
check $?

s=$(ynl_get | jq '[.entry[] | .enabled] | all(. == 0)')
check $? "$s" "true"

# SET while UP must fail
ip link set dev $NSIM_NETDEV up
ynl_set '"entry": [{"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1}]' >/dev/null 2>&1
check $? '' '' 1
ip link set dev $NSIM_NETDEV down

# Partial capabilities: only host-side input + media-side output (cap = 0x09)
clear_eeprom
seed_eeprom $SFF8024_ID_QSFP_DD $CMIS_PAGE13_SUPPORTED '\x09'

s=$(ynl_get | jq '.entry | length')
check $? "$s" "2"

s=$(ynl_get | jq '[.entry[] | .layer + "," + .direction] | sort | join(" ")')
check $? "${s//\"}" "pmd-host,host-loop-host pmd-media,line-loop-line"

# Enabling unsupported type must fail
ynl_set '"entry": [{"layer": "pmd-host", "direction": "line-loop-line", "enabled": 1}]' >/dev/null 2>&1
check $? '' '' 1

# Supported type should work
ynl_set '"entry": [{"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1}]' >/dev/null
check $?

s=$(ynl_get | jq '[.entry[] | select(.layer == "pmd-host" and .direction == "host-loop-host")] | .[0].enabled')
check $? "$s" "1"

# Non-CMIS module: phys_id=0x03 (SFP)
clear_eeprom
printf '\x03' | dd of=$MOD_DFS/pages/0 bs=1 seek=0 conv=notrunc 2>/dev/null
s=$(ynl_get | jq '.entry | length')
check $? "$s" "0"

# CMIS module but no Page 13h advertised
clear_eeprom
seed_eeprom $SFF8024_ID_QSFP_DD '\x00' '\x0f'
s=$(ynl_get | jq '.entry | length')
check $? "$s" "0"

# GET error injection (module EEPROM get_err)
clear_eeprom
seed_eeprom $SFF8024_ID_QSFP_DD $CMIS_PAGE13_SUPPORTED '\x0f'

echo 5 > $MOD_DFS/get_err
ynl_get >/dev/null 2>&1
check $? '' '' 1
echo 0 > $MOD_DFS/get_err

# SET error injection (module EEPROM set_err)
echo 5 > $MOD_DFS/set_err
ynl_set '"entry": [{"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1}]' >/dev/null 2>&1
check $? '' '' 1
echo 0 > $MOD_DFS/set_err

# Verify normal operation restored after clearing error injection
ynl_get >/dev/null 2>&1
check $?
ynl_set '"entry": [{"layer": "pmd-host", "direction": "host-loop-host", "enabled": 1}]' >/dev/null
check $?

if [ $num_errors -eq 0 ]; then
    echo "PASSED all $((num_passes)) checks"
    exit 0
else
    echo "FAILED $num_errors/$((num_errors+num_passes)) checks"
    exit 1
fi
