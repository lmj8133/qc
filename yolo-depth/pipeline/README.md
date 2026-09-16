# Real-time monocular depth pipeline (QCS8550 / Hexagon V73)

`depth_cam.c` is the whole pipeline in one C program:

```
V4L2 YUYV capture -> YUY2->RGB -> centre-crop + bilinear scale to SxS
  -> normalize to fp16 NCHW -> QNN graphExecute (resident graph)
  -> turbo colourize -> BGRA -> GStreamer waylandsink
```

The QNN setup is lifted from `../bench/qnn_bench.c`, including the System API
`BinaryInfo`/`GraphInfo` V1/V2/V3 handling (these binaries declare V3; assuming
V1 fails outright). The context binary is loaded **once** and the graph stays
resident — spawning `qnn-net-run` per frame costs ~594 ms of wall clock each.

Everything the per-frame loop touches is allocated before the loop starts. The
loop performs zero `malloc`/`free`.

## Quick start

```bash
./run.sh 384 live                 # best for demos: side by side, full 30 FPS
./run.sh                          # 512px, side by side, 300 frames
./run.sh 512 bench                # headless benchmark
./run.sh 512 live --depth-only    # depth only, no camera pane
```

Usage is `./run.sh [size] [mode] [extra flags...]`, where mode is
`display` (default), `bench` or `live`. **Anything after the mode is passed
straight through to `depth_cam`**, so any flag from the table below works:

```bash
./run.sh 384 live --depth-only --stats-every 60
```

`run.sh` builds on the board, stages the context binary, and runs with the
right environment. `/dev/shm` is a tmpfs, so it re-stages every time rather
than assuming anything survived a reboot.

Override the board address with `BOARD=<ip> ./run.sh`.

### The board's IP moves — it is not a hung board

The address is DHCP-assigned and has changed three times (`192.168.3.63` →
`.80` → `.67`). Each time it looks exactly like a dead board: SSH and `ping`
both time out while the **stale ARP entry still reads `REACHABLE`**, which is
what makes the misdiagnosis so easy. `build.sh` now fails fast with a pointer
instead of hanging.

The default lives in `board.env`, sourced by both scripts, so a move is one
edit. To find the board after it moves, sweep the subnet for its MAC:

```bash
for i in $(seq 1 254); do (ping -c1 -W1 192.168.3.$i >/dev/null 2>&1 &); done
sleep 5; ip neigh show | grep -i a0:36:bc:3c:ab:10
```

Its hostname is `kalama`. Note that `/dev/shm` is a tmpfs, so an IP change that
came with a reboot also means the staged `.bin` is gone — `run.sh` re-stages
every time, but a manual `depth_cam` invocation will not.

## Build (manual)

```bash
./build.sh [board-ip]        # default from board.env
```

Copies the source to `/dev/shm` and compiles natively (the board has gcc):

```bash
gcc -O3 -march=armv8.2-a+fp16 -Wall -Wextra -o depth_cam depth_cam.c \
    -I/opt/qcom/qirp-sdk/include -ldl -lm
```

Stage a context binary once:

```bash
scp ../artifacts/y26n_640_fp16_v73.bin root@192.168.3.67:/dev/shm/
```

## Run

On the board, always source the SDK first:

```bash
source /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1
export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:$LD_LIBRARY_PATH
cd /dev/shm

./depth_cam --model y26n_640_fp16_v73.bin --frames 300 --no-display   # benchmark
./depth_cam --model y26n_640_fp16_v73.bin --frames 300                # with display
```

| Flag | Default | Meaning |
|---|---|---|
| `--model <path>` | *(required)* | QNN context binary |
| `--device <path>` | `/dev/video2` | V4L2 capture device |
| `--size <N>` | 640 | Model input side; **overridden by the binary's own shape** |
| `--frames <N>` | 300 | 0 = run until Ctrl-C |
| `--stats-every <N>` | 30 | Frames between stats lines |
| `--no-display` | off | Headless benchmark |
| `--no-pin` | off | Disable CPU pinning (see below) |
| `--probe-centre` | off | Per-frame median metric depth of a centre patch (see below) |
| `--probe-patch <N>` | 32 | Side of that patch, in model pixels; clamped to the model input |
| `--text-scale <N>` | auto | Pixel size of one font dot in the overlay reading (auto = 2 below 512px, else 3) |
| `--fullscreen` | off | Scale the display to fill the panel |
| `--help` | | Usage with an example |

Exit code 0 on success, non-zero on failure.

## Latency and absolute scale — the two obstacle-avoidance measurements

The platform is ultimately for obstacle avoidance, where the metric that matters
is glass-to-decision latency in metres of real distance, not FPS. The stage
timings above answer neither question, so two measurements exist separately.

