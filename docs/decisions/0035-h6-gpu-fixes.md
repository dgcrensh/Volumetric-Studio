# 0035: H6, the GPU module's concurrency and GL safety

Nov 22, 2026 · plan `docs/plans/H-hardening.md`, phase H6

## Context

H6 fixes `vs_gpu`'s threading, shutdown and GL defects from the audit and
verifies on real hardware what H1 (the `vs_app_core` / `vs_app_ui` split),
H3 (the boundary distance's leaf order and `atan2`) and H5 (three Qt edits
never compiled) changed without it. It ran on Dallas's desktop (RTX 2070
Super, driver 616.87, 12 threads), which stands in for the VM the plan
named. Fields stay FP64 and CPU == GPU bit for bit; no golden value, cache
key or `kEngineVersion` changes.

## Verified first

- **`windows-msvc-app` on `main`** (after PRs 35 to 42) builds with no
  warnings: H1's split and H5's edits to `main_window.cpp`,
  `node_editor.cpp` and `sketch_mode.cpp` compile. `vs_gpu_tests` (9
  cases), `vs_app_core_tests` (49) and `vs_app_tests` (37) passed, except
  "Each kind of scene item is visible on its own", one of H7's two known
  flaky tests (screen grab against off-screen render).
- **H3's distance changes on the GPU:** `self_check()`,
  `meshing_self_check()` and the engine hash comparisons pass, so the
  sorted leaves and the uploaded pseudonormals reach the shaders as the
  CPU uses them. `closest_on_triangle()` got no `0/0` guard in H3
  (degenerate triangles are dropped when the BVH is built), so there was
  nothing to mirror in `glsl.cpp`.

## Decisions

### The device thread (`device.cpp`)

- **`run()`** shares a heap waiter (mutex, condition variable, done flag,
  exception) with the job and notifies while holding the waiter's mutex.
  Before, the job set `done` under a stack mutex, released it, then
  notified a stack condition variable: a caller woken in between (or
  spuriously) returned and freed both while the device thread still used
  them.
- **Exceptions** a job throws are caught on the device thread and
  rethrown to its caller; `loop()` also catches anything else, so the
  thread never dies (it used to call `std::terminate()`).
- **`run()` returns `bool`**: false, with the job not run, when there is
  no device or it is stopping (checked under the queue's mutex, so no job
  is queued that no one will run). `sample_gpu()` and `unmerged_net_gpu()`
  turn that into an error, never an empty result.
- **Programs** are capped at `Device::kMaxPrograms` (64), least recently
  used out. A job takes every program it needs before it dispatches, and
  none takes more than four, so eviction never deletes one in use.

### Shutdown

`gpu::shutdown()` uninstalls the backends and drops the process's
reference to the device. `shared_device()` now hands out
`std::shared_ptr<Device>`, held for the length of a call, so a call still
running on another thread finishes and the last holder closes the device
(joins its thread, deletes the context, frees `opengl32.dll`). Later calls
find no device ("the GPU was shut down"), and `install()` installs
nothing if a shutdown came during its checks. The process-wide state is
never destroyed (`new`, not a function-local static), so nothing GL is
torn down by static destructors during `atexit`, when other DLLs may
already be gone. `vs_app` calls `shutdown()` after its window and session
are destroyed and the start-up check is joined; `vstudio-cli` calls it on
every return from `main()` after the command line is read.

### GL safety

- **`Buffer`** of `0 < bytes < 16` copies the data into a zeroed 16-byte
  block; before, GL read 16 bytes from the caller's shorter data. Buffers
  given data (read-only for shaders) are `GL_STATIC_DRAW`, the rest
  `GL_DYNAMIC_READ`.
- **No silent zeros.** `gl_errors()` drains `glGetError()`.
  `sample_gpu()` checks after allocating and after every slab's dispatch
  and read-back; `unmerged_net_gpu()` after every allocation and at the
  end. Any error fails the call ("OpenGL error 0x0505 ..."), so the
  backend returns false and the CPU samples instead.
- **Storage block size.** The device reads
  `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` once. `sample_gpu()` refuses a field
  whose data buffers exceed it and sizes slabs to it (and to 32-bit sample
  indices); meshing takes the smaller of it and its own 1 GiB cap.
- **2D dispatches.** Field shaders index samples by
  `gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * 64) + x`, and
  `dispatch_2d()` splits a slab into at most 65,535 groups a side. A slice
  wider than 65,535 × 64 samples (2,048 × 2,048 and up) needed more groups
  than GL promises in one dimension. NVIDIA allows 2^31 − 1 in x, so on
  this desktop the old dispatch also worked and the new wide-grid test
  passes either way; on a driver at the minimum it would have failed
  silently, and now it can't. Meshing already dispatched in chunks with a
  base offset and is unchanged.
