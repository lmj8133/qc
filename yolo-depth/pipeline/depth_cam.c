/*
 * depth_cam.c -- real-time monocular depth pipeline for QCS8550 (Hexagon V73).
 *
 * V4L2 YUYV capture -> YUY2->RGB -> centre-crop + bilinear scale to SxS
 *   -> normalize to fp16 NCHW -> QNN graphExecute (resident graph)
 *   -> turbo colourize -> BGRA to a GStreamer waylandsink via popen().
 *
 * Design constraints that shaped this (all measured, see NOTES-capture-display.md):
 *   - The UVC camera only sustains 30 fps in YUYV at <= 640x480, and the board
 *     has no libjpeg, so MJPG would need a GStreamer decode hop. YUYV 640x480.
 *   - The QNN context binary is loaded ONCE and the graph stays resident;
 *     spawning qnn-net-run per frame costs ~594 ms wall clock per frame.
 *   - Everything the per-frame loop touches is allocated up front. The loop
 *     performs zero malloc/free.
 *
 * Build (natively, on the board):
 *   gcc -O3 -march=armv8.2-a+fp16 -o depth_cam depth_cam.c \
 *       -I/opt/qcom/qirp-sdk/include -ldl -lm
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sched.h>

#include "QnnInterface.h"
#include "QnnContext.h"
#include "QnnGraph.h"
#include "QnnTensor.h"
#include "QnnTypes.h"
#include "System/QnnSystemInterface.h"
#include "System/QnnSystemContext.h"

#define CAP_W 640
#define CAP_H 480
#define V4L2_BUFFERS 4

#define FAIL(...)                                    \
	do {                                             \
		fprintf(stderr, "ERROR: " __VA_ARGS__);      \
		fprintf(stderr, "\n");                       \
		return 1;                                    \
	} while (0)

#define CHECK(cond, ...)                             \
	do {                                             \
		if (!(cond)) { FAIL(__VA_ARGS__); }          \
	} while (0)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* ioctl retried across EINTR -- V4L2 calls are interruptible by signals. */
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static size_t tensor_elems(const Qnn_Tensor_t *t)
{
	size_t n = 1;
	for (uint32_t i = 0; i < t->v1.rank; i++) {
		n *= t->v1.dimensions[i];
	}
	return n;
}

static size_t dtype_size(Qnn_DataType_t dt)
{
	switch (dt) {
	case QNN_DATATYPE_FLOAT_16: return 2;
	case QNN_DATATYPE_FLOAT_32: return 4;
	case QNN_DATATYPE_UFIXED_POINT_8:
	case QNN_DATATYPE_SFIXED_POINT_8: return 1;
	case QNN_DATATYPE_UFIXED_POINT_16:
	case QNN_DATATYPE_SFIXED_POINT_16: return 2;
	default: return 4;
	}
}

/*
 * Pin the pipeline thread to the fastest core.
 *
 * This is not a micro-optimization, it is the difference between meeting and
 * missing the frame budget. The SoC is a 3+4+1 big.LITTLE: cpu0-2 cap at
 * 2.02 GHz, cpu3-6 at 2.80 GHz, cpu7 at 3.19 GHz. Measured cost of the
 * 480->640 bilinear+CHW pass: 16.4 ms on cpu0, 6.2 ms on cpu4, 3.3 ms on cpu7.
 * Left unpinned the scheduler drifts the loop onto a little core -- observed
 * preprocess 26.8 ms and 14.5 fps end-to-end, versus 11.4 ms and 22.4 fps
 * pinned. The QNN backend runs its own FastRPC threads, so pinning only this
 * thread does not starve the NPU.
 *
 * Picks the highest-numbered core sharing the maximum cpuinfo_max_freq.
 */