### Frame age at dequeue (always reported)

The V4L2 buffer timestamp is on `CLOCK_MONOTONIC`, the same clock `now_ms()`
reads, so subtracting it from the dequeue time gives the age of the frame when
the loop first saw it: sensor readout, the camera's own ISP, USB transfer and
driver buffering. The program prints the driver's timestamp convention once at
startup rather than assuming it:

```
buffer timestamp : monotonic, start-of-exposure
```

**Measured at 384px: 34.23 ms mean (min 32.52, max 37.27).** That is about one
frame period at 30 fps, and it is a fixed cost of the sensor, not a transfer
cost — cutting the USB payload four-fold (640x480 -> 320x240 YUYV) moves it by
about 1%, in the wrong direction. Only a higher-frame-rate sensor shortens it.

**Frame age overlaps `capture`, it does not add to it.** `capture` times a
dequeue of a buffer that was usually already waiting, which is why it reads
~0.04 ms whenever the pipeline is slower than the camera. Do not sum the two.
The summary reports them apart for this reason:

```
frame age at dq  :  34.23 ms   (min 32.52, max 37.27; ...)
glass-to-display :  51.35 ms   (frame age + preprocess + inference + ...)
```

### `--probe-centre` — checking the metric scale against a tape measure

The network's output is metric metres: the head is `Conv -> Clip(-4,5) -> Exp ->
Pow(cal_a) -> Mul(exp(cal_b))`, an exponential metric head, and `colourize()`'s
per-frame min/max normalization only ever writes the BGRA copy — `net_out` is
never rescaled. So the fp16 values are metres and can be compared with a ruler.

What has *not* been established is how accurate those metres are. The 0.9997
correlation quoted elsewhere is FP16 conversion error against the same-size ONNX
reference, not absolute depth accuracy, and NYU val has never been run. Two
specific reasons to expect a bias: the centre crop from 640x480 to 480x480
changes focal-length-in-pixels, which is exactly what monocular metric depth is
conditioned on; and the shipped calibration constants are not identity
(`cal_a = 1.0` but `exp(cal_b) = 0.8237834`).

```bash
./run.sh 384 live --probe-centre --fullscreen --text-scale 6
```

`--fullscreen` matters more than it sounds: the panel is 1920x1080 and the
side-by-side frame is 768x384, so without it the window covers 14% of the screen
and the reading is tiny from across a room. The compositor does the scaling, so
it costs nothing per frame — measured 30.2 FPS either way. `--text-scale` then
sizes the digits; 6 is comfortable at 384px, and a label too wide for its pane
is pulled back inside rather than clipped mid-digit.

Aim the centre of frame at a flat surface, put a tape measure on it, and read
the metre value at 0.5 / 1 / 2 / 4 m. Use four distances rather than one: it
tells you whether the error is an offset, a scale factor or non-linear. A pure
scale factor may be one constant away from being correct.

**With the side-by-side display**, a green box marks the patch on both panes and
the reading is drawn next to it, so you aim and read on the screen without
looking away at an SSH terminal. Both panes are boxed because they answer
different questions: the camera pane shows which real object the box is on, and
the depth pane shows whether that region is one flat surface or straddles a
depth discontinuity. A 32x32 patch is 8.3% of the frame's width at 384px, so a
small aiming error can put it on a doorframe or the floor instead of the wall —
and the reading will still look stable and plausible. That is the mistake the
overlay exists to prevent, and it is why the reading is only trustworthy when
you can see where the box landed.

The glyphs come from a 3x5 bitmap font built into the program; the board has no
font reachable from a plain C program.

**Headless or `--depth-only`** prints a reading every frame instead, since a
tape measure will not hold still for `--stats-every` frames:

```
[    2] centre  5.930 m  (frame age 33.31 ms)
```

There is deliberately no overlay in `--depth-only`: on a bare turbo-coloured
depth map there is no way to tell a wall from a chair in front of it, so a box
there would invite exactly the error above. The program says so once and carries
on — the readings are still valid, they just cannot be aimed by eye.

Note that the display costs ~8 ms of producer-blocking time, so the latency
figures printed while the overlay is up are not the shipping config's. The depth
readings are unaffected: they come from the same `net_out` either way.

The patch is a median, not a mean, so a few stray pixels at a depth
discontinuity do not drag the reading off the surface being measured.

## Measured performance — 640x640 model, 300 frames

Headless (`--no-display`):

```
--- summary (299 frames timed, frame 0 discarded as warm-up) ---
warm-up frame 0  : 276.8 ms
capture          :   0.04 ms
preprocess       :  11.64 ms   (YUY2->RGB, crop 480x480, scale 640x640, fp16 NCHW)
inference        :  31.03 ms   (QNN HTP, resident graph)
colourize        :   2.31 ms
display          :   0.00 ms   (disabled)
sum of stages    :  45.02 ms
end-to-end       :  45.03 ms   -> 22.2 FPS
```

With display (`waylandsink`):

```
capture          :   0.04 ms
preprocess       :   8.11 ms
inference        :  30.76 ms
colourize        :   2.17 ms
display          :   8.26 ms   (waylandsink)
end-to-end       :  49.35 ms   -> 20.3 FPS
```

One-time context load: 140-170 ms. Inference matches the standalone benchmark
(32.2 ms at 640), so nothing in the pipeline is stealing NPU time.

Display costs ~8 ms/frame of *producer-blocking* time: the `write()` to the
GStreamer child blocks once the sink's queue is full, so it doubles as the
pacing mechanism. It is real cost, not measurement overhead.

## Side-by-side view (default)

The display shows **camera on the left, depth on the right**, each SxS, in one
2S x S frame. The camera pane is the exact square the network saw (the 480x480
centre crop), nearest-scaled so the two panes align pixel-for-pixel.

This is for showing the thing to other people: a warm blob means nothing until
you can see it is a person. Use `--depth-only` for the old depth-only view.

Costs about 6.5 ms/frame of extra sink time, because the composited frame is
twice as wide:

| size | display pane | disp ms | **FPS** | note |
|---|---|---|---|---|
| **384** | 768 x 384 | 8.7 | **30.0** | camera-limited — full rate |
| 512 | 1024 x 512 | 12.7 | 25.7 | |
| 512 `--depth-only` | 512 x 512 | 5.9 | 29.5 | |

**For a demo, use 384 side-by-side**: it runs at the full 30 fps and the pane is
big enough to read on a monitor. 512 side-by-side looks slightly smoother in
depth detail but drops to ~26 fps.

Note the cost is the sink's blocking write, not colour conversion — `waylandsink`
takes BGRA natively and dropping `videoconvert` changed nothing (measured).
Removing it anyway, since it was a pure no-op in the path.

## Stopping it

Ctrl-C. `run.sh` requests a TTY (`ssh -tt`) so the interrupt is delivered to the
remote process group, and `depth_cam` traps `SIGINT`/`SIGTERM` to shut down
cleanly. A plain `ssh` without a TTY does NOT do this: Ctrl-C kills only the
local client and leaves `depth_cam` running on the board, holding the camera.

Belt and braces, because a dropped link delivers no signal at all:

- the remote shell traps `EXIT` and kills its children, so the `gst-launch`
  child dies with it;
- `run.sh` traps `EXIT`/`INT`/`TERM` locally and issues a remote `pkill` as a
  final sweep.

If something ever does survive (say the board was power-cycled mid-run):

```bash
ssh root@192.168.3.67 'pkill -x depth_cam; pkill -f gst-launch-1.0'
```

Symptom to recognise: a later run fails with
`ERROR: VIDIOC_S_FMT: Device or resource busy` — an earlier instance still owns
`/dev/video2`.

## Model size: 512 for headless, 384 for a live demo

End-to-end, 300+ frames each, headless and pinned:

| size | preprocess | inference | total | **FPS** | limited by |
|---|---|---|---|---|---|
| 384 | ~7 ms | 11.3 ms | 33.3 ms | **30.1** | **camera** |
| **512** | 7.79 ms | 19.4 ms | 33.3 ms | **30.1** | **camera** |
| 640 | 11.6 ms | 30.9 ms | 44.8 ms | 22.4 | compute |
| 768 | ~15 ms | 42.0 ms | 60.7 ms | 16.5 | compute |

**512 is the sweet spot when nothing is being displayed.** It hits the 30 FPS camera ceiling, so
384 buys no extra frames headless — both are camera-limited, and 384 only loses accuracy. 640
misses 30 FPS because inference alone (30.9 ms) almost exhausts the 33.3 ms budget before
preprocessing is counted.

**With the side-by-side display on, prefer 384.** The composited frame is twice as wide and the
sink's blocking write costs ~6.5 ms/frame, which pushes 512 down to 25.7 FPS while 384 stays at
the full 30.0. See the side-by-side section above for the measured table.

The stages are strictly serial, so they add. Getting 640 to 30 FPS would need preprocessing
overlapped with inference (double-buffer + thread) or the resize moved off the CPU.

## Design decision: stay single-threaded (target application is obstacle avoidance)

The loop is strictly serial — capture, preprocess, infer, colourize, display all
add up. A double-buffered (ping-pong) design would overlap CPU preprocessing with
NPU inference, turning the sum into a max:

```
now:        11.6 + 30.9 + 2.3  = 44.8 ms  -> 22.4 FPS   (640px)
overlapped: max(11.6 + 2.3, 30.9) = 30.9 ms -> 32 FPS
```

**Deliberately not done.** Two reasons, and the second is the important one:

1. At 512px the pipeline is already **camera-limited**, not compute-limited. The
   sensor tops out at 30 fps, so overlapping would buy zero extra frames.
2. **The target application is obstacle avoidance.** Pipelining raises throughput
   but adds roughly one stage of latency to each individual frame: the depth map
   you act on is older by the time it reaches you. For live viewing that is
   invisible; for a control loop reacting to an obstacle, glass-to-decision
   latency is the number that matters, not frames per second.

It also costs real complexity — two threads sharing tensor buffers need
mutex/condvar synchronisation, and the current loop's zero-allocation,
single-threaded structure is easy to reason about and hard to get wrong.

Revisit only if a genuine need for >= 640px accuracy appears, or the camera is
replaced with one that exceeds 30 fps. If it is revisited, **measure end-to-end
latency, not just FPS** — for this application a faster number that arrives later
is a regression.

## CPU pinning is load-bearing, not a micro-optimization

This SoC is a 3+4+1 big.LITTLE: cpu0-2 cap at 2.02 GHz, cpu3-6 at 2.80 GHz,
cpu7 at 3.19 GHz. Measured cost of the 480->640 bilinear + CHW pass:

| core | ms |
|---|---|
| cpu0 (little) | 16.40 |
| cpu4 (big) | 6.16 |
| cpu7 (prime) | 3.32 |

Left unpinned the scheduler drifts the loop onto a little core:

| | preprocess | end-to-end |
|---|---|---|
| unpinned | 26.8 ms | **14.5 FPS** |
| pinned to cpu7 | 11.4 ms | **22.4 FPS** |

So the program pins itself to the highest-frequency core at startup (`--no-pin`
disables it). This also corrects an earlier note: the "0.75 ms preprocessing"
figure in `NOTES-capture-display.md` measured *only* the HWC->CHW normalize of
an already-correctly-sized buffer, on a core that happened to be fast. The real
stage additionally does YUY2->RGB (~1.4 ms) and the bilinear resize (~10 ms),
because the camera's 480x480 crop has to be **up**scaled to 640x640.

**The resize is now the only CPU stage that matters.** If more headroom is
needed, the cheapest win is the 512 model (20.0 ms inference, and a 480->512
resize is smaller) rather than optimizing the resize itself.

## Why centre-crop

The camera gives 640x480; the model wants a square. The program takes the
central 480x480 (dropping 80 px each side) and scales that to SxS. Cropping
rather than stretching keeps the aspect ratio correct — a monocular depth net
infers scale partly from object proportions, so a stretched frame biases the
depth. The cost is ~25% horizontal field of view.

## Display notes

Weston's socket for root is at `/run/user/root/wayland-1`, **not** the
documented `/run/user/0` — with the latter `waylandsink` never reaches PAUSED.
The program sets `XDG_RUNTIME_DIR=/run/user/root` and
`WAYLAND_DISPLAY=wayland-1` in the `popen()` command line itself.

Display was verified, not assumed. `DEPTH_CAM_DUMP=<path>` writes the last
colourized BGRA frame (plus the raw fp16 depth and the cropped RGB) so the
displayed content can be checked off-board. On a 60-frame run the dump showed a
correct, well-aligned depth map: the person and the phone they were holding
rendered near (warm), the desk and back wall rendered far (cool), with 2562
distinct depth values and a monotonic near-to-far gradient from the bottom of
the frame to the top. The pipeline runs clean (`gbm_create_device ... msm_drm`,
exit 0), but see the caveat below about on-screen confirmation.

## Verified

- Compiles clean with `-Wall -Wextra`.
- 512px: 30.1 FPS headless / 25.7 FPS with the default side-by-side display
  (29.5 FPS with `--depth-only`), exit 0, steady.
- 640px: 22.4 FPS headless / 20.3 FPS with display, exit 0, steady.
- 1200-frame run: FPS flat across 200-frame blocks, `VmRSS` constant at
  24992 kB, no thermal drift (inference +0.036 ms first-200 vs last-200).
- Depth output matches the ONNX reference at **corr 0.999946 / MAE 0.0145 m**,
  measured on the exact fp16 tensor handed to `graphExecute`.
- Output is input-dependent: changing the scene changes the depth map
  (no two frames identical; mean |diff| 0.413 m across an exposure change).
- **On-screen output confirmed on the HDMI monitor** (2026-09-15): the
  colourized depth map renders live via `waylandsink`.
