# Capture and display constraints (measured)

Facts the pipeline design has to work around, verified on the board.

## Camera: YUYV is framerate-capped above 640px

`/dev/video2` (AVerMedia PW310P, UVC). The 30 fps modes differ sharply by format:

| Format | 30 fps modes | Note |
|---|---|---|
| **YUYV** | 320x240, 640x360, **640x480** | 1280x720 caps at **10 fps**, 1920x1080 at **5 fps** |
| **MJPG** | 320x240, 640x360, 640x480, 800x600, 1024x576, 1024x768, 1280x720, 1920x1080 | needs a JPEG decode per frame |

So "use 720p" is not free: in YUYV it costs 3x the frame budget.

**There is no libjpeg on the board** (`ldconfig -p | grep -c jpeg` → 0). MJPG would have to go
through GStreamer's `jpegdec`, adding a decode stage and a process boundary.

**Recommendation: YUYV 640x480 @30fps.** No decode, and it feeds the 640 model (31 FPS measured)
with a centre-crop rather than an upscale.

Confirmed by actually streaming, not just reading the mode table:

```
v4l2-ctl -d /dev/video2 --set-fmt-video=width=640,height=480,pixelformat=YUYV \
         --stream-mmap --stream-count=300 --stream-to=/dev/null
```

The rate **starts at ~24.8 fps and climbs to a steady 30.02 fps** over the first ~100 frames
(UVC warm-up / auto-exposure settling). A short benchmark will under-report; measure 300+ frames.

Note the headroom is thin: capture 30.0 fps against 640px inference at 31 fps (32.2 ms). At 640
the pipeline is **capture-bound**, and any inference regression makes it the bottleneck instead.
512px (50 fps) leaves real margin if that becomes a problem.

## Display: the Wayland socket is NOT where you expect

`waylandsink` fails outright with the usual `XDG_RUNTIME_DIR=/run/user/0`:

```
Failed to set pipeline to PAUSED.
```

Weston's socket actually lives at **`/run/user/root/wayland-1`**:

```bash
export XDG_RUNTIME_DIR=/run/user/root
export WAYLAND_DISPLAY=wayland-1
gst-launch-1.0 videotestsrc num-buffers=150 ! video/x-raw,width=640,height=480,framerate=30/1 ! waylandsink
```

Verified: runs the full 5 s, clean EOS, GBM buffers allocated.

This is the same shape of trap as the audio `XDG_RUNTIME_DIR` issue in
`VERIFICATION-2026-09-14.md` §6 — the documented path is wrong for root.

Available sinks: `waylandsink`, `glimagesink`. **No `/dev/fb0`**, so `fbdevsink` is not an option.

### Feeding it from C works, but the sink back-pressures

A C program writing raw BGRA to `stdout` into `fdsrc` sustains far more than we need:

```bash
./feedtest 640 400 | gst-launch-1.0 -q fdsrc ! \
  rawvideoparse width=640 height=640 format=bgra framerate=30/1 ! \
  videoconvert ! waylandsink sync=false
```

```
pushed 400 frames in 3913 ms -> 102.2 fps, worst write 112.28 ms
```

Average throughput is ~100 fps (3x headroom), and GBM really allocates buffers, so frames do
reach the compositor. **But individual `write()` calls intermittently block for 80-112 ms** —
not just at startup; it recurred across a 400-frame run.

That is ~3 frame periods. Since the depth loop is synchronous, a blocking write would stall
capture and inference behind it. Either keep `sync=false` and accept occasional hitching, or
push display onto a separate thread / non-blocking fd with frame-dropping. **Measure display
time separately in the stats** so this is visible rather than silently inflating "inference" time.

## Preprocessing is NOT cheap — the first measurement was wrong

An earlier note here claimed 0.60-0.75 ms/frame and concluded preprocessing was free. **That was
a bad measurement** and it led to a wrong throughput estimate. Recorded here because the mistake
is the useful part.

What that microbenchmark actually measured: only the HWC→CHW normalize on an *already correctly
sized* buffer, and it happened to be scheduled on a fast core. It omitted both real costs:

1. **YUY2 → RGB** conversion of the captured frame
2. **480→640 bilinear upscale** (the camera gives 480 square after crop; the model wants 640)

Measured in the real pipeline, pinned to cpu7:

| stage | 512px | 640px |
|---|---|---|
| preprocess (full: YUY2→RGB + crop + scale + fp16 NCHW) | **7.79 ms** | **11.59 ms** |

An order of magnitude more than the microbenchmark suggested.

### big.LITTLE placement dominates

The board is heterogeneous: cpu0-2 @ 2.02 GHz, cpu3-6 @ 2.80 GHz, cpu7 @ 3.19 GHz. The same
480→640 bilinear+CHW pass costs **16.4 ms on cpu0, 6.2 ms on cpu4, 3.3 ms on cpu7**.

Left unpinned, the scheduler drifts the loop onto a little core and throughput collapses
(~14.5 FPS vs 22.3 pinned). `depth_cam.c` therefore pins itself to the fastest core at startup.
This is a 5x effect on that stage — larger than any other tuning knob found.

## End-to-end result

Full pipeline, 300 frames, headless, pinned:

| size | capture | preprocess | inference | colourize | total | **FPS** |
|---|---|---|---|---|---|---|
| **512** | 4.63 | 7.79 | 19.35 | 1.49 | 33.27 ms | **30.1** ✅ |
| 640 | 0.04 | 11.59 | 30.87 | 2.25 | 44.75 ms | 22.3 |

**512px meets the 30 FPS target; 640px does not.** At 640 the budget is blown by inference
(30.9 ms) plus preprocessing (11.6 ms) alone — the loop is strictly serial, so the stages add up.

Note `capture` reads 0.04 ms at 640 but 4.63 ms at 512. That is not a capture regression: at 640
the pipeline is slower than the 30 fps camera, so a filled buffer is *always* already waiting and
the driver is dropping frames. At 512 the pipeline runs at camera speed and genuinely waits.
**A near-zero capture time is a symptom of dropping frames, not of fast capture.**

## Getting 640 to 30 FPS would need

The stages are serial and inference alone is 30.9 ms, so the only routes are overlapping
preprocessing with inference (double-buffer + thread), or moving the resize off the CPU
(`qtimlvconverter` / GPU). Neither is needed if 512 is acceptable.
