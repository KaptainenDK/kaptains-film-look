// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Mock host test of the plug-in glue (not part of the plug-in)
#include "KaptainFilm.cpp"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static PF_PixelFormat g_fmt;
static unsigned g_groups = 7; static int g_plan_calls = 0; static size_t g_plan_ops = 0; static int g_has_cur = 0, g_apply_vig_seen = 0;
static int g_vig_calls = 0; static KFGpuFrameParams g_vig_seen; static size_t g_passes_seen = 0;
static PF_Cmd g_smart_cmd = PF_Cmd_SMART_RENDER; static PF_GPU_Framework g_what_gpu = PF_GPU_Framework_NONE; static int g_gpu_calls = 0;
// Stand-ins for the Metal side (KaptainFilmGPU.mm, macOS only): the "GPU buffers" are plain float4 B,G,R,A arrays.
// (Premiere bridge executor is not part of this test)
PF_Err kf_gpu_render_frame(PF_InData*, PF_OutData*, PF_SmartRenderExtra*, PF_EffectWorld*, PF_EffectWorld*, KFGpuFrameParams p, const std::vector<KFGpuPass>& passes, const KFPlan* plan) {
    if (plan) { ++g_plan_calls; g_plan_ops = plan->ops.size(); } else ++g_vig_calls;
    g_vig_seen = p; g_passes_seen = passes.size(); g_has_cur = p.hasCur; g_apply_vig_seen = p.applyVig; return PF_Err_NONE; }
unsigned kf_gpu_groups(PF_SmartRenderExtra*) { return g_groups; }
PF_Err kf_gpu_device_setup(PF_InData*, PF_OutData*, PF_GPUDeviceSetupExtra*) { return PF_Err_NONE; }
PF_Err kf_gpu_device_setdown(PF_InData*, PF_OutData*, PF_GPUDeviceSetdownExtra*) { return PF_Err_NONE; }
PF_Err kf_gpu_render(PF_InData*, PF_OutData*, PF_SmartRenderExtra*, PF_EffectWorld* in_w, PF_EffectWorld* out_w, KFGpuCpuStage stage, void* user) {
    ++g_gpu_calls;
    stage((const float*)in_w->data, in_w->width, in_w->height, in_w->rowbytes / 16, (float*)out_w->data, out_w->width, out_w->height, out_w->rowbytes / 16, user);
    return PF_Err_NONE; }
static PF_Err mock_GetPixelFormat(const PF_EffectWorld*, PF_PixelFormat* f) { *f = g_fmt; return PF_Err_NONE; }
static PF_WorldSuite2 g_ws;
// Premiere's pixel format suite: records what GlobalSetup declares, and answers GetPixelFormat with g_prem_fmt
static PF_PixelFormatSuite1 g_pfs; static int g_fmt_count = 0; static PrPixelFormat g_fmt_added = PrPixelFormat_Invalid, g_prem_fmt = PrPixelFormat_BGRA_4444_32f;
static PF_Err mock_AddFmt(PF_ProgPtr, PrPixelFormat f) { ++g_fmt_count; g_fmt_added = f; return PF_Err_NONE; }
static PF_Err mock_ClearFmt(PF_ProgPtr) { g_fmt_count = 0; return PF_Err_NONE; }
static PF_Err mock_GetPrFmt(PF_EffectWorld*, PrPixelFormat* f) { *f = g_prem_fmt; return PF_Err_NONE; }
static SPErr mock_Acquire(const char* name, int32 ver, const void** suite) {
    if (strcmp(name, kPFPixelFormatSuite) == 0) { g_pfs.AddSupportedPixelFormat = mock_AddFmt; g_pfs.ClearSupportedPixelFormats = mock_ClearFmt; g_pfs.GetPixelFormat = mock_GetPrFmt; *suite = &g_pfs; return 0; }
    *suite = &g_ws; return 0; }
static SPErr mock_Release(const char*, int32) { return 0; }

static int fails = 0;
static PF_RenderRequest g_req_seen;
static PF_PreRenderOutput g_pre_out;
#define CHECK(c, msg) do { bool ok_ = (c); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) fails++; } while (0)

struct Run {
    PF_InData in; PF_OutData out; PF_ParamDef defs[KF_NUM_PARAMS]; PF_ParamDef* ptrs[KF_NUM_PARAMS];
    SPBasicSuite sp; PF_LayerDef outw;
};

static void setup(Run& r, int w, int h, void* indata, void* outdata, int rowbytes, double vig, double grain, double cx = 0.5) {
    memset(&r, 0, sizeof r);
    g_ws.PF_GetPixelFormat = mock_GetPixelFormat;
    r.sp.AcquireSuite = mock_Acquire; r.sp.ReleaseSuite = mock_Release;
    r.in.pica_basicP = &r.sp; r.in.time_step = 1; r.in.current_time = 0; r.in.width = w; r.in.height = h;
    for (int i = 0; i < KF_NUM_PARAMS; ++i) r.ptrs[i] = &r.defs[i];
    PF_LayerDef& ld = r.defs[KF_INPUT].u.ld; ld.width = w; ld.height = h; ld.rowbytes = rowbytes; ld.data = (PF_PixelPtr)indata;
    r.outw.width = w; r.outw.height = h; r.outw.rowbytes = rowbytes; r.outw.data = (PF_PixelPtr)outdata;
    r.defs[KF_ZONE_CENTER].u.td.x_value = (PF_Fixed)(cx * w * 65536.0); r.defs[KF_ZONE_CENTER].u.td.y_value = (PF_Fixed)(0.5 * h * 65536.0);
    r.defs[KF_ZONE_ROUNDNESS].u.fs_d.value = 1;
    r.defs[KF_INPUT_SPACE].u.pd.value = 3;           // Linear (so values are easy to reason about)
    r.defs[KF_MIX].u.fs_d.value = 1;
    r.defs[KF_VIG_ON].u.bd.value = 1; r.defs[KF_GRAIN_ON].u.bd.value = 1;
    r.defs[KF_LENS_EDGE].u.pd.value = 2; r.defs[KF_LB_TYPE].u.pd.value = 1;
    r.defs[KF_LENS_ZOOM].u.fs_d.value = 1;
    r.defs[KF_BLOOM_TINT].u.cd.value = {255,255,255,255}; r.defs[KF_HAL_TINT].u.cd.value = {255,255,77,15};
    r.defs[KF_VIG_AMOUNT].u.fs_d.value = vig; r.defs[KF_VIG_SOFTNESS].u.fs_d.value = 0.3;
    r.defs[KF_VIG_ZONE_START].u.fs_d.value = 0.3; r.defs[KF_VIG_ZONE_FULL].u.fs_d.value = 1.0;
    r.defs[KF_GRAIN_AMOUNT].u.fs_d.value = grain; r.defs[KF_GRAIN_SIZE].u.fs_d.value = 1;
    r.defs[KF_GRAIN_ROUGHNESS].u.fs_d.value = 0; r.defs[KF_GRAIN_COLOR].u.fs_d.value = 0;
    r.defs[KF_GRAIN_MIDTONE].u.fs_d.value = 0; r.defs[KF_GRAIN_SEED].u.sd.value = 1; r.defs[KF_GRAIN_ANIMATE].u.bd.value = 1;
}

