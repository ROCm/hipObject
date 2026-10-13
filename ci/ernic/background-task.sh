#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run a slow setup task of an emulated-hardware lane in the background, and
# join it from a later step, so it overlaps the steps in between.
#
# Usage:
#   background-task.sh start NAME < SCRIPT
#   background-task.sh wait NAME [TIMEOUT_SECONDS]
#
# start reads a bash script from stdin and returns at once, leaving the script
# running under "bash -e", as a run step would. Its output goes to NAME.log
# and its exit status to NAME.rc, both in ${BACKGROUND_TASK_DIR:-/tmp/bg}.
# wait blocks until the status appears, prints the log, and exits with the
# script's status. It fails if TIMEOUT_SECONDS (default 1800) pass first, or
# if NAME was never started.
#
# A background process outlives the step that started it -- the lanes rely on
# that for rocm-ernic and QEMU -- but only if it holds none of the step's
# stdio: the runner waits for that to close before it ends the step. Hence the
# redirections on the subshell below.

set -euo pipefail

usage() {
    echo "Usage: $0 start NAME < SCRIPT | wait NAME [TIMEOUT_SECONDS]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
action=$1
name=$2
dir="${BACKGROUND_TASK_DIR:-/tmp/bg}"
script="${dir}/${name}.sh"
log="${dir}/${name}.log"
status="${dir}/${name}.rc"

case "${action}" in
    start)
        [ $# -eq 2 ] || usage
        mkdir -p "${dir}"
        rm -f "${status}"
        # Read the script now, while stdin is still the caller's, and create
        # the log before returning, so wait can tell started from not.
        cat > "${script}"
        : > "${log}"
        (
            rc=0
            bash -e "${script}" > "${log}" 2>&1 || rc=$?
            # Renamed into place so wait never reads a half-written status.
            echo "${rc}" > "${status}.tmp"
            mv "${status}.tmp" "${status}"
        ) < /dev/null > /dev/null 2>&1 &
        echo "Started ${name} in the background"
        ;;
    wait)
        [ $# -le 3 ] || usage
        timeout=${3:-1800}
        if [ ! -f "${log}" ]; then
            echo "${name} was never started" >&2
            exit 1
        fi
        until [ -f "${status}" ]; do
            if [ "${SECONDS}" -ge "${timeout}" ]; then
                cat "${log}"
                echo "${name} did not finish within ${timeout}s" >&2
                exit 1
            fi
            sleep 2
        done
        cat "${log}"
        rc=$(cat "${status}")
        echo "${name} finished after waiting ${SECONDS}s"
        if [ "${rc}" -ne 0 ]; then
            echo "${name} failed with exit status ${rc}" >&2
        fi
        exit "${rc}"
        ;;
    *)
        usage
        ;;
esac
