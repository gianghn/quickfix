#!/usr/bin/env bash
# Plan items 8.3-8.6 (owner decision Q12): long runs of krx_stress on the lab Kubernetes cluster,
# one Indexed Job per suite, one pod per seed (seed = the pod's index + 1). Each pod clones the
# commit, builds krx_stress in its variant and runs it; its log is the run's record.
#
#   run.sh setup                                  the namespace and its quota (namespace.yaml)
#   run.sh start NAME VARIANT RUNS PARALLEL [--hours H] [--engine SHA] [--cpu N] -- STRESS_ARGS...
#                                                 VARIANT: release, asan (ASan+UBSan) or tsan;
#                                                 fuzz: krx_fuzz instead (ASan, clang 18),
#                                                 STRESS_ARGS are libFuzzer's (8.6);
#                                                 {seed} in STRESS_ARGS is the pod's seed;
#                                                 --engine: today's harness on an older engine
#                                                 (a red run)
#   run.sh results NAME                           one line per seed, then the PASS count
#   run.sh delete NAME
#
# e.g. 50 ASan chaos runs, 10 at a time (8.3):
#   run.sh start chaos-asan asan 50 10 -- 90 4 3 7700 {seed} /tmp/w --no-echo
# an 8 h soak (8.5):
#   run.sh start soak release 1 1 --hours 9 --cpu 1 -- 28800 4 3 7700 1 /tmp/w --soak --rate=200 \
#     --chaos-ms=5000 --report=60 --settle=600
# 120 CPU-hours of fuzzing, about 24 per mode (8.6):
#   run.sh start fuzz fuzz 10 10 --hours 13 --cpu 1 -- -max_total_time=43200 -rss_limit_mb=4096
# The commit is HEAD, which must be pushed (the pods clone it from GitHub). KUBECONFIG must point
# at the lab cluster (rke2-lab-ansible/fetched/kubeconfig.yaml).
set -euo pipefail

readonly NAMESPACE=krx-soak
readonly REPO=https://github.com/gianghn/quickfix.git
ENGINE="" # --engine: the engine's sources (src/C++ but its tests) from this commit instead
CPU=2     # --cpu: CPUs a pod requests (it may use twice that); a soak or a fuzzer needs 1
HERE=$(cd "$(dirname "$0")" && pwd)
readonly HERE

die() {
  echo "run.sh: $*" >&2
  exit 2
}

