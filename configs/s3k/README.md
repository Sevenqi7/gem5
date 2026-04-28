# S3K on gem5

This directory contains the gem5-side setup used for running S3K and the CLB
prototype.

## Directory Layout

- `cheshire/`
  Clean Cheshire-based S3K configuration. This path does not instantiate the
  CLB SimObject and is intended for baseline runs.
- `cheshire_clb/`
  Cheshire-based S3K configuration with the CLB SimObject instantiated. Use
  this for CLB-enabled experiments.

The `cheshire/` and `cheshire_clb/` directories may also contain convenience
ELF/bin files used for quick local runs.

## Main Configurations

- `cheshire/cheshire_oldschool.py`
  Baseline full-system setup for S3K without CLB.
- `cheshire_clb/cheshire_oldschool.py`
  Full-system setup for S3K with CLB support.
- `cheshire_clb/cheshire_minor.py`
  MinorCPU-based full-system setup for CLB timing experiments.

The `oldschool` suffix means these configurations use the traditional gem5
Python-script setup style rather than the newer gem5 standard library.

Both scripts support overriding the loaded application image and kernel ELF via
environment variables:

- `S3K_APP_PATH`
- `S3K_KERNEL_PATH`

## Typical Usage

Run the clean baseline:

```bash
build/RISCV/gem5.opt configs/s3k/cheshire/cheshire_oldschool.py
```

Run the CLB-enabled setup:

```bash
build/RISCV/gem5.opt configs/s3k/cheshire_clb/cheshire_oldschool.py
```

Run with explicit kernel/app overrides:

```bash
S3K_APP_PATH=/path/to/app.bin \
S3K_KERNEL_PATH=/path/to/s3k.elf \
build/RISCV/gem5.opt \
  -d m5out/s3k_run \
  configs/s3k/cheshire_clb/cheshire_oldschool.py
```

Terminal output is written to:

```text
m5out/<run-dir>/system.platform.terminal
```

## Notes

- `cheshire/cheshire_oldschool.py` and
  `cheshire_clb/cheshire_oldschool.py` use `RiscvTimingSimpleCPU`.
- `cheshire_clb/cheshire_minor.py` uses `RiscvMinorCPU`.
- The S3K environment here assumes physical addressing only; no virtual memory
  translation is modeled on the software side.
- The clean and CLB configurations are kept separate on purpose so baseline and
  CLB experiments can be compared without reusing the same kernel/config by
  accident.

## MinorCPU CLB Timing Status

The current MinorCPU integration is focused on moving the time cost of
`uclb.get` and `uclb.delete` out of the old execute-time fixed stall model and
into the Minor pipeline itself.

### Implemented

- Ordinary load/store CLB access-control checks are modeled before LSQ issue.
  The CLB lookup delay is controlled by `clb_lookup_latency`.
- `uclb.get` performs a pre-issue CLB probe in Minor's Execute stage.
  On a CLB hit, it takes a local no-memory-access path.
  On a CLB miss, it issues a real capability-table read to the memory system.
- `uclb.delete` is modeled as a probe micro-op followed by a commit micro-op.
  The probe performs a pre-issue CLB lookup in Minor's Execute stage.
  A hit uses a local probe path and the commit issues a real capability-table
  writeback.
  A miss first issues a real capability-table read, then the commit issues a
  real capability-table writeback when deletion succeeds.

### Current Limits

- The `uclb.get` and `uclb.delete` hit paths do not send real memory requests,
  but they are still modeled as MemRef-like local accesses.
  This means their absolute hit latency on Minor includes local-access and LSQ
  plumbing overhead in addition to the CLB lookup itself.
- The implemented Minor timing model is therefore most trustworthy for:
  relative sensitivity to `clb_lookup_latency`,
  hit-versus-miss path separation,
  and the effect of real capability-table traffic on miss/writeback paths.
- The current model is less suitable for claiming a precise hardware-level
  absolute cycle count for the CLB-hit fast path.
- `uclb.revoke` is not yet integrated into Minor as real memory traffic.
  Its timing is still modeled separately from the `uclb.get`/`uclb.delete`
  Minor pipeline work.
