#!/usr/bin/env bash
# [XCDE] LINUX-002: portable native component tests, with no game input.
# Usage: scripts/native_tests_linux.sh [test_name ...]
# CC selects one compiler executable (use a wrapper for a command with arguments).
# These tests do not exercise Linux fibers, the kernel, Vulkan, or gameplay.
set -Eeuo pipefail

tests=(test_time_math test_movie_buf test_translate_tex test_translate_ldc
       test_translate_xfb test_tegra_block_height test_tegra_copy)
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [test_name ...]\n' "$0"
    printf 'Default: all seven portable tests. Available tests:\n'
    printf '  %s\n' "${tests[@]}"
    printf 'CC: one compiler executable (default gcc). MONADO_NATIVE_TEST_TIMEOUT: seconds (default 120).\n'
    exit 0
fi
if (( $# )); then tests=("$@"); fi
for test_name in "${tests[@]}"; do
    case "$test_name" in
        test_time_math|test_movie_buf|test_translate_tex|test_translate_ldc|test_translate_xfb|test_tegra_block_height|test_tegra_copy) ;;
        *) printf 'Unknown portable test: %s\n' "$test_name" >&2; exit 2 ;;
    esac
done

compiler=${CC:-gcc}
test_timeout=${MONADO_NATIVE_TEST_TIMEOUT:-120}
if [[ ! $test_timeout =~ ^[1-9][0-9]*$ ]]; then
    printf 'MONADO_NATIVE_TEST_TIMEOUT must be a positive integer in seconds.\n' >&2
    exit 2
fi
for required in "$compiler" timeout sha256sum mktemp tee; do
    command -v -- "$required" >/dev/null || {
        printf 'Required executable unavailable: %s\n' "$required" >&2
        exit 2
    }
done
compiler=$(command -v -- "$compiler")
[[ $compiler == /* ]] || compiler="$PWD/$compiler"
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$source_root"
umask 077
mkdir -p build/test
# Every invocation starts fresh, so failed compilation cannot reuse an old binary.
run_dir=$(mktemp -d "$source_root/build/test/linux-native.XXXXXXXX")
run_log="$run_dir/run.log"
printf 'Native component artifacts: %s\n' "$run_dir" | tee -a "$run_log"

run() {
    { printf '+'; printf ' %q' "$@"; printf '\n'; } | tee -a "$run_log"
    if "$@" 2>&1 | tee -a "$run_log"; then
        return 0
    else
        local result=$?
        printf 'FAIL: command exited %d; evidence retained at %s\n' "$result" "$run_dir" | tee -a "$run_log"
        return "$result"
    fi
}

run date -u '+%Y-%m-%dT%H:%M:%SZ'
run uname -srmo
run "$compiler" --version
run sha256sum scripts/native_tests_linux.sh
flags=(-O1 -ffp-contract=off -fno-strict-aliasing)
passed=0
for test_name in "${tests[@]}"; do
    sources=("tests/native/$test_name.c")
    headers=()
    extra=()
    case "$test_name" in
        test_time_math) ;;
        test_movie_buf)
            headers=(runtime/hle/movie_buf.h)
            extra=(-Iruntime)
            ;;
        test_translate_tex|test_translate_ldc|test_translate_xfb)
            sources+=(runtime/nvn/maxwell/translate.c)
            headers=(runtime/nvn/maxwell/shader.h)
            extra=(-Iruntime/nvn/maxwell)
            ;;
        test_tegra_block_height|test_tegra_copy)
            sources+=(runtime/nvn/tegra.c runtime/nvn/formats.c)
            headers=(runtime/nvn/tegra.h)
            extra=(-Iruntime/nvn -pthread)
            ;;
    esac
    run sha256sum "${sources[@]}" "${headers[@]}"
    run timeout --kill-after=5s 120s "$compiler" "${flags[@]}" "${extra[@]}" \
        "${sources[@]}" -o "$run_dir/$test_name"
    run sha256sum "$run_dir/$test_name"
    # Keep any incidental test output within this invocation's directory.
    (cd -- "$run_dir" && run timeout --kill-after=5s "${test_timeout}s" "./$test_name")
    passed=$((passed + 1))
done
printf 'PASS: %d portable component tests. Fibers/kernel/Vulkan/gameplay not covered.\n' "$passed" | tee -a "$run_log"
