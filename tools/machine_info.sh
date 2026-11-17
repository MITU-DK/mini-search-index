#!/usr/bin/env bash
# Print the facts a benchmark result needs about the machine it ran on (DESIGN.md §10.2).
set -u

echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
echo "logical_cpus: $(nproc --all)"
echo "cores_per_socket: $(lscpu | sed -n 's/^Core(s) per socket: *//p')"
echo "threads_per_core: $(lscpu | sed -n 's/^Thread(s) per core: *//p')"
echo "ram: $(free -h | awk '/^Mem:/ {print $2 " total, " $7 " available"}')"
echo "kernel: $(uname -r)"
echo "compiler: $(g++ --version | head -1)"

if command -v powerprofilesctl >/dev/null 2>&1; then
  echo "power_profile: $(powerprofilesctl get)"
fi
governor=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ -r "$governor" ]; then
  echo "cpu_governor: $(cat "$governor")"
fi

if [ -n "${CODESPACE_NAME:-}" ]; then
  echo "cloud: GitHub Codespace ($CODESPACE_NAME)"
elif [ -r /sys/class/dmi/id/product_name ]; then
  echo "machine_model: $(cat /sys/class/dmi/id/product_name)"
fi
