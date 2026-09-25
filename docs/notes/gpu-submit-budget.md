# GPU submit budget

A submission that runs past the driver's watchdog loses the device:
`VK_ERROR_DEVICE_LOST` (-4), and on the inference path a segfault inside the
driver right after it. The watchdog is 2 s under Windows TDR, and 2 s for
amdgpu on Linux 7.0 (`modinfo amdgpu | grep lockup_timeout`; older kernels
used 10 s). Work sized on a discrete GPU crosses that on an integrated one:
a 2-CU RADV iGPU (Ryzen 7000 "Raphael") runs these paths 20-140x slower
than an RTX 5070.

`core/SubmitBudget.h` is the one mechanism. Each caller counts work in its
own units, times its submits, and sizes the next one to about 0.25 s of GPU
time (8x headroom). A fast GPU measures a large rate and keeps its old
batching; only the first submits of a process are smaller.
`SS_SUBMIT_BUDGET_MS` overrides the target -- raise it to reproduce the old
behaviour, lower it to exercise the slicing on a fast GPU (results are
bit-identical except where noted below).

| path | unit | what gets split | measured on the iGPU before |
|---|---|---|---|
| SfM brute-force matcher (`sfm/feature/Matcher.h`) | descriptor words multiplied | pairs per chunk | 64 cross-checked 8192^2 pairs = 2.0 s, one submit |
| SIFT pyramid (`sfm/feature/Sift.h`) | pixels x taps | blur steps per submit | 0.83 s at 3200 px, >2 s at 5000 px |
| SIFT orient / descriptor | keypoints | keypoint ranges | 392 / 222 ms, one dispatch each |
| inference stream (`nn/vk/Stream.cpp`) | FLOPs, per-op estimate | submits by cost; GEMM rows and attention queries/batches past the cap | SAM 3 memory attention ~190 GFLOP in one dispatch |
| meshing cull / occupancy / bisection / color | pairs or points | launch ranges, capped at the old sizes | cull: 0.11 s per launch on an RTX 5070 |

The inference stream submits asynchronously, so it brackets every command
buffer with two timestamps and reads them when the ring slot comes round
again; until then it assumes 50 GFLOP/s. Attention cost is weighted 2x,
because flash attention reaches half the efficiency of the GEMMs the rate
is mostly learned from.

Slicing attention by query block can change the key-split decision per
slice, so outputs move by float-reordering noise (a handful of mask-edge
pixels in `spirula geometry`); everything else is bit-identical.

Not covered: GPU bundle adjustment records up to `cg_max_iters` PCG
iterations (or a whole dense Cholesky) per submit, but it only runs on a
device with fp64 atomic add, which neither NVIDIA nor RADV consumer parts
expose. The splat viewer's forward pass is the training forward and was
left alone.
