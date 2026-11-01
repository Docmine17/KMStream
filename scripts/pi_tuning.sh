#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Usage: sudo $0 [--overclock-vpu]" >&2
    exit 1
fi

echo "==> Setting CPU governor to 'performance' on all cores"
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [[ -w "${gov}" ]]; then
        echo performance > "${gov}"
    fi
done
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor || true

echo "==> Configuring UDP socket buffer sysctls (/etc/sysctl.d/90-kmstream.conf)"
cat >/etc/sysctl.d/90-kmstream.conf <<'EOF'
net.core.rmem_max = 4194304
net.core.rmem_default = 2097152
net.core.netdev_max_backlog = 4096
EOF
/usr/sbin/sysctl -p /etc/sysctl.d/90-kmstream.conf

if [[ "${1:-}" == "--overclock-vpu" ]]; then
    CFG="/boot/firmware/config.txt"
    if [[ -f "${CFG}" ]]; then
        echo "==> Ensuring VPU 1080p60 clock profile (core_freq=450, h264_freq=333) in ${CFG}"
        grep -q "^core_freq=" "${CFG}" || echo "core_freq=450" >> "${CFG}"
        grep -q "^h264_freq=" "${CFG}" || echo "h264_freq=333" >> "${CFG}"
        echo "    (Reboot required for /boot/firmware/config.txt clock changes to take effect)"
    fi
fi

echo "==> Hardware & network diagnostics"
if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd get_throttled || true
    vcgencmd measure_temp || true
    vcgencmd measure_clock core || true
    vcgencmd measure_clock h264 || true
    vcgencmd measure_clock arm || true
fi
grep -i cma /proc/meminfo || true
netstat -su | grep -iE "receive buffer errors|packet receive errors|packets received" || true
