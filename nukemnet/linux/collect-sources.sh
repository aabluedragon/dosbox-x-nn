#!/bin/sh
set -eu
apt-get -o Acquire::Check-Valid-Until=false update -qq
mkdir -p /build/dependency-sources
cd /build/dependency-sources
python3 - <<'PY'
import json
import subprocess
for source in sorted(set(json.load(open('/build/portable/sources.json')).values())):
    package, version = source.split(' ', 1)
    subprocess.run(['apt-get', 'source', '--download-only', '--only-source', package + '=' + version], check=True)
PY
tar -czf "/build/output/dosbox-x-nn-linux-${TARGET_ARCH:-armv7l}-dependency-sources-20261109.tar.gz" -C /build/dependency-sources .
