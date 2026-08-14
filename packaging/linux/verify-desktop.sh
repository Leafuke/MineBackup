#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
executable="${1:-${repo_root}/build/linux-x64/bin/MineBackup}"
mode="${2:-all}"

if [[ ! -x "${executable}" ]]; then
    printf 'MineBackup executable is missing or not executable: %s\n' "${executable}" >&2
    exit 2
fi

run_smoke() {
    local expected_backend="$1"
    local profile="$2"
    local log="$3"
    shift 3

    set +e
    LC_ALL=C timeout --kill-after=2s 15s "$@" "${executable}" --data-dir "${profile}" >"${log}" 2>&1
    local status=$?
    set -e
    if [[ ${status} -ne 0 && ${status} -ne 124 ]]; then
        cat "${log}" >&2
        printf 'MineBackup exited unexpectedly during %s smoke test: %s\n' \
            "${expected_backend}" "${status}" >&2
        return 1
    fi
    if ! grep -Fq "GLFW selected platform: ${expected_backend}" "${log}"; then
        cat "${log}" >&2
        printf 'MineBackup did not select the expected GLFW backend: %s\n' \
            "${expected_backend}" >&2
        return 1
    fi
}

run_x11() (
    command -v xvfb-run >/dev/null
    local root
    root="$(mktemp -d)"
    trap 'rm -rf "${root}"' EXIT
    # Xvfb 提供的是独立 X11 会话；清除继承的 Wayland 会话标记，避免
    # GLFW 在自动探测时先连接不存在的 Wayland 显示而阻塞整个 smoke test。
    run_smoke X11 "${root}/MineBackup-中文/profile" "${root}/minebackup.log" \
        xvfb-run -a env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11
)

run_wayland() (
    command -v weston >/dev/null
    local root runtime weston_pid
    root="$(mktemp -d)"
    runtime="${root}/runtime"
    mkdir -m 700 "${runtime}"
    XDG_RUNTIME_DIR="${runtime}" weston --backend=headless-backend.so \
        --socket=minebackup-wayland --idle-time=0 --log="${root}/weston.log" &
    weston_pid=$!
    trap 'kill "${weston_pid}" 2>/dev/null || true; wait "${weston_pid}" 2>/dev/null || true; rm -rf "${root}"' EXIT
    for _ in {1..50}; do
        [[ -S "${runtime}/minebackup-wayland" ]] && break
        sleep 0.1
    done
    if [[ ! -S "${runtime}/minebackup-wayland" ]]; then
        cat "${root}/weston.log" >&2
        return 1
    fi
    run_smoke Wayland "${root}/MineBackup-中文/profile" "${root}/minebackup.log" \
        env -u DISPLAY XDG_RUNTIME_DIR="${runtime}" WAYLAND_DISPLAY=minebackup-wayland
)

case "${mode}" in
    x11) run_x11 ;;
    wayland) run_wayland ;;
    all) run_x11; run_wayland ;;
    *) printf 'Usage: %s [MineBackup executable] [all|x11|wayland]\n' "$0" >&2; exit 2 ;;
esac
