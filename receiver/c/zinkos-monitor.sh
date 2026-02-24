#!/bin/bash
# zinkos-monitor — live stats, nothing stored. Ctrl+C to quit.

while true; do
  clear
  echo "=== Zinkos RX Monitor ==="
  echo ""

  # Service status
  status=$(systemctl is-active zinkos-rx 2>/dev/null)
  if [ "$status" = "active" ]; then
    pid=$(pgrep -x zinkos_rx)
    uptime=$(ps -o etime= -p "$pid" 2>/dev/null | xargs)
    echo "  Status:  RUNNING (pid $pid, uptime $uptime)"
  else
    echo "  Status:  STOPPED"
  fi

  # CPU temp
  temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
  if [ -n "$temp" ]; then
    echo "  CPU:     $((temp / 1000))°C"
  fi

  # Fan
  fan=$(cat /proc/acpi/ibm/fan 2>/dev/null | grep speed | awk '{print $2}')
  if [ -n "$fan" ]; then
    echo "  Fan:     ${fan} RPM"
  fi

  # Disk
  echo "  Disk:    $(df -h / | tail -1 | awk '{print $3 "/" $2 " (" $5 ")"}')"

  # Journal size
  jsize=$(journalctl --disk-usage 2>/dev/null | grep -oP '[\d.]+\S+')
  echo "  Logs:    $jsize"

  # Recent log lines
  echo ""
  echo "--- Last 10 log lines ---"
  journalctl -u zinkos-rx --no-pager -n 10 --no-hostname -o short-iso 2>/dev/null

  sleep 2
done
