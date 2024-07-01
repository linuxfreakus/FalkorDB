#!/bin/bash
# Copyright Broadcom, Inc. All Rights Reserved.
# SPDX-License-Identifier: APACHE-2.0

# shellcheck disable=SC1091

set -o errexit
set -o nounset
set -o pipefail
# set -o xtrace # Uncomment this line for debugging purposes

# Load Redis environment variables
. /opt/bitnami/scripts/redis-env.sh

# Load libraries
. /opt/bitnami/scripts/libos.sh
. /opt/bitnami/scripts/libredis.sh

export MODULE_DIR=/FalkorDB/bin/src

# Parse CLI flags to pass to the 'redis-server' call
args=("${REDIS_BASE_DIR}/etc/redis.conf" "--daemonize" "no" "--protected-mode" "no")
# Add flags specified via the 'REDIS_EXTRA_FLAGS' environment variable
read -r -a extra_flags <<< "$REDIS_EXTRA_FLAGS"
[[ "${#extra_flags[@]}" -gt 0 ]] && args+=("${extra_flags[@]}")
# Load FalkorDB Module
args+=("--loadmodule" "${MODULE_DIR}/falkordb.so" "${FALKORDB_ARGS}")
# Add flags passed to this script
args+=("$@")


if [ ${BROWSER:-1} -eq 1 ]
then
    if [ -d /FalkorDBBrowser ]
    then
        info "** Starting FalkorDBBrowser **"
        cd /FalkorDBBrowser && HOSTNAME="0.0.0.0" node server.js &
    fi
fi

info "** Starting FalkorDB **"
if am_i_root; then
    exec_as_user "$REDIS_DAEMON_USER" redis-server "${args[@]}"
else
    exec redis-server "${args[@]}"
fi
