# Kirby compute compilation investigation (build 160)

Based on `eden_log(20261112-105649).txt` and `pipeline-builds.txt`, from
`kirby-build160-stack-fix-build-6`, Ryzen 7 3700X / NVIDIA 610.88.

## Recorded evidence

The driver-build trace has 896 compute builds, each with a different shader hash,
and 708 graphics builds. Eleven compute builds exceed one second. Build times are
wall times on concurrent threads: they must not be summed into elapsed gameplay time.
The old compute trace misleadingly calls the shader hash `pipeline=`; it does not
include shared-memory or workgroup variants.

| Compute shader hash | Recorded driver-wrapper wall time (ms) |
| --- | ---: |
| f67f71955efe09fe | 3897.6 |
| 916b2f0e9d3f6c62 | 3571.9 |
| 46fa37b7f6fc6fb1 | 3560.5 |
| dbc22c81c5b5e5f0 | 2514.0 |
| 4b13465509ee1703 | 1830.6 |
| 8e5ce3961bf06fa8 | 1655.7 |
| 3a9067a417da8340 | 1640.1 |
| b6fee8a7cdaf01f1 | 1553.9 |
| 971ed6108a0a8520 | 1264.7 |
| 656af14f2e3820f9 | 1235.3 |
| cbffeba7e6d02967 | 1082.3 |

Frame 2521 took 4809.0 ms and recorded 4702.3 ms in pipeline waits plus
1131.1 ms in compute-pipeline preparation (nine builds). Frame 2607 took
5148.0 ms and recorded 4006.6 ms waiting plus 1708.3 ms compute preparation
(six builds). Thread intervals overlap. The 999.8 ms preparation of
33acb3ded21e7a4b is distinct from the driver-wrapper intervals above.

The longest frame, 1995 (5654.4 ms), recorded no builds or pipeline waits:
texture and buffer work also needs separate investigation. Out-of-bounds sparse
memory reports and macro 0x1d2 instruction-limit errors are unresolved correctness
issues; this optimization does not fix them.

There are no logged stale-null compute rebuilds in this session. The cache reports
1289 native programs from records, 91 lowered without a record, and zero stale
program records. This does not prove stable keys across later launches.

## Where execution waits

`PipelineCache::CurrentComputePipeline` looks up a key containing shader hash,
shared-memory allocation, and workgroup dimensions; the key has a unique object
representation (no padding ambiguity). CFG construction and `TranslateProgram`
currently execute synchronously on the GPU emulation/back thread. Eligible IR is
owned by a queued CPU job that emits SPIR-V and creates its shader module. A driver
job then creates the Vulkan pipeline.

`ComputePipeline::Configure` records a wait into the Vulkan command worker before
recording the dispatch's use of the pipeline. The worker invokes
`ShaderJobScheduler::RunOrWait`: it can claim queued CPU/driver stages itself, or
wait for an already running worker. It then waits on `build_condvar` until built.
Thus a build is asynchronous to submission, but its first dependent dispatch blocks
command recording. Allowing that dispatch or later dependent commands past the wait
would require a different command/dependency design; skipping it can lose writes.
No job counts, waits, barriers, or dispatch order are changed here.

## Added phase measurements

With `EDEN_NX2_PIPELINE_BUILD_TRACE=<file>`, records now include a monotonic timestamp,
thread identity, logical pipeline key, shader hash, and separate phases:

- compute-cfg and compute-translate: separate CFG and translation times, with IR size,
  storage-buffer count, shared-memory size and workgroup dimensions.
- compute-spirv: emission time, emitted word count, original and submitted SPIR-V hashes,
  whether shared calls are enabled, functions marked and transformation time.
- compute-module: shader-module creation.
- compute driver-wrapper: existing BEGIN/END compute records, now keyed by full pipeline key.
- compute-driver-api: inner Vulkan pipeline creation when using pipeline binaries; wrapper
  minus API time includes binary key, store, capture and other overhead. Without the
  binary layer the existing wrapper surrounds direct logical-device creation.
- compute-wait: queued command worker's pipeline wait and before/after job state.
  Compare its thread ID to the driver-build and emission thread IDs to identify which
  thread did the work. Job state is a snapshot, not proof of the eventual builder.
- compute-cache USE / REBUILD: first-use source and mapped-null-pointer rebuild reason,
  with native key and variant identity.
- compute-job SUBMIT: CPU-stage presence and memory-budget estimates.