// ---- mock of the SmartFX callbacks ----
static PF_ParamDef* g_defs = nullptr;
static PF_EffectWorld g_in_world, g_out_world;
static int g_layer_w = 0, g_layer_h = 0;
static int g_world_w = 0, g_world_h = 0;   // 0 = same as in_data (test can override to mimic a wrong in_data size)
static int g_ui_registered = 0; static long g_ui_events = 0;
static PF_Err mock_register_ui(PF_ProgPtr, PF_CustomUIInfo* ci) { g_ui_registered++; g_ui_events = ci->events; return PF_Err_NONE; }
static PF_Err mock_checkout_param(PF_ProgPtr, PF_ParamIndex idx, A_long, A_long, A_u_long, PF_ParamDef* out) { *out = g_defs[idx]; return PF_Err_NONE; }
static PF_Err mock_checkin_param(PF_ProgPtr, PF_ParamDef*) { return PF_Err_NONE; }
static PF_Err mock_checkout_layer(PF_ProgPtr, PF_ParamIndex, A_long, const PF_RenderRequest* req, A_long, A_long, A_u_long, PF_CheckoutResult* res) {
    // the plug-in must ask for the whole layer, whatever AE asked for
    g_req_seen = *req;
    // like AE: the result is the request, which can exceed the layer (padding) if the plug-in asks for too much
    int rw = std::max(g_layer_w, (int)req->rect.right), rh = std::max(g_layer_h, (int)req->rect.bottom);
    res->result_rect = {0, 0, rw, rh}; res->max_result_rect = res->result_rect; return PF_Err_NONE; }
static PF_Err mock_checkout_layer_pixels(PF_ProgPtr, A_long, PF_EffectWorld** w) { *w = &g_in_world; return PF_Err_NONE; }
static PF_Err mock_checkin_layer_pixels(PF_ProgPtr, A_long) { return PF_Err_NONE; }
static PF_Err mock_checkout_output(PF_ProgPtr, PF_EffectWorld** w) { *w = &g_out_world; return PF_Err_NONE; }

// Runs PRE_RENDER + SMART_RENDER. The output buffer may be a sub-rectangle (ox, oy, ow, oh) of the layer.
static PF_Err run_smart(Run& r, void* indata, void* outdata, int rowbytes, int ox, int oy, int ow, int oh, int bytes_per_px) {
    g_defs = r.defs; g_layer_w = g_world_w ? g_world_w : r.in.width; g_layer_h = g_world_h ? g_world_h : r.in.height;
    r.in.inter.checkout_param = mock_checkout_param; r.in.inter.checkin_param = mock_checkin_param;
    memset(&g_in_world, 0, sizeof g_in_world); memset(&g_out_world, 0, sizeof g_out_world);
    const int ww = g_world_w ? g_world_w : r.in.width, wh = g_world_h ? g_world_h : r.in.height;
    g_in_world.width = ww; g_in_world.height = wh; g_in_world.rowbytes = ww * bytes_per_px; g_in_world.data = (PF_PixelPtr)indata;
    g_out_world.width = ow; g_out_world.height = oh; g_out_world.rowbytes = ow * bytes_per_px; g_out_world.data = (PF_PixelPtr)outdata;
    g_out_world.origin_x = ox; g_out_world.origin_y = oy;
    PF_PreRenderInput pin; memset(&pin, 0, sizeof pin); pin.output_request.rect = {oy, ox, ox + ow, oy + oh};
    pin.what_gpu = g_what_gpu; pin.output_request.rect.left = ox; pin.output_request.rect.top = oy; pin.output_request.rect.right = ox + ow; pin.output_request.rect.bottom = oy + oh;
    PF_PreRenderOutput pout; memset(&pout, 0, sizeof pout);
    PF_PreRenderCallbacks pcb; memset(&pcb, 0, sizeof pcb); pcb.checkout_layer = mock_checkout_layer;
    PF_PreRenderExtra pex = {&pin, &pout, &pcb};
    PF_Err e = EffectMain(PF_Cmd_SMART_PRE_RENDER, &r.in, &r.out, r.ptrs, nullptr, &pex);
    if (e) return e;
    g_pre_out = pout;
    PF_SmartRenderInput sin; memset(&sin, 0, sizeof sin); sin.output_request = pin.output_request; sin.pre_render_data = pout.pre_render_data; sin.what_gpu = g_what_gpu;
    PF_SmartRenderCallbacks scb; scb.checkout_layer_pixels = mock_checkout_layer_pixels; scb.checkin_layer_pixels = mock_checkin_layer_pixels; scb.checkout_output = mock_checkout_output;
    PF_SmartRenderExtra sex = {&sin, &scb};
    e = EffectMain(g_smart_cmd, &r.in, &r.out, r.ptrs, nullptr, &sex);
    if (pout.delete_pre_render_data_func && pout.pre_render_data) pout.delete_pre_render_data_func(pout.pre_render_data);
    return e;
}

static int g_param_count = 0; static std::vector<int> g_ids;
static PF_Err mock_add_param(PF_ProgPtr, A_long, PF_ParamDefPtr def) { ++g_param_count; if (def->uu.id) g_ids.push_back(def->uu.id); return PF_Err_NONE; }

