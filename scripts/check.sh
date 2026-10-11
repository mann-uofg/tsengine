#!/usr/bin/env bash
#
# Runs locally what CI runs: the format check, a build and the tests with GCC and Clang in
# debug (with sanitizers) and release, and the TSBS smoke test if the TSBS tools are there.
#
# usage: check.sh [work dir]

set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-${TSBENCH_WORK:-$HOME/tsbench}}"

cd "${SRC}"
git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run --Werror
echo "format: ok"

for cxx in g++ clang++; do
  for kind in Debug Release; do
    sanitize=OFF
    if [[ "${kind}" == "Debug" ]]; then
      sanitize=ON
    fi
    build="${WORK}/check-${cxx}-${kind}"
    CXX="${cxx}" cmake -S "${SRC}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE="${kind}" \
      -DTSENGINE_SANITIZE="${sanitize}" > /dev/null
    cmake --build "${build}" > "${build}/build.log" 2>&1 || {
      tail -30 "${build}/build.log"
      exit 1
    }
    (cd "${build}" && ctest -j 8 2>&1 | grep -E 'tests passed|tests failed|Failed')
    echo "${cxx} ${kind}: ok"
  done
done

if [[ -x "${WORK}/bin/tsbs_load_influx" ]]; then
  bash "${SRC}/scripts/tsbs_smoke.sh" "${WORK}/check-g++-Release/tsengine_server" "${WORK}/bin" |
    grep -E '^ok|PASS|FAIL|generated'
fi
echo "all checks passed"
