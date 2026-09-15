/*
 * Persistent QNN inference benchmark for QCS8550 (Hexagon V73).
 *
 * Loads a context binary ONCE, then runs N inferences against a resident graph,
 * which is what a real-time pipeline must do: spawning qnn-net-run per frame
 * costs ~560ms of load/teardown versus ~30ms of actual compute.
 *
 * Reports per-inference wall-clock latency (min/mean/median/p95/max) and FPS.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "QnnInterface.h"
#include "QnnContext.h"
#include "QnnGraph.h"
#include "QnnTensor.h"
#include "QnnTypes.h"
#include "System/QnnSystemInterface.h"
#include "System/QnnSystemContext.h"

#define CHECK(cond, ...)                             \
    do {                                             \
        if (!(cond)) {                               \
            fprintf(stderr, "ERROR: " __VA_ARGS__);  \
            fprintf(stderr, "\n");                   \
            return 1;                                \
        }                                            \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Total element count of a tensor's dimensions. */
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <context.bin> <input.raw> [iterations]\n"
                "Example: %s y26n_640_fp16_v73.bin in_640.raw 200\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *bin_path = argv[1];
    const char *in_path  = argv[2];
    int iters = (argc > 3) ? atoi(argv[3]) : 200;

    const char *backend_lib = "libQnnHtp.so";
    const char *system_lib  = "libQnnSystem.so";

    /* ---- load backend + system libraries ---- */
    void *bh = dlopen(backend_lib, RTLD_NOW | RTLD_LOCAL);
    CHECK(bh, "dlopen %s: %s", backend_lib, dlerror());
    void *sh = dlopen(system_lib, RTLD_NOW | RTLD_LOCAL);
    CHECK(sh, "dlopen %s: %s", system_lib, dlerror());

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

    /* ---- read the context binary ---- */
    FILE *f = fopen(bin_path, "rb");
    CHECK(f, "cannot open %s", bin_path);
    fseek(f, 0, SEEK_END);
    long bin_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *bin_buf = malloc(bin_size);
    CHECK(bin_buf && fread(bin_buf, 1, bin_size, f) == (size_t)bin_size, "read %s", bin_path);
    fclose(f);

    /* ---- inspect graph metadata (names, tensor shapes) without guessing ---- */
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
        CHECK(0, "unsupported binary info version %d", (int)bin_info->version);
    }
    CHECK(num_graphs > 0, "no graphs in binary");

    /* GraphInfo V1/V2/V3 share the same leading members, but read via the
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

    printf("graph            : %s\n", graph_name);
    printf("context binary   : %.1f MB\n", bin_size / 1048576.0);

    /* ---- create backend/device/context: the one-time cost ---- */
    double t_load0 = now_ms();
    Qnn_BackendHandle_t backend = NULL;
    CHECK(qnn.backendCreate(NULL, NULL, &backend) == QNN_SUCCESS, "backendCreate failed");
    Qnn_DeviceHandle_t device = NULL;
    qnn.deviceCreate(NULL, NULL, &device);  /* optional on HTP */

    Qnn_ContextHandle_t context = NULL;
    CHECK(qnn.contextCreateFromBinary(backend, device, NULL, bin_buf, bin_size, &context, NULL)
              == QNN_SUCCESS,
          "contextCreateFromBinary failed -- version mismatch?");
    Qnn_GraphHandle_t graph = NULL;
    CHECK(qnn.graphRetrieve(context, graph_name, &graph) == QNN_SUCCESS, "graphRetrieve failed");
    double t_load = now_ms() - t_load0;
    printf("one-time load    : %.1f ms\n", t_load);

    /* ---- allocate I/O buffers from the graph's own metadata ---- */
    size_t in_bytes = tensor_elems(&in_tensors[0]) * dtype_size(in_tensors[0].v1.dataType);
    size_t out_bytes = tensor_elems(&out_tensors[0]) * dtype_size(out_tensors[0].v1.dataType);

    Qnn_Tensor_t *ins = calloc(num_in, sizeof(Qnn_Tensor_t));
    Qnn_Tensor_t *outs = calloc(num_out, sizeof(Qnn_Tensor_t));
    for (uint32_t i = 0; i < num_in; i++) {
        ins[i] = in_tensors[i];
        ins[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
        ins[i].v1.clientBuf.data = calloc(1, in_bytes);
        ins[i].v1.clientBuf.dataSize = in_bytes;
    }
    for (uint32_t i = 0; i < num_out; i++) {
        outs[i] = out_tensors[i];
        outs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
        outs[i].v1.clientBuf.data = calloc(1, out_bytes);
        outs[i].v1.clientBuf.dataSize = out_bytes;
    }

    /* ---- load the input frame ---- */
    FILE *fi = fopen(in_path, "rb");
    CHECK(fi, "cannot open %s", in_path);
    float *src = malloc(tensor_elems(&in_tensors[0]) * sizeof(float));
    size_t got = fread(src, sizeof(float), tensor_elems(&in_tensors[0]), fi);
    fclose(fi);
    CHECK(got == tensor_elems(&in_tensors[0]), "input size mismatch: got %zu floats", got);

    /* The graph takes fp16; convert once (a real pipeline does this per frame).
     * aarch64 gcc supports __fp16 natively, so the conversion is a single instruction. */
    if (in_tensors[0].v1.dataType == QNN_DATATYPE_FLOAT_16) {
        __fp16 *dst = (__fp16 *)ins[0].v1.clientBuf.data;
        for (size_t i = 0; i < tensor_elems(&in_tensors[0]); i++) {
            dst[i] = (__fp16)src[i];
        }
    } else {
        memcpy(ins[0].v1.clientBuf.data, src, in_bytes);
    }

    /* ---- steady-state loop ---- */
    double *lat = malloc(iters * sizeof(double));
    for (int i = 0; i < iters; i++) {
        double t0 = now_ms();
        Qnn_ErrorHandle_t e = qnn.graphExecute(graph, ins, num_in, outs, num_out, NULL, NULL);
        lat[i] = now_ms() - t0;
        CHECK(e == QNN_SUCCESS, "graphExecute failed at iter %d (0x%lx)", i, (unsigned long)e);
    }

    /* First iterations include HVX/HMX power-on; report warm-up separately. */
    int warm = (iters > 20) ? 10 : 1;
    double sum = 0;
    for (int i = warm; i < iters; i++) sum += lat[i];
    int n = iters - warm;
    double *sorted = malloc(n * sizeof(double));
    memcpy(sorted, lat + warm, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    printf("\n--- steady state (%d iters, first %d discarded as warm-up) ---\n", n, warm);
    printf("warm-up (iter 0) : %.2f ms\n", lat[0]);
    printf("min              : %.2f ms\n", sorted[0]);
    printf("median           : %.2f ms\n", sorted[n / 2]);
    printf("mean             : %.2f ms\n", sum / n);
    printf("p95              : %.2f ms\n", sorted[(int)(n * 0.95)]);
    printf("max              : %.2f ms\n", sorted[n - 1]);
    printf("FPS (mean)       : %.1f\n", 1000.0 / (sum / n));

    /* Dump the last output so correctness can be checked against the ONNX
     * reference -- a fast loop computing garbage would prove nothing. */
    const char *dump = getenv("QNN_BENCH_DUMP");
    if (dump) {
        size_t out_n = tensor_elems(&out_tensors[0]);
        float *fo = malloc(out_n * sizeof(float));
        if (out_tensors[0].v1.dataType == QNN_DATATYPE_FLOAT_16) {
            const __fp16 *s16 = (const __fp16 *)outs[0].v1.clientBuf.data;
            for (size_t i = 0; i < out_n; i++) fo[i] = (float)s16[i];
        } else {
            memcpy(fo, outs[0].v1.clientBuf.data, out_n * sizeof(float));
        }
        FILE *fd = fopen(dump, "wb");
        if (fd) { fwrite(fo, sizeof(float), out_n, fd); fclose(fd); }
        printf("output written   : %s (%zu floats)\n", dump, out_n);
        free(fo);
    }

    qnn.contextFree(context, NULL);
    qnn.backendFree(backend);
    sys.systemContextFree(sys_ctx);
    return 0;
}
