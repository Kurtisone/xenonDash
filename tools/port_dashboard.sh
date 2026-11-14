#!/bin/sh
# Chaîne de portage d'un Dashboard : voir tools/port_dashboard.py --help et docs/technique.md (« Porter une nouvelle version »).
# usage: tools/port_dashboard.sh <mise_à_jour|paquet_STFS|dash.xex> <version> [options]
exec python3 "$(dirname "$0")/port_dashboard.py" "$@"
