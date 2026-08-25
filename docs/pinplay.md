# Deterministic exec-driven runs with PinPlay

The exec-driven frontend (`pin_exec.so`) executes the *real* program live under
PIN. That execution depends on the host kernel, CPU, and libraries, so the same
simulation launched on two different machines can fast-forward to different
program points and produce different results (the trace frontends do not have
this problem — a trace file replays identically everywhere).

PinPlay removes that host dependence: record a **pinball** once (a checkpoint of
the region plus an injection log of every non-deterministic input — syscall
results, `RDTSC`/`CPUID`, signals, etc.), then **replay** it. On replay PIN
re-injects the recorded values instead of asking the host, so the exec-driven
frontend re-executes the identical instruction and data stream on any machine,
while keeping its true wrong-path re-execution.

## Requirements

PinPlay is **not** part of the stock PIN 3.15 kit. Point `PIN_ROOT` at a
pinplay-capable kit (an SDE kit's `pinkit`, or a PIN "PinPlay/DrDebug" kit) that
provides `extras/pinplay/{include,lib,lib-ext}`.

## Building

PinPlay support is opt-in so the standard PIN kit keeps building unchanged:

```sh
ENABLE_PINPLAY=1 make opt        # from src/, with PIN_ROOT set to a pinplay kit
```

Without `ENABLE_PINPLAY`, `pin_exec.so` builds exactly as before (no pinplay).

## Recording a pinball

Run the exec-driven tool with the logger active over the region of interest:

```sh
$PIN_ROOT/pin -t src/pin/pin_exec/obj-intel64/pin_exec.so \
    -log -log:basename /path/to/region \
    -- <program and args>
```

Use the pinplay controller knobs (`-log:controller_*`) or a SimPoint pcregions
file to bound the region. This is done once, on any machine.

## Replaying a pinball in Scarab

Drive the exec-driven frontend from the pinball instead of a live program. The
replayer args go through `scarab_launch.py`'s existing `--pintool_args`:

```sh
bin/scarab_launch.py --program "$PIN_ROOT/extras/pinplay/bin/intel64/nullapp" \
    --pintool_args "-replay -replay:basename /path/to/region" \
    --scarab_args "..."
```

Every replay reproduces the identical run regardless of host, so `value` vs
`perfect` (or any A/B) differ only in the microarchitecture under study, not in
the fast-forward landing point.