static int pin_to_fastest_core(void)
{
	long best_khz = -1;
	int best_cpu = -1;
	cpu_set_t set;

	for (int cpu = 0; cpu < CPU_SETSIZE && cpu < 64; cpu++) {
		char path[128];
		FILE *fp;
		long khz;

		snprintf(path, sizeof(path),
		         "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		fp = fopen(path, "r");
		if (!fp) {
			continue;
		}
		if (fscanf(fp, "%ld", &khz) == 1 && khz >= best_khz) {
			best_khz = khz;
			best_cpu = cpu;
		}
		fclose(fp);
	}
	if (best_cpu < 0) {
		return -1;
	}
	CPU_ZERO(&set);
	CPU_SET(best_cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0) {
		fprintf(stderr, "WARN: sched_setaffinity(cpu%d): %s\n", best_cpu, strerror(errno));
		return -1;
	}
	printf("cpu affinity     : pinned to cpu%d (%.2f GHz max)\n", best_cpu, best_khz / 1e6);
	return best_cpu;
}

/* ---------------------------------------------------------------- V4L2 --- */

struct v4l2_cam {
	int fd;
	uint32_t n_buf;
	void *buf[V4L2_BUFFERS];
	size_t len[V4L2_BUFFERS];
};

static int cam_open(struct v4l2_cam *c, const char *dev)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_streamparm parm;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	memset(c, 0, sizeof(*c));
	c->fd = open(dev, O_RDWR | O_NONBLOCK, 0);
	CHECK(c->fd >= 0, "open %s: %s", dev, strerror(errno));

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = CAP_W;
	fmt.fmt.pix.height = CAP_H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	CHECK(xioctl(c->fd, VIDIOC_S_FMT, &fmt) == 0, "VIDIOC_S_FMT: %s", strerror(errno));
	CHECK(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV,
	      "driver refused YUYV (gave 0x%08x)", fmt.fmt.pix.pixelformat);
	CHECK(fmt.fmt.pix.width == CAP_W && fmt.fmt.pix.height == CAP_H,
	      "driver refused %dx%d (gave %ux%u)", CAP_W, CAP_H,
	      fmt.fmt.pix.width, fmt.fmt.pix.height);

	/* Ask for 30 fps explicitly; UVC otherwise may pick a slower interval. */
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = 30;
	xioctl(c->fd, VIDIOC_S_PARM, &parm);

	memset(&req, 0, sizeof(req));
	req.count = V4L2_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	CHECK(xioctl(c->fd, VIDIOC_REQBUFS, &req) == 0, "VIDIOC_REQBUFS: %s", strerror(errno));
	CHECK(req.count >= 2, "got only %u buffers", req.count);
	c->n_buf = req.count;

	for (uint32_t i = 0; i < c->n_buf; i++) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		CHECK(xioctl(c->fd, VIDIOC_QUERYBUF, &b) == 0, "VIDIOC_QUERYBUF %u", i);
		c->len[i] = b.length;
		c->buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		                 c->fd, b.m.offset);
		CHECK(c->buf[i] != MAP_FAILED, "mmap buffer %u: %s", i, strerror(errno));
		CHECK(xioctl(c->fd, VIDIOC_QBUF, &b) == 0, "VIDIOC_QBUF %u", i);
	}

	CHECK(xioctl(c->fd, VIDIOC_STREAMON, &type) == 0, "VIDIOC_STREAMON: %s", strerror(errno));
	return 0;
}

static void cam_close(struct v4l2_cam *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (c->fd < 0) {
		return;
	}
	xioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (uint32_t i = 0; i < c->n_buf; i++) {
		if (c->buf[i] && c->buf[i] != MAP_FAILED) {
			munmap(c->buf[i], c->len[i]);
		}
	}
	close(c->fd);
	c->fd = -1;
}

/*
 * Block on the fd, dequeue one filled buffer, copy nothing -- the caller reads
 * straight out of the mmap'd buffer and calls cam_requeue when done with it.
 * Returns the buffer index, or -1 on error/timeout.
 */
static int cam_grab(struct v4l2_cam *c, struct v4l2_buffer *out, int timeout_s)
{
	fd_set fds;
	struct timeval tv;
	int r;

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(c->fd, &fds);
		tv.tv_sec = timeout_s;
		tv.tv_usec = 0;
		r = select(c->fd + 1, &fds, NULL, NULL, &tv);
		if (r == -1) {
			if (errno == EINTR) {
				if (g_stop) {
					return -1;
				}
				continue;
			}
			return -1;
		}
		if (r == 0) {
			fprintf(stderr, "ERROR: capture timeout after %d s\n", timeout_s);
			return -1;
		}
		memset(out, 0, sizeof(*out));
		out->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		out->memory = V4L2_MEMORY_MMAP;
		if (xioctl(c->fd, VIDIOC_DQBUF, out) == 0) {
			return (int)out->index;
		}
		if (errno == EAGAIN) {
			continue;
		}
		fprintf(stderr, "ERROR: VIDIOC_DQBUF: %s\n", strerror(errno));
		return -1;
	}
}

static int cam_requeue(struct v4l2_cam *c, struct v4l2_buffer *b)
{
	return xioctl(c->fd, VIDIOC_QBUF, b);
}

/* --------------------------------------------------------- conversion --- */

static inline uint8_t clamp_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/*
 * YUY2 (Y0 U Y1 V) -> packed RGB888, integer BT.601 full-range coefficients.
 * The source rectangle is a centre crop: the camera is 640x480 and the model
 * wants a square, so we take the central 480x480 and drop 80 px from each
 * side. Cropping rather than letterboxing/stretching keeps the aspect ratio
 * correct, which matters because a monocular depth net infers scale partly
 * from object proportions; a stretched frame biases the depth.
 */