int main() {
    const int W = 64, H = 36;
    // ---- 8 bit, opaque grey 0.5 ----
    { std::vector<PF_Pixel8> a(W * H), b(W * H);
      for (auto& p : a) { p.alpha = 255; p.red = p.green = p.blue = 128; }
      Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_Pixel8), 2.0, 0.0); g_fmt = PF_PixelFormat_ARGB32;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      PF_Pixel8 c = b[(H / 2) * W + W / 2], k = b[0];
      CHECK(abs(c.red - 128) <= 1, "8-bit: centre unchanged");
      CHECK(k.red < 50 && k.alpha == 255, "8-bit: corner darkened by vignette, alpha kept");
      CHECK(k.red == k.green && k.green == k.blue, "8-bit: grey stays grey"); }
    // ---- 16 bit ----
    { std::vector<PF_Pixel16> a(W * H), b(W * H);
      for (auto& p : a) { p.alpha = 32768; p.red = p.green = p.blue = 16384; }
      Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_Pixel16), 2.0, 0.0); g_fmt = PF_PixelFormat_ARGB64;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      CHECK(abs((int)b[(H / 2) * W + W / 2].red - 16384) <= 2, "16-bit: centre unchanged");
      CHECK(b[0].red < 6000 && b[0].alpha == 32768, "16-bit: corner darkened, alpha kept"); }
    // ---- 32 bit float, with a value > 1 (HDR) ----
    { std::vector<PF_PixelFloat> a(W * H), b(W * H);
      for (auto& p : a) { p.alpha = 1; p.red = 3.0f; p.green = 2.0f; p.blue = 1.0f; }
      Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_PixelFloat), 1.0, 0.0); g_fmt = PF_PixelFormat_ARGB128;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      PF_PixelFloat c = b[(H / 2) * W + W / 2], k = b[0];
      CHECK(fabs(c.red - 3.0f) < 1e-3f && fabs(c.green - 2.0f) < 1e-3f, "float: HDR values > 1 kept in the centre");
      { double R = std::hypot(W / 2.0, H / 2.0), d = std::hypot(0.5 - W / 2.0, 0.5 - H / 2.0) / R, t = (d - 0.3) / 0.7, pw = 1 + 3 * (1 - 0.3);
        double expect = std::pow(2.0, -1.0 * std::pow(t, pw));
        CHECK(fabs(k.red / 3.0 - expect) < 0.005, "float: corner gain matches the vignette formula"); } }
    // ---- premultiplied alpha ----
    { std::vector<PF_Pixel8> a(W * H), b(W * H);
      for (auto& p : a) { p.alpha = 128; p.red = p.green = p.blue = 64; }   // straight 0.5 grey at 50% alpha, premultiplied
      Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_Pixel8), 0.0, 0.0); g_fmt = PF_PixelFormat_ARGB32;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      PF_Pixel8 c = b[(H / 2) * W + W / 2];
      CHECK(abs(c.alpha - 128) <= 1 && abs(c.red - 64) <= 1, "premultiplied alpha survives a round trip with no effect"); }
    // ---- moved zone centre (point parameter) ----
    { std::vector<PF_PixelFloat> a(W * H), b(W * H);
      for (auto& p : a) { p.alpha = 1; p.red = p.green = p.blue = 1.0f; }
      Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_PixelFloat), 3.0, 0.0, 0.2); g_fmt = PF_PixelFormat_ARGB128;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      float left = b[(H / 2) * W + (int)(0.2 * W)].red, right = b[(H / 2) * W + W - 1].red;
      CHECK(left > 0.99f && right < 0.2f, "zone centre point parameter moves the bright area"); }
    // ---- grain: changes pixels, differs per frame, same frame identical ----
    { std::vector<PF_Pixel8> a(W * H), b1(W * H), b2(W * H), b3(W * H);
      for (auto& p : a) { p.alpha = 255; p.red = p.green = p.blue = 128; }
      Run r; setup(r, W, H, a.data(), b1.data(), W * sizeof(PF_Pixel8), 0.0, 2.0); g_fmt = PF_PixelFormat_ARGB32;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      r.outw.data = (PF_PixelPtr)b2.data(); EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      r.in.current_time = 1; r.outw.data = (PF_PixelPtr)b3.data(); EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      int diff = 0, same = 0; for (int i = 0; i < W * H; ++i) { diff += b1[i].red != 128; same += b1[i].red == b2[i].red; }
      int dframe = 0; for (int i = 0; i < W * H; ++i) dframe += b1[i].red != b3[i].red;
      CHECK(diff > W * H / 2, "grain modifies the picture");
      CHECK(same == W * H, "same frame gives identical grain");
      CHECK(dframe > W * H / 4, "next frame gives different grain"); }
    // ---- SmartFX path: 32-bit HDR gives the same result as the classic path ----
    { std::vector<PF_PixelFloat> a(W * H), c(W * H), s(W * H);
      for (int i = 0; i < W * H; ++i) { a[i].alpha = 1; a[i].red = 3.0f * (i % W) / W; a[i].green = 2.0f; a[i].blue = 0.5f; }
      Run r; setup(r, W, H, a.data(), c.data(), W * sizeof(PF_PixelFloat), 1.5, 1.0); g_fmt = PF_PixelFormat_ARGB128;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      PF_Err e = run_smart(r, a.data(), s.data(), 0, 0, 0, W, H, sizeof(PF_PixelFloat));
      CHECK(e == PF_Err_NONE, "smart: pre-render + render return no error");
      CHECK(memcmp(c.data(), s.data(), c.size() * sizeof(PF_PixelFloat)) == 0, "smart: 32-bit float identical to the classic path");
      CHECK(g_req_seen.rect.left == 0 && g_req_seen.rect.top == 0 && g_req_seen.rect.right == W && g_req_seen.rect.bottom == H, "smart: whole layer requested from the input");
      CHECK(g_pre_out.result_rect.right == W && g_pre_out.result_rect.bottom == H && (g_pre_out.flags & PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS), "smart: whole layer reported as result, extra pixels flagged");
      CHECK(g_pre_out.delete_pre_render_data_func != nullptr, "smart: pre-render data has a delete function"); }
    // ---- SmartFX path: in_data->width/height may be wrong (full-res vs reduced); the zone centre must still be right ----
    { std::vector<PF_PixelFloat> a(W * H), c(W * H), s(W * H);
      for (auto& p : a) { p.alpha = 1; p.red = p.green = p.blue = 1.0f; }
      Run r; setup(r, W, H, a.data(), c.data(), W * sizeof(PF_PixelFloat), 3.0, 0.0, 0.3); g_fmt = PF_PixelFormat_ARGB128;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      r.in.width = W * 2; r.in.height = H * 2;      // pretend in_data reports the full-res size while the buffers are half-res
      r.in.downsample_x.num = 1; r.in.downsample_x.den = 2; r.in.downsample_y.num = 1; r.in.downsample_y.den = 2;
      g_world_w = W; g_world_h = H;
      run_smart(r, a.data(), s.data(), 0, 0, 0, W, H, sizeof(PF_PixelFloat));
      g_world_w = g_world_h = 0;
      CHECK(memcmp(c.data(), s.data(), c.size() * sizeof(PF_PixelFloat)) == 0, "smart: zone centre does not depend on in_data->width (real buffer size is used)");
      CHECK(g_req_seen.rect.right == W && g_req_seen.rect.bottom == H, "smart: at 1/2 resolution the request is the half-res layer size, not the full-res in_data size (AE padded the buffer otherwise)");
      CHECK(g_pre_out.result_rect.right == W && g_pre_out.result_rect.bottom == H, "smart: half-res result rect is exactly the layer (no padding)"); }
    // ---- odd sizes at 1/34 (as seen in AE): 3840x2160 -> 113x64 ----
    { Run r; memset(&r, 0, sizeof r); r.in.width = 3840; r.in.height = 2160; r.in.downsample_x.num = 1; r.in.downsample_x.den = 34; r.in.downsample_y.num = 1; r.in.downsample_y.den = 34;
      CHECK(scaled_size(r.in.width, r.in.downsample_x) == 113 && scaled_size(r.in.height, r.in.downsample_y) == 64, "downsample 1/34 gives 113x64 like AE's own request"); }
    // ---- SmartFX path: 8-bit and 16-bit ----
    { std::vector<PF_Pixel8> a(W * H), c(W * H), s(W * H);
      for (int i = 0; i < W * H; ++i) { a[i].alpha = 255; a[i].red = (A_u_char)(i % 200); a[i].green = 100; a[i].blue = 50; }
      Run r; setup(r, W, H, a.data(), c.data(), W * sizeof(PF_Pixel8), 1.0, 1.0); g_fmt = PF_PixelFormat_ARGB32;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      run_smart(r, a.data(), s.data(), 0, 0, 0, W, H, sizeof(PF_Pixel8));
      CHECK(memcmp(c.data(), s.data(), c.size() * sizeof(PF_Pixel8)) == 0, "smart: 8-bit identical to the classic path"); }
    { std::vector<PF_Pixel16> a(W * H), c(W * H), s(W * H);
      for (int i = 0; i < W * H; ++i) { a[i].alpha = 32768; a[i].red = (A_u_short)(i * 7 % 30000); a[i].green = 9000; a[i].blue = 4000; }
      Run r; setup(r, W, H, a.data(), c.data(), W * sizeof(PF_Pixel16), 1.0, 1.0); g_fmt = PF_PixelFormat_ARGB64;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      run_smart(r, a.data(), s.data(), 0, 0, 0, W, H, sizeof(PF_Pixel16));
      CHECK(memcmp(c.data(), s.data(), c.size() * sizeof(PF_Pixel16)) == 0, "smart: 16-bit identical to the classic path"); }
    // ---- SmartFX path: AE asks for a sub-rectangle only; the picture must still be computed on the whole layer ----
    { std::vector<PF_PixelFloat> a(W * H), full(W * H), part(20 * 10);
      for (int i = 0; i < W * H; ++i) { a[i].alpha = 1; a[i].red = a[i].green = a[i].blue = 1.0f; }
      Run r; setup(r, W, H, a.data(), full.data(), W * sizeof(PF_PixelFloat), 3.0, 0.0); g_fmt = PF_PixelFormat_ARGB128;
      run_smart(r, a.data(), full.data(), 0, 0, 0, W, H, sizeof(PF_PixelFloat));
      run_smart(r, a.data(), part.data(), 0, 40, 20, 20, 10, sizeof(PF_PixelFloat));
      bool same = true; for (int y = 0; y < 10; ++y) for (int x = 0; x < 20; ++x) same = same && part[y * 20 + x].red == full[(20 + y) * W + 40 + x].red;
      CHECK(same, "smart: cropped output rectangle matches the same area of the full render (origin handled)");
      CHECK(part[9 * 20 + 19].red < 0.9f && part[0].red > 0.99f, "smart: crop is darkened only where the whole-layer zone says so (zones use the whole layer, not the crop)"); }
    // ---- milestone B: every effect reaches the core, Off really means off ----
    { const int W2 = 96, H2 = 54;
      std::vector<PF_PixelFloat> a(W2 * H2), off(W2 * H2), on(W2 * H2), sm(W2 * H2);
      for (int y = 0; y < H2; ++y) for (int x = 0; x < W2; ++x) { PF_PixelFloat& p = a[y * W2 + x];
          p.alpha = 1; p.red = p.green = p.blue = (x > 40 && x < 56 && y > 20 && y < 34) ? 3.0f : 0.3f; }
      struct E { const char* name; int on_idx; void (*cfg)(Run&); };
      auto cfg_lens = [](Run& r) { r.defs[KF_LENS_AMOUNT].u.fs_d.value = 0.5; };
      auto cfg_ca = [](Run& r) { r.defs[KF_CA_AMOUNT].u.fs_d.value = 2.0; };
      auto cfg_lb = [](Run& r) { r.defs[KF_LB_AMOUNT].u.fs_d.value = 3.0; r.defs[KF_LB_ZONE_START].u.fs_d.value = 0.0; r.defs[KF_LB_ZONE_FULL].u.fs_d.value = 0.0; };
      auto cfg_bloom = [](Run& r) { r.defs[KF_BLOOM_THRESHOLD].u.fs_d.value = 0.5; r.defs[KF_BLOOM_INTENSITY].u.fs_d.value = 1.0; r.defs[KF_BLOOM_RADIUS].u.fs_d.value = 5.0; };
      auto cfg_hal = [](Run& r) { r.defs[KF_HAL_THRESHOLD].u.fs_d.value = 0.5; r.defs[KF_HAL_INTENSITY].u.fs_d.value = 1.0; r.defs[KF_HAL_RADIUS].u.fs_d.value = 5.0; };
      E es[] = {{"lens distortion", KF_LENS_ON, cfg_lens}, {"chromatic aberration", KF_CA_ON, cfg_ca}, {"lens blur", KF_LB_ON, cfg_lb},
                {"bloom", KF_BLOOM_ON, cfg_bloom}, {"halation", KF_HAL_ON, cfg_hal}};
      for (auto& e : es) {
          Run r; setup(r, W2, H2, a.data(), off.data(), W2 * sizeof(PF_PixelFloat), 0.0, 0.0); g_fmt = PF_PixelFormat_ARGB128;
          r.defs[KF_VIG_ON].u.bd.value = 0; r.defs[KF_GRAIN_ON].u.bd.value = 0; e.cfg(r);
          r.defs[e.on_idx].u.bd.value = 0;
          EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
          CHECK(memcmp(a.data(), off.data(), a.size() * sizeof(PF_PixelFloat)) == 0, (std::string(e.name) + ": Off leaves the picture untouched").c_str());
          r.defs[e.on_idx].u.bd.value = 1; r.outw.data = (PF_PixelPtr)on.data();
          EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
          CHECK(memcmp(a.data(), on.data(), a.size() * sizeof(PF_PixelFloat)) != 0, (std::string(e.name) + ": On changes the picture").c_str());
          run_smart(r, a.data(), sm.data(), 0, 0, 0, W2, H2, sizeof(PF_PixelFloat));
          CHECK(memcmp(on.data(), sm.data(), on.size() * sizeof(PF_PixelFloat)) == 0, (std::string(e.name) + ": SmartFX identical to the classic path").c_str()); }
      // Mix = 0 gives back the original even with effects on
      Run r; setup(r, W2, H2, a.data(), on.data(), W2 * sizeof(PF_PixelFloat), 2.0, 0.0); g_fmt = PF_PixelFormat_ARGB128;
      r.defs[KF_BLOOM_ON].u.bd.value = 1; r.defs[KF_BLOOM_THRESHOLD].u.fs_d.value = 0.5; r.defs[KF_BLOOM_INTENSITY].u.fs_d.value = 1; r.defs[KF_BLOOM_RADIUS].u.fs_d.value = 5;
      r.defs[KF_MIX].u.fs_d.value = 0;
      EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr);
      double dmax = 0; for (size_t i = 0; i < a.size(); ++i) dmax = std::max(dmax, (double)fabs(a[i].red - on[i].red));
      CHECK(dmax < 1e-4, "Mix 0 returns the original"); }
    // ---- parameter list: ParamsSetup adds exactly KF_NUM_PARAMS - 1 params, unique disk ids ----
    { Run r; memset(&r, 0, sizeof r); g_param_count = 0; g_ids.clear(); r.in.inter.add_param = mock_add_param; r.in.inter.register_ui = mock_register_ui;
      EffectMain(PF_Cmd_PARAMS_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK(r.out.num_params == KF_NUM_PARAMS, "ParamsSetup reports KF_NUM_PARAMS");
      CHECK(g_param_count == KF_NUM_PARAMS - 1, "ParamsSetup adds one param per enum slot (UI order matches the enum)");
      bool uniq = true; for (size_t i = 0; i < g_ids.size(); ++i) for (size_t j = i + 1; j < g_ids.size(); ++j) uniq = uniq && g_ids[i] != g_ids[j];
      CHECK(uniq, "disk ids are unique"); }
    // ---- global setup / flags ----
    { Run r; memset(&r, 0, sizeof r); EffectMain(PF_Cmd_GLOBAL_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK((r.out.out_flags & PF_OutFlag_NON_PARAM_VARY) && (r.out.out_flags & PF_OutFlag_DEEP_COLOR_AWARE), "global setup flags (match PiPL 0x02008004)");
      CHECK(r.out.out_flags2 == 0x0A001400, "global setup flags2 (match PiPL 0x0A001400: GPU F32 added)");
      CHECK((r.out.out_flags2 & PF_OutFlag2_FLOAT_COLOR_AWARE) && (r.out.out_flags2 & PF_OutFlag2_SUPPORTS_SMART_RENDER), "FLOAT_COLOR_AWARE comes with SUPPORTS_SMART_RENDER (the After Effects error)");
      // the PiPL text must carry exactly the same flag values as the code
      FILE* f = fopen("KaptainFilmPiPL.r", "r"); std::string txt; if (f) { char buf[512]; while (fgets(buf, sizeof buf, f)) txt += buf; fclose(f); }
      char want1[64], want2[64]; snprintf(want1, sizeof want1, "AE_Effect_Global_OutFlags { 0x%08X }", (unsigned)r.out.out_flags);
      snprintf(want2, sizeof want2, "AE_Effect_Global_OutFlags_2 { 0x%08X }", (unsigned)r.out.out_flags2);
      CHECK(txt.find(want1) != std::string::npos, "PiPL OutFlags equals the code's out_flags");
      CHECK(txt.find(want2) != std::string::npos, "PiPL OutFlags_2 equals the code's out_flags2"); }
    // ---- build 8: zone overlay ----
    { Run r; memset(&r, 0, sizeof r); EffectMain(PF_Cmd_GLOBAL_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK((r.out.out_flags & PF_OutFlag_CUSTOM_UI) != 0, "CUSTOM_UI flag set (zone overlay)"); }
    { Run r; memset(&r, 0, sizeof r); g_ui_registered = 0; r.in.inter.add_param = mock_add_param; r.in.inter.register_ui = mock_register_ui;
      EffectMain(PF_Cmd_PARAMS_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK(g_ui_registered == 1, "ParamsSetup registers the custom UI once");
      CHECK((g_ui_events & PF_CustomEFlag_COMP) && (g_ui_events & PF_CustomEFlag_LAYER), "custom UI listens to comp + layer windows"); }
    { // ring geometry equals the core's zone distance for every angle, level and roundness
      double worst = 0;
      for (double round : {0.0, 0.3, 1.0}) for (double level : {0.2, 0.5, 1.0}) for (int k = 0; k < 64; ++k) {
          KFRingGeom g{960, 540, 960, 540, round}; double x, y; kf_ring_point(g, 2 * M_PI * k / 64, level, &x, &y);
          double dx = x - g.cx, dy = y - g.cy, Ra = std::hypot(g.hw, g.hh);
          double d = (std::hypot(dx / g.hw, dy / g.hh) * 0.70710678) * (1 - round) + (std::hypot(dx, dy) / Ra) * round;
          worst = std::max(worst, std::fabs(d - level)); }
      CHECK(worst < 1e-6, "overlay ring points sit exactly on the core's zone distance");
      KFRingGeom c{0, 0, 960, 540, 1.0}; double x, y; kf_ring_point(c, 0, 0.5, &x, &y);
      CHECK(std::fabs(x - 0.5 * std::hypot(960.0, 540.0)) < 1e-6 && std::fabs(y) < 1e-6, "round ring at 0.5 = half the half-diagonal"); }
    { KFInfo i; memset(&i, 0, sizeof i); KFOverlayRing rg[7];
      CHECK(kf_overlay_rings(i, rg) == 0, "no enabled effects -> no rings");
      i.vig_on = 1; i.vig_amount = 0.5f; i.vig_zone = {0.3f, 1.f}; i.grain_on = 1; i.g_amount = 0.2f; i.g_zone = {0.f, 0.f};
      CHECK(kf_overlay_rings(i, rg) == 1 && rg[0].r == 1.f && rg[0].g == 1.f, "only effects with a zone (not 0/0) and an effect amount get rings (vignette yellow)");
      i.vig_on = 0; CHECK(kf_overlay_rings(i, rg) == 0, "disabled effect gets no ring"); }
    { // Premiere never draws (no comp overlay there); a null event is ignored
      Run r; memset(&r, 0, sizeof r); r.in.appl_id = kAppID_Premiere; PF_EventExtra ev; memset(&ev, 0, sizeof ev); ev.e_type = PF_Event_DRAW;
      PF_Err e = EffectMain(PF_Cmd_EVENT, &r.in, &r.out, r.ptrs, nullptr, &ev);
      CHECK(e == PF_Err_NONE && !(ev.evt_out_flags & PF_EO_HANDLED_EVENT), "Premiere: draw event ignored");
      CHECK(EffectMain(PF_Cmd_EVENT, &r.in, &r.out, r.ptrs, nullptr, nullptr) == PF_Err_NONE, "null event extra is safe"); }
    // ---- build 9: GPU skeleton ----
    { static Run r; memset(&r, 0, sizeof r); r.sp.AcquireSuite = mock_Acquire; r.sp.ReleaseSuite = mock_Release; r.in.pica_basicP = &r.sp;
      r.in.appl_id = kAppID_Premiere; EffectMain(PF_Cmd_GLOBAL_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK(!(r.out.out_flags2 & PF_OutFlag2_SUPPORTS_GPU_RENDER_F32), "Premiere: GPU flag not announced (AE-only path)");
      CHECK(g_fmt_count == 1 && g_fmt_added == PrPixelFormat_BGRA_4444_32f, "Premiere: declares 32-bit float BGRA (what makes Premiere offer the GPU filter)");
      g_fmt_count = 0; r.in.appl_id = 0; EffectMain(PF_Cmd_GLOBAL_SETUP, &r.in, &r.out, r.ptrs, nullptr, nullptr);
      CHECK(g_fmt_count == 0, "After Effects: no Premiere pixel formats declared"); }
    { // Premiere BGRA float frames on the CPU path give the same picture as After Effects ARGB float frames
      const int W = 40, H = 24; std::vector<PF_PixelFloat> a(W * H), b(W * H); std::vector<KFPremPixel> pa(W * H), pb(W * H);
      for (int i = 0; i < W * H; ++i) {
          float al = (i % 7 == 0) ? 0.5f : 1.f; a[i].alpha = al; a[i].red = (0.2f + 0.01f * (i % 50)) * al; a[i].green = 0.4f * al; a[i].blue = (0.9f - 0.005f * (i % 90)) * al;
          pa[i].a = a[i].alpha; pa[i].r = a[i].red; pa[i].g = a[i].green; pa[i].b = a[i].blue; }
      { Run r; setup(r, W, H, a.data(), b.data(), W * sizeof(PF_PixelFloat), 1.5, 0.0); g_fmt = PF_PixelFormat_ARGB128;
        EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr); }
      { static Run r; setup(r, W, H, pa.data(), pb.data(), W * sizeof(KFPremPixel), 1.5, 0.0); r.in.appl_id = kAppID_Premiere; g_prem_fmt = PrPixelFormat_BGRA_4444_32f;
        EffectMain(PF_Cmd_RENDER, &r.in, &r.out, r.ptrs, &r.outw, nullptr); }
      bool same = true; for (int i = 0; i < W * H; ++i) same = same && pb[i].a == b[i].alpha && pb[i].r == b[i].red && pb[i].g == b[i].green && pb[i].b == b[i].blue;
      CHECK(same, "Premiere BGRA 32f CPU render == After Effects ARGB 32f render (channels in the right places)"); }
    { const int GW = 48, GH = 30;      // GPU result must equal the CPU float result, pixel for pixel
      std::vector<PF_PixelFloat> a(GW * GH), c(GW * GH); std::vector<float> ga(GW * GH * 4), gd(GW * GH * 4);
      for (int i = 0; i < GW * GH; ++i) {
          float al = (i % 7 == 0) ? 0.5f : (i % 11 == 0 ? 0.f : 1.f);
          a[i].alpha = al; a[i].red = (0.2f + 0.01f * (i % 50)) * al; a[i].green = 0.4f * al; a[i].blue = (0.9f - 0.005f * (i % 90)) * al;
          ga[i * 4 + 0] = a[i].blue; ga[i * 4 + 1] = a[i].green; ga[i * 4 + 2] = a[i].red; ga[i * 4 + 3] = a[i].alpha; }
      Run r; setup(r, GW, GH, a.data(), c.data(), GW * sizeof(PF_PixelFloat), 1.5, 1.0); g_fmt = PF_PixelFormat_ARGB128;
      r.defs[KF_SHOW_ZONES].u.bd.value = 1;   // burned-in zone rings are drawn on the CPU: forces the CPU-stage GPU path under test here
      g_smart_cmd = PF_Cmd_SMART_RENDER; g_what_gpu = PF_GPU_Framework_NONE;
      run_smart(r, a.data(), c.data(), 0, 0, 0, GW, GH, sizeof(PF_PixelFloat));
      CHECK(!(g_pre_out.flags & PF_RenderOutputFlag_GPU_RENDER_POSSIBLE), "pre-render: no GPU flag when AE renders on the CPU");
      g_fmt = PF_PixelFormat_GPU_BGRA128; g_smart_cmd = PF_Cmd_SMART_RENDER_GPU; g_what_gpu = PF_GPU_Framework_METAL; g_gpu_calls = 0;
      PF_Err e = run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(e == PF_Err_NONE && g_gpu_calls == 1, "GPU smart render runs the GPU path once");
      CHECK((g_pre_out.flags & PF_RenderOutputFlag_GPU_RENDER_POSSIBLE) != 0, "pre-render: GPU possible when AE picked Metal");
      double worst = 0; for (int i = 0; i < GW * GH; ++i) {
          worst = std::max(worst, (double)fabsf(gd[i * 4 + 0] - c[i].blue)); worst = std::max(worst, (double)fabsf(gd[i * 4 + 1] - c[i].green));
          worst = std::max(worst, (double)fabsf(gd[i * 4 + 2] - c[i].red)); worst = std::max(worst, (double)fabsf(gd[i * 4 + 3] - c[i].alpha)); }
      CHECK(worst < 1e-6, "GPU path output equals the CPU float output (B,G,R,A premultiplied)");
      // wrong pixel format on the GPU path is refused, not rendered
      g_fmt = PF_PixelFormat_ARGB128; g_gpu_calls = 0; e = run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(e != PF_Err_NONE && g_gpu_calls == 0, "GPU path refuses a non-GPU pixel format");
      // cropped output on the GPU path: same area as the full render
      std::vector<float> part(20 * 10 * 4); g_fmt = PF_PixelFormat_GPU_BGRA128;
      run_smart(r, ga.data(), part.data(), 0, 20, 10, 20, 10, 16);
      bool same = true; for (int y = 0; y < 10; ++y) for (int x = 0; x < 20; ++x) same = same && fabsf(part[(y * 20 + x) * 4 + 2] - gd[((10 + y) * GW + 20 + x) * 4 + 2]) < 1e-6;
      CHECK(same, "GPU path: cropped output rectangle matches the full render");
      g_smart_cmd = PF_Cmd_SMART_RENDER; g_what_gpu = PF_GPU_Framework_NONE; g_fmt = PF_PixelFormat_ARGB128; }
    // ---- build 10 / step 2a: vignette-only frames stay entirely on the GPU ----
    { const int GW = 48, GH = 30; std::vector<float> ga(GW * GH * 4, 0.5f), gd(GW * GH * 4);
      Run r; setup(r, GW, GH, ga.data(), gd.data(), GW * 16, 1.5, 1.0, 0.3); r.defs[KF_GRAIN_ON].u.bd.value = 0;
      r.defs[KF_VIG_SOFTNESS].u.fs_d.value = 0.4; r.defs[KF_MIX].u.fs_d.value = 0.75; r.defs[KF_ZONE_ROUNDNESS].u.fs_d.value = 0.6;
      g_fmt = PF_PixelFormat_GPU_BGRA128; g_smart_cmd = PF_Cmd_SMART_RENDER_GPU; g_what_gpu = PF_GPU_Framework_METAL; g_gpu_calls = 0; g_vig_calls = 0;
      run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_calls == 1 && g_gpu_calls == 0, "vignette only: whole frame on the GPU (no CPU stage)");
      CHECK(g_vig_seen.applyVig == 1 && g_vig_seen.inputSpace == 0 && g_vig_seen.zoneEverywhere == 0, "kernel params: vignette on, linear, zone set");
      CHECK(fabsf(g_vig_seen.amount - 1.5f) < 1e-6 && fabsf(g_vig_seen.mix - 0.75f) < 1e-6 && fabsf(g_vig_seen.softness - 0.4f) < 1e-6 && fabsf(g_vig_seen.roundness - 0.6f) < 1e-6, "kernel params: amount, mix, softness, roundness");
      CHECK(fabsf(g_vig_seen.cx - 0.3f * GW) < 1e-3 && fabsf(g_vig_seen.cy - 0.5f * GH) < 1e-3 && g_vig_seen.width == GW && g_vig_seen.srcPitch == GW, "kernel params: zone centre in pixels, size, pitch");
      r.defs[KF_INPUT_SPACE].u.pd.value = 1; g_vig_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_seen.inputSpace == 1, "kernel params: sRGB input space maps to 1");
      r.defs[KF_INPUT_SPACE].u.pd.value = 2; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_seen.inputSpace == 2, "kernel params: Rec.709 maps to gamma 2.4");
      CHECK(g_vig_seen.applyGrain == 0 && g_passes_seen == 0, "grain off: no noise passes");
      // grain is on the GPU too (step 2b): noise passes are planned
      r.defs[KF_GRAIN_ON].u.bd.value = 1; r.defs[KF_GRAIN_AMOUNT].u.fs_d.value = 1.0; r.defs[KF_GRAIN_SIZE].u.fs_d.value = 40; r.defs[KF_GRAIN_ROUGHNESS].u.fs_d.value = 0.5; r.defs[KF_GRAIN_COLOR].u.fs_d.value = 0.5;
      g_vig_calls = 0; g_gpu_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_calls == 1 && g_gpu_calls == 0 && g_vig_seen.applyGrain == 1 && g_vig_seen.gColor == 1, "grain on: still the whole frame on the GPU, colour grain planned");
      CHECK(g_passes_seen == 4 * 2 * 2, "grain plan: 4 layers x (fine + coarse) x (horizontal + vertical blur)");
      // any other effect -> the CPU-stage path, same as step 1
      // lens checkbox on but amount 0: nothing to do, the light path is used
      r.defs[KF_LENS_ON].u.bd.value = 1; g_vig_calls = 0; g_gpu_calls = 0; g_plan_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_calls == 1 && g_plan_calls == 0 && g_gpu_calls == 0, "lens on with amount 0: still the light whole-frame path");
      // lens with an amount: heavy chain, whole frame on the GPU
      r.defs[KF_LENS_AMOUNT].u.fs_d.value = 0.4; g_vig_calls = 0; g_gpu_calls = 0; g_plan_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_plan_calls == 1 && g_vig_calls == 0 && g_gpu_calls == 0 && g_plan_ops >= 2, "lens on: whole frame on the GPU through the kernel list (no CPU stage)");
      CHECK(g_has_cur == 1 && g_apply_vig_seen == 0, "heavy chain: frame kernel reads the working buffer, vignette is inside the chain");
      g_groups = KF_GRP_BLUR | KF_GRP_EFF; g_vig_calls = 0; g_gpu_calls = 0; g_plan_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_plan_calls == 0 && g_gpu_calls == 1, "lens kernel group missing: falls back to the CPU-stage GPU path");
      g_groups = 7;
      r.defs[KF_LB_ON].u.bd.value = 1; r.defs[KF_LB_AMOUNT].u.fs_d.value = 1.0; r.defs[KF_BLOOM_ON].u.bd.value = 1; r.defs[KF_BLOOM_INTENSITY].u.fs_d.value = 1.0;
      g_plan_calls = 0; g_gpu_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_plan_calls == 1 && g_gpu_calls == 0, "lens blur + bloom + lens: one heavy plan");
      r.defs[KF_LB_ON].u.bd.value = 0; r.defs[KF_BLOOM_ON].u.bd.value = 0; r.defs[KF_LENS_AMOUNT].u.fs_d.value = 0;
      r.defs[KF_LENS_ON].u.bd.value = 0;
      r.defs[KF_SHOW_ZONES].u.bd.value = 1; g_vig_calls = 0; g_gpu_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 0, 0, GW, GH, 16);
      CHECK(g_vig_calls == 0 && g_gpu_calls == 1, "Show Zones (burned in) uses the CPU-stage path");
      r.defs[KF_SHOW_ZONES].u.bd.value = 0; g_vig_calls = 0; g_gpu_calls = 0; run_smart(r, ga.data(), gd.data(), 0, 20, 10, 20, 10, 16);
      CHECK(g_vig_calls == 0 && g_gpu_calls == 1, "cropped output (different area) uses the CPU-stage path");
      g_smart_cmd = PF_Cmd_SMART_RENDER; g_what_gpu = PF_GPU_Framework_NONE; g_fmt = PF_PixelFormat_ARGB128; }

    // ---- Premiere GPU bridge: the values path must give exactly what the After Effects path gives ----
    {
        const int W = 64, H = 36;
        std::vector<float> dummy((size_t)W * H * 4, 0.f);
        static Run r; setup(r, W, H, dummy.data(), dummy.data(), W * 16, 0.6, 0.4, 0.4);
        r.defs[KF_LENS_ON].u.bd.value = 1; r.defs[KF_LENS_AMOUNT].u.fs_d.value = 0.3; r.defs[KF_LENS_AUTOZOOM].u.bd.value = 1;
        r.defs[KF_CA_ON].u.bd.value = 1; r.defs[KF_CA_AMOUNT].u.fs_d.value = 0.5; r.defs[KF_CA_BLUR].u.fs_d.value = 0.2;
        r.defs[KF_LB_ON].u.bd.value = 1; r.defs[KF_LB_TYPE].u.pd.value = 3; r.defs[KF_LB_AMOUNT].u.fs_d.value = 0.7;
        r.defs[KF_LB_ANGLE].u.ad.value = (PF_Fixed)(35.0 * 65536.0);
        r.defs[KF_BLOOM_ON].u.bd.value = 1; r.defs[KF_BLOOM_INTENSITY].u.fs_d.value = 0.8; r.defs[KF_BLOOM_THRESHOLD].u.fs_d.value = 0.5;
        r.defs[KF_BLOOM_RADIUS].u.fs_d.value = 4; r.defs[KF_BLOOM_BLUR].u.fs_d.value = 1; r.defs[KF_BLOOM_STRETCH].u.fs_d.value = 0.4;
        r.defs[KF_BLOOM_STRETCH_ANGLE].u.ad.value = (PF_Fixed)(20.0 * 65536.0);
        r.defs[KF_HAL_ON].u.bd.value = 1; r.defs[KF_HAL_INTENSITY].u.fs_d.value = 0.6; r.defs[KF_HAL_THRESHOLD].u.fs_d.value = 0.6; r.defs[KF_HAL_RADIUS].u.fs_d.value = 3;
        r.defs[KF_HAL_ZONE_START].u.fs_d.value = 0.2; r.defs[KF_HAL_ZONE_FULL].u.fs_d.value = 0.9;
        r.defs[KF_GRAIN_SEED].u.sd.value = 7;
        const KFInfo ref = info_from(r.ptrs, &r.in);

        // the same parameters as Premiere would hand them out (input frame excluded), all in Premiere order
        auto values = [&](bool with_groups, std::vector<KFPremValue>& v) {
            v.clear();
            for (int k = 1; k < KF_NUM_PARAMS; ++k) {
                if (!with_groups && kf_is_group_marker(k)) continue;
                KFPremValue x; memset(&x, 0, sizeof x);
                const PF_ParamDef& d = r.defs[k];
                if (k == KF_ZONE_CENTER) { x.type = 2; x.x = d.u.td.x_value / 65536.0 / W; x.y = d.u.td.y_value / 65536.0 / H; }
                else if (k == KF_LB_ANGLE || k == KF_BLOOM_STRETCH_ANGLE || k == KF_HAL_STRETCH_ANGLE) { x.type = 1; x.num = d.u.ad.value / 65536.0; }
                else if (k == KF_BLOOM_TINT || k == KF_HAL_TINT) {
                    x.type = 3; x.raw = ((unsigned long long)d.u.cd.value.alpha << 24) | (d.u.cd.value.red << 16) | (d.u.cd.value.green << 8) | d.u.cd.value.blue; x.num = (double)x.raw; }
                else if (k == KF_INPUT_SPACE || k == KF_LENS_EDGE || k == KF_LB_TYPE) { x.type = 3; x.num = d.u.pd.value; x.raw = (unsigned long long)d.u.pd.value; }
                else if (k == KF_GRAIN_SEED) { x.type = 3; x.num = d.u.sd.value; x.raw = (unsigned long long)d.u.sd.value; }
                else if (k == KF_LENS_ON || k == KF_LENS_AUTOZOOM || k == KF_CA_ON || k == KF_LB_ON || k == KF_VIG_ON || k == KF_BLOOM_ON || k == KF_HAL_ON ||
                         k == KF_GRAIN_ON || k == KF_GRAIN_ANIMATE || k == KF_SHOW_ZONES) { x.type = 1; x.num = d.u.bd.value ? 1.0 : 0.0; }
                else if (kf_is_group_marker(k)) { x.type = 0; }
                else { x.type = 1; x.num = d.u.fs_d.value; }
                v.push_back(x);
            }
        };
        std::vector<KFPremValue> v;
        KFGpuFrameParams fa = kf_make_frame_params(make_params(ref, W, H, 0, 0), W, H, nullptr), fb; memset(&fb, 0, sizeof fb);
        const kf::Params kpa = make_params(ref, W, H, 0, 0);
        KFPlan pa; kf_build_heavy_plan(kpa, W, H, 7, pa); fa.applyVig = 0; fa.hasCur = 1;
        for (int layout = 0; layout < 2; ++layout) {
            values(layout == 1, v);
            std::vector<KFGpuPass> passes; KFPlan pb; bool use_plan = false;
            const bool ok = kf_prem_prepare(v.data(), (int)v.size(), W, H, 0, 7, fb, passes, pb, use_plan);
            CHECK(ok && use_plan, layout ? "Premiere values (with group slots): prepare ok, heavy plan" : "Premiere values (no group slots): prepare ok, heavy plan");
            CHECK(ok && memcmp(&fa, &fb, sizeof fa) == 0, "Premiere values: frame parameters identical to the After Effects path");
            CHECK(ok && pb.ops.size() == pa.ops.size() && pb.cur == pa.cur, "Premiere values: same plan as the After Effects path");
        }
        // light path (only vignette + grain)
        { static Run r2; setup(r2, W, H, dummy.data(), dummy.data(), W * 16, 0.6, 0.4, 0.4);
          const KFInfo ref2 = info_from(r2.ptrs, &r2.in);
          Run* keep = &r; (void)keep;
          std::vector<KFPremValue> v2;
          auto saved = r.defs; (void)saved;
          // reuse the converter on r2 by temporarily swapping the pointer used above
          std::swap(r, r2); values(true, v2); std::swap(r, r2);
          KFGpuFrameParams f2; std::vector<KFGpuPass> ps; KFPlan p2; bool up = true;
          const bool ok = kf_prem_prepare(v2.data(), (int)v2.size(), W, H, 0, 7, f2, ps, p2, up);
          KFGpuFrameParams e2 = kf_make_frame_params(make_params(ref2, W, H, 0, 0), W, H, nullptr);
          CHECK(ok && !up && memcmp(&e2, &f2, sizeof e2) == 0, "Premiere values: vignette + grain only = light path, same frame parameters"); }
        // things that must be refused (Premiere then renders with the CPU effect)
        values(true, v);
        { std::vector<KFPremValue> w = v; KFGpuFrameParams f; std::vector<KFGpuPass> ps; KFPlan pl; bool up;
          CHECK(!kf_prem_prepare(w.data(), (int)w.size() - 1, W, H, 0, 7, f, ps, pl, up), "unknown parameter count is refused");
          w = v; w[KF_BLOOM_TINT - 1].type = 1;
          CHECK(!kf_prem_prepare(w.data(), (int)w.size(), W, H, 0, 7, f, ps, pl, up), "colour in an unexpected type is refused");
          w = v; w[KF_SHOW_ZONES - 1].num = 1.0;
          CHECK(!kf_prem_prepare(w.data(), (int)w.size(), W, H, 0, 7, f, ps, pl, up), "Show Zones on is refused (burned-in rings are CPU only)");
          w = v; w[KF_MIX - 1].type = 0;
          CHECK(!kf_prem_prepare(w.data(), (int)w.size(), W, H, 0, 7, f, ps, pl, up), "missing value is refused");
          CHECK(!kf_prem_prepare(v.data(), (int)v.size(), W, H, 0, 5, f, ps, pl, up), "heavy effects without kernel groups are refused"); }
    }
    printf("%s\n", fails ? "SOME FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