- **The hidden window** is a message-only window (`HWND_MESSAGE`): never
  shown and left out of broadcasts, which the device thread (it pumps no
  messages) would otherwise hold up, hanging whoever broadcast
  `WM_SETTINGCHANGE`. If a driver gives a message-only window no pixel
  format, the old hidden top-level window is the fallback.
- **`wglCreateContextAttribsARB`**, when the driver has it, swaps the
  legacy context for a 4.3 core one, so a driver whose legacy context
  stops short of 4.3 still offers compute; otherwise the legacy context
  stays. NVIDIA now reports "4.3.0 NVIDIA 616.87" (the version asked
  for) where the legacy context said 4.6.0; nothing here needs more than
  4.3, and every check and hash is the same.

### Calibration and installs

- `best_of()` times a stage as infinite when any GPU run returned nothing,
  and `gpu_wins()` gives the GPU a stage only on a finite, faster time. A
  GPU stage that failed quickly used to look fastest and win.
- `install()` holds an install mutex for its whole run, so two callers
  (the app's start-up thread and a test, say) never check and calibrate
  at once; the second gets the first's status. `sample_gpu()` reads
  `status.why` under the state mutex.

### Shaders (`glsl.cpp`)

- **`sqrt_rn()`** scales any `0 < x < 2^-900` by `2^256`, takes the
  correctly rounded root and scales it by `2^-128`; both scalings are
  exact. Below that, the residual test's products underflow and the
  hardware root of a subnormal may be flushed. On this GPU the old code
  gave wrong roots for subnormal and tiny inputs (the arithmetic test now
  covers every exponent from `2^-1074` to `2^-880` and fails on the old
  shader); normal inputs take the old path unchanged, so no field value
  changes.
- **`MESH{}` defines** come from the translator's mesh count
  (`FieldProgram::meshes`), not from searching the source for each name.

### Dropped

- **TPMS argument reduction** (optional in the plan): within the ±1,000 mm
  working space `sin_det()`'s argument stays far below where reduction
  loses precision, and no test showed a real case, so field values stay
  as they are.

## Results

`bench-gyroid --size 100 --cell 0.5` on the desktop (12 threads; the
same mesh, `e5cdb4b8ba77111c`, every way):

| | CPU only | `--gpu` | `--gpu=force` | decision 0019, `--gpu=force` |
| --- | --- | --- | --- | --- |
| sample | 0.84 s | 0.86 s | 1.17–1.63 s | 1.0 s |
| clip | | 0.38 s | 0.25 s | 0.25 s |
| unmerged net | 1.41 s | 1.67 s | 3.99 s | 3.9 s |
| merge | 2.92 s | 3.10 s | 2.96 s | 3.1 s |
| **engine** | **9.6 s** | **10.0 s** | **12.3 s** | 13.2 s |

Plain `--gpu` calibrates in 2.8 s (sampling GPU 197 ms against CPU 122,
clip 198 against 112, unmerged net 82 against 22; decision 0019 had 248,
245 and 98 on the GPU) and keeps every stage on the CPU, as before: the
2070 Super's doubles run at 1/32 rate. Both runs are clean, and so are
`vstudio-cli selftest` and `vs_app` opening a procedural project with the
GPU installed and exiting through `shutdown()`. Forced, the engine is
faster than 0019's, but forced sampling of the full grid took 1.17 to
1.63 s over three runs against 0019's 1.0 s, while the calibration's
sampling got faster (197 ms against 248). The cause isn't isolated (the
2D dispatch, the per-slab error check and the core context are the
candidates); it doesn't touch a default run here, where sampling stays on
the CPU, and is worth a look when the L4 VM, where the GPU wins sampling,
is next measured. The full `windows-msvc-app` CTest run is 724
tests; the one failure is "Each kind of scene item is visible on its own",
which fails on `main` here too and is H7's.

## Tests

- `libs/vs_gpu/tests/test_device.cpp`: eight threads running 2,000 jobs
  each, a throwing job rethrown with the device still working, a 4-byte
  buffer reading back its data and zeros, GL errors reported, the program
  cap (least recently used out, a busy program kept, an evicted one
  compiling again), 2D dispatch sizes, a 2,100 × 2,100 slice against the
  CPU, twelve boundary meshes defined and sampled, the calibration rule,
  two concurrent installs, and (in its own CTest process, tag
  `[.shutdown]`) shutdown with a call still holding the device.
- `test_arithmetic.cpp` uses the shaders' own `sqrt_rn()` (it had a copy)
  and adds subnormals and tiny normals.