static void yuyv_crop_to_rgb(const uint8_t *src, int src_w, int crop_x, int crop_w,
                             int crop_h, uint8_t *dst)
{
	/* Each iteration decodes a YUY2 macropixel (2 pixels, 6 RGB bytes), so an odd
	 * crop_w would write 3 bytes past the row end on the final iteration. */
	if (crop_w % 2 != 0) {
		return;
	}
	for (int y = 0; y < crop_h; y++) {
		const uint8_t *s = src + (size_t)y * src_w * 2 + (size_t)crop_x * 2;
		uint8_t *d = dst + (size_t)y * crop_w * 3;
		for (int x = 0; x < crop_w; x += 2) {
			int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
			int cu = u - 128, cv = v - 128;
			int r_off = (91881 * cv) >> 16;
			int g_off = (-22554 * cu - 46802 * cv) >> 16;
			int b_off = (116130 * cu) >> 16;
			d[0] = clamp_u8(y0 + r_off);
			d[1] = clamp_u8(y0 + g_off);
			d[2] = clamp_u8(y0 + b_off);
			d[3] = clamp_u8(y1 + r_off);
			d[4] = clamp_u8(y1 + g_off);
			d[5] = clamp_u8(y1 + b_off);
			s += 4;
			d += 6;
		}
	}
}

/*
 * Bilinear resize of a packed RGB888 image, plus the HWC->CHW transpose and
 * the [0,1] normalization into fp16, all in one pass so the intermediate
 * never hits memory twice. Measured under 1 ms/frame at every model size.
 */
static void resize_to_chw_fp16(const uint8_t *src, int sw, int sh,
                               __fp16 *dst, int S)
{
	const size_t plane = (size_t)S * S;
	const float xs = (float)sw / (float)S;
	const float ys = (float)sh / (float)S;

	for (int oy = 0; oy < S; oy++) {
		float fy = ((float)oy + 0.5f) * ys - 0.5f;
		int y0 = (int)fy;
		float wy = fy - (float)y0;
		if (y0 < 0) { y0 = 0; wy = 0.0f; }
		if (y0 >= sh - 1) { y0 = sh - 2; wy = 1.0f; }
		const uint8_t *r0 = src + (size_t)y0 * sw * 3;
		const uint8_t *r1 = r0 + (size_t)sw * 3;

		for (int ox = 0; ox < S; ox++) {
			float fx = ((float)ox + 0.5f) * xs - 0.5f;
			int x0 = (int)fx;
			float wx = fx - (float)x0;
			if (x0 < 0) { x0 = 0; wx = 0.0f; }
			if (x0 >= sw - 1) { x0 = sw - 2; wx = 1.0f; }

			const uint8_t *a = r0 + (size_t)x0 * 3;
			const uint8_t *b = r1 + (size_t)x0 * 3;
			float w00 = (1.0f - wx) * (1.0f - wy);
			float w10 = wx * (1.0f - wy);
			float w01 = (1.0f - wx) * wy;
			float w11 = wx * wy;
			size_t o = (size_t)oy * S + (size_t)ox;

			for (int ch = 0; ch < 3; ch++) {
				float p = a[ch] * w00 + a[3 + ch] * w10 +
				          b[ch] * w01 + b[3 + ch] * w11;
				dst[(size_t)ch * plane + o] = (__fp16)(p * (1.0f / 255.0f));
			}
		}
	}
}

/* --------------------------------------------------------- colourize --- */

/*
 * Turbo-like colour map, 256 entries, built once at startup. Storing it as
 * packed BGRA lets the colourize loop do a single 32-bit store per pixel.
 */
static void build_turbo_lut(uint32_t *lut)
{
	/* Anchor points of the Turbo map, sampled at 1/8 intervals. */
	static const float key[9][3] = {
		{0.190f, 0.072f, 0.232f}, {0.246f, 0.431f, 0.856f},
		{0.164f, 0.706f, 0.965f}, {0.118f, 0.898f, 0.741f},
		{0.353f, 0.984f, 0.424f}, {0.686f, 0.950f, 0.208f},
		{0.930f, 0.776f, 0.157f}, {0.996f, 0.487f, 0.118f},
		{0.845f, 0.161f, 0.020f}
	};
	for (int i = 0; i < 256; i++) {
		float t = (float)i / 255.0f * 8.0f;
		int k = (int)t;
		if (k > 7) { k = 7; }
		float f = t - (float)k;
		float r = key[k][0] + (key[k + 1][0] - key[k][0]) * f;
		float g = key[k][1] + (key[k + 1][1] - key[k][1]) * f;
		float b = key[k][2] + (key[k + 1][2] - key[k][2]) * f;
		uint32_t R = (uint32_t)clamp_u8((int)(r * 255.0f + 0.5f));
		uint32_t G = (uint32_t)clamp_u8((int)(g * 255.0f + 0.5f));
		uint32_t B = (uint32_t)clamp_u8((int)(b * 255.0f + 0.5f));
		/* Little-endian BGRA byte order: B,G,R,A */
		lut[i] = B | (G << 8) | (R << 16) | 0xFF000000u;
	}
}