The completed trace is appended to, not truncated; use separate output files for runs.
Timings are wall times, including driver-internal contention, not CPU-exclusive times.
Job states: 0 queued, 1 CPU running, 2 driver queued, 3 driver running, 4 done,
5 cancelled, 6 failed; -1 means no scheduler job.

## One opt-in optimization

`EDEN_NX2_COMPUTE_SHARED_CALLS=1` adapts the compute portion of upstream
[Lichii PR 1166](https://github.com/zortos293/Lichii/pull/1166), by daniel-lynch and
PantACRO4life. It sets the SPIR-V DontInline hint on non-entry functions with at
least 128 instructions and two call sites. Only function control words change;
the driver may honor or ignore the hint. The shader instructions and dispatch order
remain intact. GPU runtime cost can increase because calls may replace inlining.
Graphics stages are unaffected. The switch defaults off for a baseline run.

The upstream measurements on other NVIDIA hardware show lower compilation time,
which motivates this experiment; no Kirby speedup has yet been measured. The
submitted SPIR-V determines Vulkan driver cache identity, and the binary store
checks the driver pipeline key before reuse. Guest pipeline keys are preserved.
Shader precache markers do not promise warm-up of the experimental variant: its
first driver build may still occur at use.

## Reproduce

Open PowerShell in the extracted package folder. Baseline:

```powershell
$env:EDEN_NX2_STUTTER_TRACE="1"
$env:EDEN_NX2_GPU_PROFILE="1"
$env:EDEN_NX2_PIPELINE_BUILD_TRACE="$PWD\compute-baseline.txt"
$env:EDEN_NX2_COMPUTE_SHARED_CALLS="0"
.\lichii.exe
```

Exit fully, preserve the log, then run the same route with:

```powershell
$env:EDEN_NX2_PIPELINE_BUILD_TRACE="$PWD\compute-shared-calls.txt"
$env:EDEN_NX2_COMPUTE_SHARED_CALLS="1"
.\lichii.exe
```

Keep settings and route fixed. Compare matched shader and variant hashes, marked
functions, phase times, frame waits and image correctness. Cache hits must be
separated from fresh compiles; comparing a warm run with a cold run does not measure
compiler speed. Preserve both traces and session logs. No measured improvement is
claimed until those runs are available.

## Validation

Six focused tests compile the production transformation helper and cover thresholds,
entry points, forced Inline replacement, malformed instructions, missing function
end, and graphics execution-model handling of the generic helper. They passed with
GCC and address/undefined-behavior sanitizers; leak checking was disabled because
LeakSanitizer cannot run under this environment's ptrace. A concurrent trace smoke test passed 800 paired events across eight threads,
without interleaved lines. Full Vulkan and Windows runtime validation requires CI
and the owner's A/B runs.


## November 12 follow-up measurements

The appended `compute-baseline.txt` contains multiple launches. Isolating the
session starting at monotonic 129192.125 seconds (matching the boot marker in
`eden_log(20261112-194725).txt`) gives 892 startup compute translations and 85
runtime compute translations. No full compute key appears twice within this
session, and no mapped-null-pointer rebuild is logged.

Runtime shader `33acb3ded21e7a4b` takes 771.4 ms in TranslateProgram and 4.7 ms
in the pipeline creation wrapper. Shaders `511959c65c8cb951` and
`5154b8682a422996` take 475.3 and 435.1 ms in translation. All runtime translations
use one thread, while startup translation is parallel. The 977 driver creation
wrappers peak at 8.0 ms; the driver cache may already be warm. These measurements
establish synchronous translation as a material runtime bottleneck in this run,
without invalidating earlier cold driver compilation measurements.

GPU_PROFILE was absent in the successful diagnostic run. Enabling it in a
subsequent comparison coincided with presentation waiting for fence 3494 while
3492 remained completed. This is correlation, not proof that the profiler causes
the hang. Keep EDEN_NX2_GPU_PROFILE unset for the translation investigation.

Per-pass translation events use the same pipeline trace file. Keep shared calls
at zero for the baseline and use the same route/settings. The additional timing
does not change shader passes, compilation thread counts, or command ordering.

Pass events are `END compute-pass ... ms=... pass=<name>` for successful passes
at least 1 ms, or `FAIL compute-pass` when a pass throws. Sub-millisecond success
and begin events are omitted. Timer bodies exclude the trace sink; total translation
time still includes trace overhead. No global/thread-local trace state is used.
Focused production-header checks passed with address/undefined-behavior sanitizers,
including return-value preservation, nested contexts, exception propagation,
throwing sinks, and concurrent context isolation. Windows compilation is checked
by the branch Actions build; game performance still requires the owner run.
