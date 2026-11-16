#!/usr/bin/env bash
exec python3 "$(dirname "$0")/../cluster_control/verify_framework_ha.py" "$@"