/*
 * Map the fp16 metric depth map to BGRA. The normalization window is computed
 * per frame from the actual min/max so the display stays useful regardless of
 * scene depth range; near = warm, far = cool (the map is inverted).
 */
static void colourize(const __fp16 *depth, int S, const uint32_t *lut, uint32_t *bgra,
                      float *out_min, float *out_max)
{
	const size_t n = (size_t)S * S;
	float mn = 1e30f, mx = -1e30f;

	for (size_t i = 0; i < n; i++) {
		float d = (float)depth[i];
		if (d < mn) { mn = d; }
		if (d > mx) { mx = d; }
	}
	*out_min = mn;
	*out_max = mx;

	float scale = (mx > mn) ? 255.0f / (mx - mn) : 0.0f;
	for (size_t i = 0; i < n; i++) {
		int v = (int)(((float)depth[i] - mn) * scale);
		if (v < 0) { v = 0; }
		if (v > 255) { v = 255; }
		bgra[i] = lut[255 - v];
	}
}

/*
 * Build the side-by-side view: camera on the left, depth on the right, each SxS,
 * giving a 2S x S BGRA frame. Seeing the source next to the depth map is what
 * makes the output legible to someone who has not been staring at it -- a warm
 * blob means nothing until you can see it is a person.
 *
 * The camera image is the cropped RGB (crop_w x crop_h, the same square the net
 * saw), nearest-neighbour scaled to SxS so both panes align. Nearest is enough
 * here: this is a preview, and it costs a fraction of the bilinear path.
 */
static void compose_side_by_side(const uint8_t *rgb, int crop_w, int crop_h,
                                 const uint32_t *depth_bgra, int S, uint32_t *out)
{
	const int W = 2 * S;

	for (int y = 0; y < S; y++) {
		int sy = y * crop_h / S;
		const uint8_t *srow = rgb + (size_t)sy * crop_w * 3;
		uint32_t *orow = out + (size_t)y * W;

		for (int x = 0; x < S; x++) {
			int sx = x * crop_w / S;
			const uint8_t *px = srow + (size_t)sx * 3;
			/* rgb is R,G,B; the sink wants BGRA little-endian (B,G,R,A) */
			orow[x] = 0xff000000u | ((uint32_t)px[0] << 16)
			          | ((uint32_t)px[1] << 8) | (uint32_t)px[2];
		}
		memcpy(orow + S, depth_bgra + (size_t)y * S, (size_t)S * 4);
	}
}

/* ------------------------------------------------------------ display --- */

static int write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, buf + off, len - off);
		if (w <= 0) {
			if (w < 0 && errno == EINTR) {
				continue;
			}
			return -1;
		}
		off += (size_t)w;
	}
	return 0;
}

/*
 * The display sink is a child gst-launch-1.0 reading raw BGRA from its stdin.
 * Weston's socket for root is at /run/user/root/wayland-1, NOT /run/user/0 --
 * waylandsink fails to reach PAUSED with the documented path.
 */
static FILE *display_open(int w, int h, int fps)
{
	char cmd[768];
	snprintf(cmd, sizeof(cmd),
	         "XDG_RUNTIME_DIR=/run/user/root WAYLAND_DISPLAY=wayland-1 "
	         "exec gst-launch-1.0 -q fdsrc fd=0 ! "
	         "rawvideoparse use-sink-caps=false width=%d height=%d format=bgra "
	         /* No videoconvert: waylandsink takes BGRA natively, and at 2S wide
	          * the extra copy cost ~6 ms/frame for nothing. */
	         "framerate=%d/1 ! waylandsink sync=false "
	         /* gst-launch prints a running position counter ("0:00:01.2 / ...")
	          * to STDOUT even under -q, which floods an interactive ssh -t
	          * session and hides our own stats lines. Only the child's stdin is
	          * ours (the pipe); its stdout/stderr just inherit our terminal, so
	          * drop both. Failures still surface: popen/write errors are caught
	          * by the caller, and a dead sink shows up as a write failure. */
	         ">/dev/null 2>&1",
	         w, h, fps);
	FILE *p = popen(cmd, "w");
	if (!p) {
		fprintf(stderr, "WARN: popen(gst-launch-1.0) failed: %s\n", strerror(errno));
	}
	return p;
}

/* ---------------------------------------------------------------- stats -- */

struct stage_stats {
	double cap, pre, inf, col, disp;
	int n;
};

static void stats_reset(struct stage_stats *s)
{
	s->cap = s->pre = s->inf = s->col = s->disp = 0.0;
	s->n = 0;
}