# The pod's script: build the variant, then run with the seed in place of {seed}.
pod_script() {
  cat <<'SCRIPT'
set -euo pipefail
seed=$((JOB_COMPLETION_INDEX + 1))
packages="g++ cmake make git ca-certificates gdb"
if [ "$VARIANT" = fuzz ]; then packages="$packages clang-18 libclang-rt-18-dev"; fi
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $packages >/dev/null
git clone -q --filter=blob:none "$REPO" /w/src
git -C /w/src checkout -q "$COMMIT"
if [ -n "$ENGINE" ]; then
  git -C /w/src checkout -q "$ENGINE" -- src/C++ ':!src/C++/test'
fi
read -ra args <<<"${STRESS_ARGS//\{seed\}/$seed}"
echo "commit=$COMMIT engine=${ENGINE:-same} variant=$VARIANT seed=$seed node=$NODE"
if [ "$VARIANT" = fuzz ]; then
  # Plan item 8.6: libFuzzer with ASan over FixInput's five modes, as the nightly CI job. A crash
  # input is printed in base64, since the pod's disk goes with it.
  flags="-fsanitize=address,fuzzer-no-link -g"
  CC=clang-18 CXX=clang++-18 cmake -S /w/src -B /w/build -DCMAKE_BUILD_TYPE=Debug -DKRX_FUZZ=ON \
    -DQUICKFIX_EXAMPLES=OFF "-DCMAKE_CXX_FLAGS=$flags" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address \
    -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address >/w/build.log 2>&1 \
    || { tail -50 /w/build.log; exit 2; }
  cmake --build /w/build --target krx_fuzz --parallel "$(nproc)" >>/w/build.log 2>&1 \
    || { tail -50 /w/build.log; exit 2; }
  mkdir -p /w/corpus /w/crashes
  rc=0
  /w/build/src/C++/test/krx/krx_fuzz "${args[@]}" -seed="$seed" -artifact_prefix=/w/crashes/ \
    /w/corpus /w/src/test/krx/corpus || rc=$?
  for input in /w/crashes/*; do
    if [ -f "$input" ]; then echo "CRASH INPUT $input: $(base64 -w0 "$input")"; fi
  done
  if [ "$rc" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FUZZ EXIT $rc"; fi
  exit "$rc"
fi
case "$VARIANT" in
  asan) flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
  tsan) flags="-fsanitize=thread -fno-omit-frame-pointer" ;;
  *) flags="" ;;
esac
build_type=Debug
if [ "$VARIANT" = release ]; then build_type=Release; fi
cmake -S /w/src -B /w/build -DCMAKE_BUILD_TYPE="$build_type" -DQUICKFIX_EXAMPLES=OFF \
  -DQUICKFIX_SHARED_LIBS=OFF "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$flags" \
  >/w/build.log 2>&1 || { tail -50 /w/build.log; exit 2; }
cmake --build /w/build --target krx_stress --parallel "$(nproc)" >>/w/build.log 2>&1 \
  || { tail -50 /w/build.log; exit 2; }
rc=0
/w/build/src/C++/test/krx/krx_stress "${args[@]}" >/w/out.log 2>&1 &
pid=$!
tail -n +1 -f --pid="$pid" /w/out.log &
# A hang or a stalled settle (with --hang-grace): every thread's stack, once.
dumped=""
while kill -0 "$pid" 2>/dev/null; do
  if [ -z "$dumped" ] && grep -qE '\*\*\* (HANG|STALLED)' /w/out.log; then
    gdb -p "$pid" -batch -ex 'thread apply all bt 25' 2>&1 | grep -E '^Thread|^#' || true
    dumped=yes
  fi
  sleep 2
done
wait "$pid" || rc=$?
sleep 1
# A failed run with --event-log or --file-log: each session's last events (the pod's disk goes).
if [ "$rc" != 0 ] && [ -d "${args[5]:-/nonexistent}" ]; then
  find "${args[5]}" -name '*.event.current.log' | sort | while read -r log; do
    echo "== $log"
    tail -n 150 "$log"
  done
fi
exit "$rc"
SCRIPT
}

# The TSan runs' only privileged step: TSan needs fewer ASLR bits than Ubuntu's default.
tsan_init() {
  cat <<'YAML'
      initContainers:
        - name: aslr-bits
          image: busybox:1.36
          command: ["sysctl", "-w", "vm.mmap_rnd_bits=28"]
          securityContext:
            privileged: true
          resources:
            requests: {cpu: 10m, memory: 16Mi}
            limits: {cpu: 100m, memory: 32Mi}
YAML
}

job_yaml() {
  local name=$1 variant=$2 runs=$3 parallel=$4 hours=$5 commit=$6 stress_args=$7 script
  script=$(pod_script)
  script=${script//$'\n'/$'\n'              }
  cat <<YAML
apiVersion: batch/v1
kind: Job
metadata:
  name: $name
  namespace: $NAMESPACE
  labels: {app: krx-stress, variant: $variant}
spec:
  completionMode: Indexed
  completions: $runs
  parallelism: $parallel
  backoffLimitPerIndex: 0
  ttlSecondsAfterFinished: 604800
  template:
    metadata:
      labels: {app: krx-stress, variant: $variant}
    spec:
      restartPolicy: Never
      activeDeadlineSeconds: $((hours * 3600 + 1800))
YAML
  if [[ $variant == tsan ]]; then tsan_init; fi
  cat <<YAML
      containers:
        - name: stress
          image: ubuntu:24.04
          command: ["bash", "-c"]
          args:
            - |
              $script
          env:
            - {name: REPO, value: "$REPO"}
            - {name: COMMIT, value: "$commit"}
            - {name: ENGINE, value: "$ENGINE"}
            - {name: VARIANT, value: "$variant"}
            - {name: STRESS_ARGS, value: "$stress_args"}
            - {name: ASAN_OPTIONS, value: "detect_leaks=1:detect_stack_use_after_return=1"}
            - {name: LSAN_OPTIONS, value: "verbosity=1:log_threads=1"}
            - {name: UBSAN_OPTIONS, value: "print_stacktrace=1:halt_on_error=1"}
            - {name: TSAN_OPTIONS, value: "halt_on_error=1:second_deadlock_stack=1"}
            - name: NODE
              valueFrom: {fieldRef: {fieldPath: spec.nodeName}}
          resources:
            requests: {cpu: "$CPU", memory: 3Gi, ephemeral-storage: 4Gi}
            limits: {cpu: "$((CPU * 2))", memory: 6Gi, ephemeral-storage: 8Gi}
          securityContext:
            capabilities: {add: [SYS_PTRACE]}  # gdb takes the stacks of a hang (Yama)
          volumeMounts:
            - {name: work, mountPath: /w}
      volumes:
        - name: work
          emptyDir: {}
YAML
}

start() {
  [[ $# -ge 5 ]] || die "start NAME VARIANT RUNS PARALLEL [--hours H] [--engine SHA] [--cpu N] -- ARGS..."
  local name=$1 variant=$2 runs=$3 parallel=$4 hours=4
  shift 4
  while [[ ${1:-} == --hours || ${1:-} == --engine || ${1:-} == --cpu ]]; do
    case "$1" in
      --hours) hours=$2 ;;
      --engine) ENGINE=$2 ;;
      *) CPU=$2 ;;
    esac
    shift 2
  done
  [[ ${1:-} == -- ]] || die "missing -- before the krx_stress arguments"
  shift
  case "$variant" in release | asan | tsan | fuzz) ;; *) die "variant: release, asan, tsan or fuzz" ;; esac
  local commit pushed manifest
  commit=$(git rev-parse HEAD)
  pushed=$(git branch -r --contains "$commit")
  [[ -n $pushed ]] || die "HEAD $commit is not pushed"
  manifest=$(job_yaml "$name" "$variant" "$runs" "$parallel" "$hours" "$commit" "$*")
  kubectl apply -f - <<<"$manifest"
}

results() {
  [[ $# -eq 1 ]] || die "results NAME"
  local pass=0 total=0 pods pod log verdict
  pods=$(kubectl -n "$NAMESPACE" get pods -l "job-name=$1" -o name)
  pods=$(sort -V <<<"$pods")
  while read -r pod; do
    [[ -n $pod ]] || continue
    log=$(kubectl -n "$NAMESPACE" logs "$pod" 2>/dev/null || true)
    verdict=$(grep -E '^(commit=|RESULT|orders|echoes|settle:|\*\*\* HANG)|SUMMARY: ' <<<"$log" || true)
    echo "${pod#pod/}: ${verdict:-running or no log}" | tr '\n' ' '
    echo
    total=$((total + 1))
    if [[ $verdict == *"RESULT: PASS"* ]]; then pass=$((pass + 1)); fi
  done <<<"$pods"
  echo "$1: $pass of $total PASS"
}

case "${1:-}" in
  setup) kubectl apply -f "$HERE/namespace.yaml" ;;
  start) shift && start "$@" ;;
  results) shift && results "$@" ;;
  delete) kubectl -n "$NAMESPACE" delete job "${2:?NAME}" ;;
  *) die "setup | start | results | delete (see the top of this file)" ;;
esac
