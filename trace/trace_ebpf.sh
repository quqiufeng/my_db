#! /bin/bash
# tracec generated | target: ebpf | probes: 0
PID="${PID:-$1}"
if [ -z "$PID" ]; then echo "usage: PID=<pid> bash $0"; exit 1; fi
set -e