static void usage(const char *prog)
{
	printf(
"Usage: %s --model <context.bin> [options]\n"
"\n"
"Real-time monocular depth: V4L2 YUYV capture -> QNN HTP inference -> display.\n"
"\n"
"  --model <path>    QNN context binary (required), e.g. y26n_640_fp16_v73.bin\n"
"  --device <path>   V4L2 capture device            (default /dev/video2)\n"
"  --size <N>        Model input side length S      (default 640)\n"
"  --frames <N>      Stop after N frames, 0 = run until Ctrl-C (default 300)\n"
"  --stats-every <N> Frames between stats lines      (default 30)\n"
"  --no-display      Skip the GStreamer sink (headless benchmark)\n"
"  --no-pin          Do not pin to the fastest CPU core (pinning is on by default)\n"
"  --depth-only      Show only the depth map; default is camera|depth side by side\n"
"  --help            This message\n"
"\n"
"Example:\n"
"  %s --model /dev/shm/y26n_640_fp16_v73.bin --device /dev/video2 \\\n"
"      --size 640 --frames 300 --no-display\n"
"\n"
"Exit code 0 on success, non-zero on failure.\n",
	prog, prog);
}

int main(int argc, char **argv)
{
	const char *bin_path = NULL;
	const char *dev_path = "/dev/video2";
	int S = 640;
	int want_frames = 300;
	int stats_every = 30;
	int no_display = 0;
	int no_pin = 0;
	int depth_only = 0;

	static struct option opts[] = {
		{ "model",       required_argument, 0, 'm' },
		{ "device",      required_argument, 0, 'd' },
		{ "size",        required_argument, 0, 's' },
		{ "frames",      required_argument, 0, 'f' },
		{ "stats-every", required_argument, 0, 'e' },
		{ "no-display",  no_argument,       0, 'n' },
		{ "no-pin",      no_argument,       0, 'p' },
		{ "depth-only",  no_argument,       0, 'D' },
		{ "help",        no_argument,       0, 'h' },
		{ 0, 0, 0, 0 }
	};

	for (;;) {
		int c = getopt_long(argc, argv, "m:d:s:f:e:nphD", opts, NULL);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'm': bin_path = optarg; break;
		case 'd': dev_path = optarg; break;
		case 's': S = atoi(optarg); break;
		case 'f': want_frames = atoi(optarg); break;
		case 'e': stats_every = atoi(optarg); break;
		case 'n': no_display = 1; break;
		case 'p': no_pin = 1; break;
		case 'D': depth_only = 1; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (!bin_path) {
		fprintf(stderr, "ERROR: --model is required\n\n");
		usage(argv[0]);
		return 2;
	}
	CHECK(S > 0 && S <= 4096, "bad --size %d", S);
	CHECK(stats_every > 0, "bad --stats-every %d", stats_every);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);   /* a dying gst child must not kill us */

	if (!no_pin) {
		pin_to_fastest_core();
	}

	/* =========================== QNN setup (from qnn_bench.c) =========== */
	void *bh = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL);
	CHECK(bh, "dlopen libQnnHtp.so: %s", dlerror());
	void *sh = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL);
	CHECK(sh, "dlopen libQnnSystem.so: %s", dlerror());

	Qnn_ErrorHandle_t (*get_providers)(const QnnInterface_t ***, uint32_t *) =
		dlsym(bh, "QnnInterface_getProviders");
	CHECK(get_providers, "QnnInterface_getProviders not found");
	const QnnInterface_t **providers = NULL;
	uint32_t num_providers = 0;
	CHECK(get_providers(&providers, &num_providers) == QNN_SUCCESS && num_providers > 0,
	      "getProviders failed");
	QNN_INTERFACE_VER_TYPE qnn = providers[0]->QNN_INTERFACE_VER_NAME;

	Qnn_ErrorHandle_t (*get_sys_providers)(const QnnSystemInterface_t ***, uint32_t *) =
		dlsym(sh, "QnnSystemInterface_getProviders");
	CHECK(get_sys_providers, "QnnSystemInterface_getProviders not found");
	const QnnSystemInterface_t **sys_providers = NULL;
	uint32_t num_sys = 0;
	CHECK(get_sys_providers(&sys_providers, &num_sys) == QNN_SUCCESS && num_sys > 0,
	      "system getProviders failed");
	QNN_SYSTEM_INTERFACE_VER_TYPE sys = sys_providers[0]->QNN_SYSTEM_INTERFACE_VER_NAME;

	FILE *f = fopen(bin_path, "rb");
	CHECK(f, "cannot open %s", bin_path);
	fseek(f, 0, SEEK_END);
	long bin_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *bin_buf = malloc(bin_size);
	CHECK(bin_buf && fread(bin_buf, 1, bin_size, f) == (size_t)bin_size, "read %s", bin_path);
	fclose(f);

	QnnSystemContext_Handle_t sys_ctx = NULL;
	CHECK(sys.systemContextCreate(&sys_ctx) == QNN_SUCCESS, "systemContextCreate failed");
	const QnnSystemContext_BinaryInfo_t *bin_info = NULL;
	Qnn_ContextBinarySize_t bin_info_size = 0;
	CHECK(sys.systemContextGetBinaryInfo(sys_ctx, bin_buf, bin_size, &bin_info, &bin_info_size)
	          == QNN_SUCCESS,
	      "systemContextGetBinaryInfo failed");

	const QnnSystemContext_GraphInfo_t *graphs = NULL;
	uint32_t num_graphs = 0;
	switch (bin_info->version) {
	case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1:
		graphs = bin_info->contextBinaryInfoV1.graphs;
		num_graphs = bin_info->contextBinaryInfoV1.numGraphs;
		break;
	case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2:
		graphs = bin_info->contextBinaryInfoV2.graphs;
		num_graphs = bin_info->contextBinaryInfoV2.numGraphs;
		break;
	case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3:
		graphs = bin_info->contextBinaryInfoV3.graphs;
		num_graphs = bin_info->contextBinaryInfoV3.numGraphs;
		break;
	default:
		FAIL("unsupported binary info version %d", (int)bin_info->version);
	}
	CHECK(num_graphs > 0, "no graphs in binary");

	/* GraphInfo V1/V2/V3 share their leading members, but read via the
	 * version the binary actually declares rather than assuming V1. */
	const char *graph_name;
	Qnn_Tensor_t *in_tensors, *out_tensors;
	uint32_t num_in, num_out;
	switch (graphs[0].version) {
	case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3: {
		const QnnSystemContext_GraphInfoV3_t *g = &graphs[0].graphInfoV3;
		graph_name = g->graphName; in_tensors = g->graphInputs; out_tensors = g->graphOutputs;
		num_in = g->numGraphInputs; num_out = g->numGraphOutputs;
		break;
	}
	case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2: {
		const QnnSystemContext_GraphInfoV2_t *g = &graphs[0].graphInfoV2;
		graph_name = g->graphName; in_tensors = g->graphInputs; out_tensors = g->graphOutputs;
		num_in = g->numGraphInputs; num_out = g->numGraphOutputs;
		break;
	}
	default: {
		const QnnSystemContext_GraphInfoV1_t *g = &graphs[0].graphInfoV1;
		graph_name = g->graphName; in_tensors = g->graphInputs; out_tensors = g->graphOutputs;
		num_in = g->numGraphInputs; num_out = g->numGraphOutputs;
		break;
	}
	}

	/* Trust the binary's own shape over the --size flag: a mismatch here
	 * would silently feed the graph a wrongly-scaled frame. */
	CHECK(in_tensors[0].v1.rank == 4, "expected rank-4 input, got %u", in_tensors[0].v1.rank);
	int model_s = (int)in_tensors[0].v1.dimensions[2];
	if (model_s != S) {
		fprintf(stderr, "NOTE: --size %d overridden by the binary's input %d\n", S, model_s);
		S = model_s;
	}
	CHECK(in_tensors[0].v1.dataType == QNN_DATATYPE_FLOAT_16,
	      "expected FLOAT16 input, got %d", (int)in_tensors[0].v1.dataType);
	CHECK(out_tensors[0].v1.dataType == QNN_DATATYPE_FLOAT_16,
	      "expected FLOAT16 output, got %d", (int)out_tensors[0].v1.dataType);
	/* colourize() reads S*S output elements, so the output must really be SxS.
	 * Trusting the input shape alone would over-read on an asymmetric graph. */
	CHECK(out_tensors[0].v1.rank == 4, "expected rank-4 output, got %u", out_tensors[0].v1.rank);
	CHECK((int)out_tensors[0].v1.dimensions[2] == S && (int)out_tensors[0].v1.dimensions[3] == S,
	      "output %ux%u does not match input %dx%d",
	      out_tensors[0].v1.dimensions[2], out_tensors[0].v1.dimensions[3], S, S);

	printf("graph            : %s\n", graph_name);
	printf("context binary   : %s (%.1f MB)\n", bin_path, bin_size / 1048576.0);
	printf("model input      : %dx%d fp16 NCHW\n", S, S);

	double t_load0 = now_ms();
	Qnn_BackendHandle_t backend = NULL;
	CHECK(qnn.backendCreate(NULL, NULL, &backend) == QNN_SUCCESS, "backendCreate failed");
	Qnn_DeviceHandle_t device = NULL;
	qnn.deviceCreate(NULL, NULL, &device);   /* optional on HTP */

	Qnn_ContextHandle_t context = NULL;
	CHECK(qnn.contextCreateFromBinary(backend, device, NULL, bin_buf, bin_size, &context, NULL)
	          == QNN_SUCCESS,
	      "contextCreateFromBinary failed -- version mismatch?");
	Qnn_GraphHandle_t graph = NULL;
	CHECK(qnn.graphRetrieve(context, graph_name, &graph) == QNN_SUCCESS, "graphRetrieve failed");
	printf("one-time load    : %.1f ms\n", now_ms() - t_load0);

	/* =========================== allocate everything up front ========== */
	size_t in_bytes = tensor_elems(&in_tensors[0]) * dtype_size(in_tensors[0].v1.dataType);
	size_t out_bytes = tensor_elems(&out_tensors[0]) * dtype_size(out_tensors[0].v1.dataType);

	Qnn_Tensor_t *ins = calloc(num_in, sizeof(Qnn_Tensor_t));
	Qnn_Tensor_t *outs = calloc(num_out, sizeof(Qnn_Tensor_t));
	CHECK(ins && outs, "tensor array alloc failed");
	for (uint32_t i = 0; i < num_in; i++) {
		ins[i] = in_tensors[i];
		ins[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
		ins[i].v1.clientBuf.data = calloc(1, in_bytes);
		ins[i].v1.clientBuf.dataSize = in_bytes;
		CHECK(ins[i].v1.clientBuf.data, "input buffer alloc failed");
	}
	for (uint32_t i = 0; i < num_out; i++) {
		outs[i] = out_tensors[i];
		outs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
		outs[i].v1.clientBuf.data = calloc(1, out_bytes);
		outs[i].v1.clientBuf.dataSize = out_bytes;
		CHECK(outs[i].v1.clientBuf.data, "output buffer alloc failed");
	}

	const int crop_w = CAP_H;                 /* 480x480 centre crop of 640x480 */
	const int crop_h = CAP_H;
	const int crop_x = (CAP_W - crop_w) / 2;  /* 80 px dropped each side */
	uint8_t *rgb = malloc((size_t)crop_w * crop_h * 3);
	uint32_t *bgra = malloc((size_t)S * S * 4);
	/* Side-by-side needs a 2S-wide frame; allocated once, like everything else. */
	uint32_t *composite = depth_only ? NULL : malloc((size_t)S * 2 * S * 4);
	uint32_t *lut = malloc(256 * sizeof(uint32_t));
	CHECK(rgb && bgra && lut && (depth_only || composite), "frame buffer alloc failed");
	build_turbo_lut(lut);

	__fp16 *net_in = (__fp16 *)ins[0].v1.clientBuf.data;
	const __fp16 *net_out = (const __fp16 *)outs[0].v1.clientBuf.data;

	struct v4l2_cam cam;
	if (cam_open(&cam, dev_path) != 0) {
		return 1;
	}
	printf("capture          : %s YUYV %dx%d -> centre crop %dx%d\n",
	       dev_path, CAP_W, CAP_H, crop_w, crop_h);

	FILE *disp = NULL;
	if (!no_display) {
		disp = display_open(depth_only ? S : 2 * S, S, 30);
		if (disp) {
			printf("display          : gst-launch-1.0 fdsrc -> waylandsink (%dx%d BGRA)\n", S, S);
		} else {
			fprintf(stderr, "WARN: continuing without display\n");
		}
	} else {
		printf("display          : disabled (--no-display)\n");
	}
	printf("frames           : %d\n\n", want_frames);

	/* =========================== steady-state loop ===================== */
	struct stage_stats st;
	stats_reset(&st);
	double t_first = 0.0, t_run0 = 0.0;
	int frames = 0;
	int rc = 0;
	float dmin = 0.0f, dmax = 0.0f;

	while (!g_stop && (want_frames == 0 || frames < want_frames)) {
		struct v4l2_buffer vb;

		double t0 = now_ms();
		int idx = cam_grab(&cam, &vb, 5);
		if (idx < 0) {
			if (g_stop) {
				break;
			}
			rc = 1;
			break;
		}
		double t1 = now_ms();

		yuyv_crop_to_rgb((const uint8_t *)cam.buf[idx], CAP_W, crop_x, crop_w, crop_h, rgb);
		resize_to_chw_fp16(rgb, crop_w, crop_h, net_in, S);
		double t2 = now_ms();

		/* The mmap'd buffer is fully consumed by now -- give it straight back
		 * so the driver keeps filling while the NPU works. */
		cam_requeue(&cam, &vb);

		Qnn_ErrorHandle_t e = qnn.graphExecute(graph, ins, num_in, outs, num_out, NULL, NULL);
		if (e != QNN_SUCCESS) {
			fprintf(stderr, "ERROR: graphExecute failed at frame %d (0x%lx)\n",
			        frames, (unsigned long)e);
			rc = 1;
			break;
		}
		double t3 = now_ms();

		colourize(net_out, S, lut, bgra, &dmin, &dmax);
		double t4 = now_ms();

		if (disp) {
			const uint8_t *frame;
			size_t frame_bytes;

			if (depth_only) {
				frame = (const uint8_t *)bgra;
				frame_bytes = (size_t)S * S * 4;
			} else {
				compose_side_by_side(rgb, crop_w, crop_h, bgra, S, composite);
				frame = (const uint8_t *)composite;
				frame_bytes = (size_t)S * 2 * S * 4;
			}
			if (write_all(fileno(disp), frame, frame_bytes) < 0) {
				fprintf(stderr, "WARN: display sink closed; continuing headless\n");
				pclose(disp);
				disp = NULL;
			}
		}
		double t5 = now_ms();

		if (frames == 0) {
			/* Frame 0 carries HVX/HMX power-on and the sink's first-buffer
			 * negotiation; start the FPS clock after it. */
			t_first = t5 - t0;
			t_run0 = t5;
		} else {
			st.cap += t1 - t0;
			st.pre += t2 - t1;
			st.inf += t3 - t2;
			st.col += t4 - t3;
			st.disp += t5 - t4;
			st.n++;
		}
		frames++;

		if (st.n > 0 && frames % stats_every == 0) {
			double e2e = (now_ms() - t_run0) / st.n;
			printf("[%5d] cap %5.2f | pre %5.2f | inf %6.2f | col %5.2f | disp %5.2f ms"
			       "  -> %5.1f fps  (depth %.2f-%.2f m)\n",
			       frames, st.cap / st.n, st.pre / st.n, st.inf / st.n,
			       st.col / st.n, st.disp / st.n, 1000.0 / e2e, dmin, dmax);
			fflush(stdout);
		}
	}

	/* Taken immediately on loop exit: the optional dump below writes several MB
	 * and would otherwise be charged to the end-to-end frame time. */
	double t_total = now_ms() - t_run0;

	/* Optional: write the last colourized frame so the displayed content can be
	 * checked off-board. A pipeline that errors nowhere can still show garbage. */
	const char *dump = getenv("DEPTH_CAM_DUMP");
	if (dump && frames > 0) {
		FILE *fd = fopen(dump, "wb");
		if (fd) {
			fwrite(bgra, 4, (size_t)S * S, fd);
			fclose(fd);
			printf("last frame BGRA  : %s (%dx%d)\n", dump, S, S);
		}
		char rawp[512];
		snprintf(rawp, sizeof(rawp), "%s.depth", dump);
		fd = fopen(rawp, "wb");
		if (fd) {
			fwrite(net_out, 2, (size_t)S * S, fd);
			fclose(fd);
			printf("last frame depth : %s (fp16 %dx%d)\n", rawp, S, S);
		}
		snprintf(rawp, sizeof(rawp), "%s.rgb", dump);
		fd = fopen(rawp, "wb");
		if (fd) {
			fwrite(rgb, 3, (size_t)crop_w * crop_h, fd);
			fclose(fd);
			printf("last frame rgb   : %s (%dx%d)\n", rawp, crop_w, crop_h);
		}
	}

	if (st.n > 0) {
		printf("\n--- summary (%d frames timed, frame 0 discarded as warm-up) ---\n", st.n);
		printf("warm-up frame 0  : %.1f ms\n", t_first);
		printf("capture          : %6.2f ms\n", st.cap / st.n);
		printf("preprocess       : %6.2f ms   (YUY2->RGB, crop %dx%d, scale %dx%d, fp16 NCHW)\n",
		       st.pre / st.n, crop_w, crop_h, S, S);
		printf("inference        : %6.2f ms   (QNN HTP, resident graph)\n", st.inf / st.n);
		printf("colourize        : %6.2f ms\n", st.col / st.n);
		printf("display          : %6.2f ms   (%s)\n", st.disp / st.n,
		       disp ? "waylandsink" : "disabled");
		printf("sum of stages    : %6.2f ms\n",
		       (st.cap + st.pre + st.inf + st.col + st.disp) / st.n);
		printf("end-to-end       : %6.2f ms   -> %.1f FPS\n",
		       t_total / st.n, 1000.0 * st.n / t_total);
	} else {
		fprintf(stderr, "ERROR: no frames completed\n");
		rc = rc ? rc : 1;
	}

	/* =========================== teardown ============================== */
	if (disp) {
		pclose(disp);
	}
	cam_close(&cam);
	qnn.contextFree(context, NULL);
	qnn.backendFree(backend);
	sys.systemContextFree(sys_ctx);
	free(rgb);
	free(bgra);
	free(composite);
	free(lut);
	free(bin_buf);
	return rc;
}
