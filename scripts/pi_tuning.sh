#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Usage: sudo $0" >&2
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

echo "==> Hardware & network diagnostics"
if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd get_throttled || true
    vcgencmd measure_temp || true
    vcgencmd measure_clock core || true
    vcgencmd measure_clock arm || true
fi
grep -i cma /proc/meminfo || true
netstat -su | grep -iE "receive buffer errors|packet receive errors|packets received" || true
