# bench — persistent QNN inference

Proves the real-time premise: load the context binary **once**, keep the graph
resident, and execute per frame.

Spawning `qnn-net-run` per frame costs ~594 ms wall clock against ~30 ms of
compute, capping throughput below 2 FPS. Holding the graph open removes that.

## Measured (Hexagon V73, FP16)

| imgsz | mean | p95 | FPS | one-time load |
|---|---|---|---|---|
| 384 | 11.9 ms | 13.1 ms | **84** | ~172 ms |
| 512 | 20.0 ms | 21.3 ms | **50** | ~161 ms |
| 640 | 32.2 ms | 34.5 ms | **31** | ~185 ms |
| 768 | 43.9 ms | 45.7 ms | 23 | ~185 ms |

Correctness verified at 640 against the ONNX reference: corr 0.99976,
MAE 0.0130 m — identical to the `qnn-net-run` path.

These figures include the per-frame **float32 → fp16 input conversion** done on
the CPU, so they are closer to real pipeline cost than the pure accelerator
time (`qnn-net-run` profiling reports ~28.6 ms of accelerator work at 640).

Note the first inference costs an extra ~3-4 ms (HVX/HMX power-on); the tool
discards the first 10 iterations.

## Build

```bash
./build.sh [board-ip]      # copies the source and compiles natively on the board
```

The program reads graph name and tensor shapes from the binary itself via the
QNN System API, so it works with any of the built sizes without recompiling.
