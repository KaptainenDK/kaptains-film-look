// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Test harness: kf_cli in.kfi out.kfi key=value ...
// .kfi = int32 magic 'KFI1', int32 w, int32 h, int32 c(=4), then w*h*4 float32 (RGBA, interleaved)
#include "kf_core.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool read_kfi(const char* path, kf::Buf& b) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    int hdr[4];
    if (fread(hdr, 4, 4, f) != 4 || hdr[0] != 0x3149464b || hdr[3] != 4) { fclose(f); return false; }
    b = kf::Buf(hdr[1], hdr[2], 4);
    size_t n = b.d.size();
    bool ok = fread(b.d.data(), 4, n, f) == n;
    fclose(f); return ok;
}
static bool write_kfi(const char* path, const kf::Buf& b) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    int hdr[4] = {0x3149464b, b.w, b.h, 4};
    fwrite(hdr, 4, 4, f);
    fwrite(b.d.data(), 4, b.d.size(), f);
    fclose(f); return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: kf_cli in.kfi out.kfi [key=value ...]\n"); return 2; }
    kf::Buf in, out;
    if (!read_kfi(argv[1], in)) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    kf::Params p;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i]; size_t eq = a.find('=');
        if (eq == std::string::npos) { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
        std::string k = a.substr(0, eq); float v = (float)atof(a.c_str() + eq + 1);
        const char* s = a.c_str() + eq + 1;
#define F(name) else if (k == #name) p.name = v;
#define B(name) else if (k == #name) p.name = v != 0.f;
#define I(name) else if (k == #name) p.name = (int)v;
        if (false) {}
        F(mix) F(zone_center_x) F(zone_center_y) F(zone_roundness) B(show_zones)
        B(lens_enable) F(lens_amount) B(lens_auto_zoom) F(lens_zoom)
        B(ca_enable) F(ca_amount) F(ca_blur)
        B(lblur_enable) F(lblur_amount) F(lblur_angle)
        B(vig_enable) F(vig_amount) F(vig_softness)
        B(bloom_enable) F(bloom_threshold) F(bloom_intensity) F(bloom_radius) F(bloom_blur) F(bloom_stretch) F(bloom_stretch_angle)
        B(halation_enable) F(halation_threshold) F(halation_intensity) F(halation_radius) F(halation_blur) F(halation_stretch) F(halation_stretch_angle)
        B(grain_enable) F(grain_amount) F(grain_size) F(grain_roughness) F(grain_color) F(grain_midtone_bias)
        I(grain_seed) I(frame) B(grain_animate)
        else if (k == "input_space") p.input_space = (kf::InputSpace)(int)v;
        else if (k == "lens_edge") p.lens_edge = (kf::EdgeMode)(int)v;
        else if (k == "lblur_type") p.lblur_type = (kf::LensBlurType)(int)v;
#define Z(pre) else if (k == #pre "_zone_start") p.pre##_zone.start = v; else if (k == #pre "_zone_full") p.pre##_zone.full = v;
        Z(lens) Z(ca) Z(lblur) Z(vig) Z(bloom) Z(halation) Z(grain)
        else if (k == "active_area") { sscanf(s, "%f,%f,%f,%f", &p.active_area.x0, &p.active_area.y0, &p.active_area.x1, &p.active_area.y1); }
        else if (k == "bloom_tint") { sscanf(s, "%f,%f,%f", &p.bloom_tint[0], &p.bloom_tint[1], &p.bloom_tint[2]); }
        else if (k == "halation_tint") { sscanf(s, "%f,%f,%f", &p.halation_tint[0], &p.halation_tint[1], &p.halation_tint[2]); }
        else { fprintf(stderr, "unknown param %s\n", k.c_str()); return 2; }
    }
    kf::process(in, out, p);
    if (!write_kfi(argv[2], out)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    printf("auto_zoom=%.5f\n", kf::auto_zoom_for(in.w, in.h, p));
    return 0;
}
