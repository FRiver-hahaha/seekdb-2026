#!/usr/bin/env bash
# Regression test for https://github.com/oceanbase/seekdb/issues/1544
#
# A fresh bootstrap must not depend on the short rpc_timeout budget. Before the fix,
# ObBootstrap::init_global_stat() wrote __all_global_stat through ObGlobalStatProxy::update(),
# which bounded the whole operation with ObShareUtil::get_rs_default_timeout_ctx()
# (rpc_timeout, 2s by default). The rows live in __all_core_table, so the SELECT ...
# FOR UPDATE alone could consume that budget on slow storage; the following DML then failed
# with OB_TIMEOUT (-4012) and the native process exited before becoming ready.
#
# This test starts a throw-away instance with rpc_timeout=1ms. Any bootstrap SQL that still
# uses the RPC budget fails deterministically, while global-stat operations must use the
# regular internal SQL budget and complete normally.
#
# Usage: tools/ci/bootstrap_timeout_smoke_test.sh <seekdb-binary> [timeout-seconds]
set -euo pipefail

SEEKDB_BIN="${1:-}"
TIMEOUT_SECONDS="${2:-180}"
if [[ -z "${SEEKDB_BIN}" || ! -x "${SEEKDB_BIN}" ]]; then
  echo "usage: $0 <seekdb-binary> [timeout-seconds]" >&2
  exit 2
fi

BASE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/seekdb-bootstrap-timeout.XXXXXX")"
PORT="${SEEKDB_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')}"
LOG_DIR="${BASE_DIR}/log"
CONSOLE_LOG="${BASE_DIR}/console.log"
READY_MARKER="observer instance start success"

pid=""
cleanup() {
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
  rm -rf "${BASE_DIR}"
}
trap cleanup EXIT

dump_logs() {
  if [[ -f "${CONSOLE_LOG}" ]]; then
    echo "----- console.log -----" >&2
    tail -n 120 "${CONSOLE_LOG}" >&2 || true
  fi
  for log in "${LOG_DIR}"/*.log; do
    if [[ -f "${log}" ]]; then
      echo "----- ${log} -----" >&2
      tail -n 120 "${log}" >&2 || true
    fi
  done
}

echo "[bootstrap-timeout] starting ${SEEKDB_BIN} with rpc_timeout=1ms (base_dir=${BASE_DIR})"
"${SEEKDB_BIN}" --nodaemon --base-dir="${BASE_DIR}" --port="${PORT}" \
  --parameter rpc_timeout=1ms >"${CONSOLE_LOG}" 2>&1 &
pid=$!

deadline=$((SECONDS + TIMEOUT_SECONDS))
while (( SECONDS < deadline )); do
  if ! kill -0 "${pid}" 2>/dev/null; then
    status=0
    wait "${pid}" || status=$?
    echo "[bootstrap-timeout] FAIL: seekdb exited before becoming ready (status ${status})" >&2
    dump_logs
    exit 1
  fi
  if grep -Rqs "${READY_MARKER}" "${LOG_DIR}" 2>/dev/null; then
    echo "[bootstrap-timeout] PASS: fresh database bootstrapped and became ready"
    exit 0
  fi
  sleep 1
done

echo "[bootstrap-timeout] FAIL: timed out after ${TIMEOUT_SECONDS}s" >&2
dump_logs
exit 1
