#define NOMINMAX
#include <windows.h>

#include "engine.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <deque>
#include <cstdio>
#include <fstream>

namespace engine {

static std::string strf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static const int kMaxInstances = 8;  // per "others" term (SAM 3.1 instances kept)

// ------------------------------------------------------------------ PFM

bool readPFM(const fs::path& path, Gray& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char kind[3] = {0};
    int w = 0, h = 0;
    float scale = 0;
    if (fscanf(f, "%2s %d %d %f", kind, &w, &h, &scale) != 4 || w <= 0 || h <= 0) {
        fclose(f);
        return false;
    }
    fgetc(f);
    int ch = strcmp(kind, "PF") == 0 ? 3 : 1;
    std::vector<float> raw((size_t)w * h * ch);
    bool ok = fread(raw.data(), 4, raw.size(), f) == raw.size();
    fclose(f);
    if (!ok) return false;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)  // PFM is bottom-to-top
        for (int x = 0; x < w; ++x) out.px[(size_t)(h - 1 - y) * w + x] = raw[((size_t)y * w + x) * ch];
    return true;
}

bool writePFM(const fs::path& path, const Gray& g) {
    fs::path tmp = path;
    tmp += L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "Pf\n%d %d\n-1.0\n", g.w, g.h);
    bool ok = true;
    for (int y = g.h - 1; y >= 0 && ok; --y) ok = fwrite(&g.px[(size_t)y * g.w], 4, g.w, f) == (size_t)g.w;
    fclose(f);
    if (!ok) return false;
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

fs::path matteFile(const fs::path& dir, const char* kind, int index) {
    return dir / strf("%s_%06d.pfm", kind, index);
}

// ------------------------------------------------------------ matte maths

static void maxFilter1D(const float* in, float* out, int n, int stride, int r) {
    std::deque<int> q;  // monotonic queue of indices
    for (int i = 0, o = -r; i < n + r; ++i, ++o) {
        if (i < n) {
            float v = in[(size_t)i * stride];
            while (!q.empty() && in[(size_t)q.back() * stride] <= v) q.pop_back();
            q.push_back(i);
        }
        if (o >= 0) {
            while (q.front() < o - r) q.pop_front();
            out[(size_t)o * stride] = in[(size_t)q.front() * stride];
        }
    }
}

static void gauss1D(const float* in, float* out, int n, int stride, const std::vector<float>& k) {
    int r = (int)k.size() / 2;
    for (int i = 0; i < n; ++i) {
        float acc = 0;
        for (int t = -r; t <= r; ++t) acc += k[t + r] * in[(size_t)std::clamp(i + t, 0, n - 1) * stride];
        out[(size_t)i * stride] = acc;
    }
}

Gray growBlur(const Gray& in, int grow, double blur) {
    Gray a = in, b = in;
    const int w = in.w, h = in.h;
    if (grow != 0) {
        int r = std::abs(grow);
        bool erode = grow < 0;
        if (erode)
            for (float& v : a.px) v = -v;
        for (int y = 0; y < h; ++y) maxFilter1D(&a.px[(size_t)y * w], &b.px[(size_t)y * w], w, 1, r);
        for (int x = 0; x < w; ++x) maxFilter1D(&b.px[x], &a.px[x], h, w, r);
        if (erode)
            for (float& v : a.px) v = -v;
    }
    if (blur > 0) {
        int r = std::max(1, (int)std::ceil(blur * 3));
        std::vector<float> k(2 * r + 1);
        float s = 0;
        for (int i = -r; i <= r; ++i) s += k[i + r] = (float)std::exp(-(i * i) / (2 * blur * blur));
        for (float& v : k) v /= s;
        for (int y = 0; y < h; ++y) gauss1D(&a.px[(size_t)y * w], &b.px[(size_t)y * w], w, 1, k);
        for (int x = 0; x < w; ++x) gauss1D(&b.px[x], &a.px[x], h, w, k);
    }
    for (float& v : a.px) v = std::clamp(v, 0.f, 1.f);
    return a;
}

static float sampleBilinear(const Gray& g, double u, double v) {  // u,v in 0-1 of the image, v down
    double fx = u * g.w - 0.5, fy = v * g.h - 0.5;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    double ax = fx - x0, ay = fy - y0;
    auto at = [&](int x, int y) { return g.px[(size_t)std::clamp(y, 0, g.h - 1) * g.w + std::clamp(x, 0, g.w - 1)]; };
    return (float)((at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) +
                   (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay);
}

static Gray resized(const Gray& g, int w, int h) {
    if (g.w == w && g.h == h) return g;
    Gray o;
    o.w = w;
    o.h = h;
    o.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) o.px[(size_t)y * w + x] = sampleBilinear(g, (x + 0.5) / w, (y + 0.5) / h);
    return o;
}

// Keep only the matte islands that touch the selection: label the solid parts of
// the matte (> 0.5) 4-connected, keep labels that overlap the un-grown selection,
// then widen the kept region a little so each island's soft fringe survives.
static Gray islandsTouching(const Gray& matte, const Gray& sel) {
    const int w = matte.w, h = matte.h;
    std::vector<int> label((size_t)w * h, 0);
    std::vector<char> keep(1, 0);
    std::vector<int> stack;
    int next = 1;
    for (int start = 0; start < w * h; ++start) {
        if (label[start] || matte.px[start] <= 0.5f) continue;
        bool touches = false;
        stack.assign(1, start);
        label[start] = next;
        while (!stack.empty()) {
            int p = stack.back();
            stack.pop_back();
            if (sel.px[p] > 0.5f) touches = true;
            int x = p % w, y = p / w;
            const int nb[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1};
            for (int q : nb)
                if (q >= 0 && !label[q] && matte.px[q] > 0.5f) {
                    label[q] = next;
                    stack.push_back(q);
                }
        }
        keep.push_back(touches ? 1 : 0);
        ++next;
    }
    std::vector<size_t> area(next, 0);
    for (int l : label) ++area[l];
    size_t largest = 0;
    for (int l = 1; l < next; ++l)
        if (keep[l]) largest = std::max(largest, area[l]);
    for (int l = 1; l < next; ++l)  // specks: < 0.2% of the main island
        if (keep[l] && area[l] * 500 < largest) keep[l] = 0;
    Gray k;
    k.w = w;
    k.h = h;
    k.px.resize((size_t)w * h);
    for (size_t i = 0; i < k.px.size(); ++i) k.px[i] = keep[label[i]] ? 1.f : 0.f;
    // fill enclosed holes (glass, gaps between fingers...) so their partial alpha survives:
    // anything not reachable from the border without crossing a kept island is inside it
    std::vector<char> outside((size_t)w * h, 0);
    stack.clear();
    auto seed = [&](int p) {
        if (!outside[p] && k.px[p] == 0.f) {
            outside[p] = 1;
            stack.push_back(p);
        }
    };
    for (int x = 0; x < w; ++x) seed(x), seed((h - 1) * w + x);
    for (int y = 0; y < h; ++y) seed(y * w), seed(y * w + w - 1);
    while (!stack.empty()) {
        int p = stack.back();
        stack.pop_back();
        int x = p % w, y = p / w;
        if (x > 0) seed(p - 1);
        if (x < w - 1) seed(p + 1);
        if (y > 0) seed(p - w);
        if (y < h - 1) seed(p + w);
    }
    for (size_t i = 0; i < k.px.size(); ++i)
        if (!outside[i]) k.px[i] = 1.f;
    return growBlur(k, 6, 2.0);  // reach into the island's own soft edge
}

#include "layers.inc"

// ---------------------------------------------------------- graph builder

static std::string L(int id, int slot) { return strf("[\"%d\",%d]", id, slot); }
static std::string I(long long v) { return strf("%lld", v); }
static std::string F(double v) { return strf("%.6g", v); }
static std::string B(bool v) { return v ? "true" : "false"; }
static std::string Q(const std::string& s) { return json::quote(s); }

struct Graph {
    std::string s;
    void node(int id, const char* cls, std::initializer_list<std::pair<const char*, std::string>> in) {
        s += (s.empty() ? "" : ",") + strf("\"%d\":{\"class_type\":\"%s\",\"inputs\":{", id, cls);
        bool first = true;
        for (auto& kv : in) {
            if (kv.second.empty()) continue;  // optional input left out
            s += (first ? "" : ",") + Q(kv.first) + ":" + kv.second;
            first = false;
        }
        s += "}}";
    }
    std::string json() const { return "{" + s + "}"; }
};

static std::string coordsJson(const std::vector<Point>& pts, bool positive) {
    std::string o;
    for (auto& p : pts)
        if (p.positive == positive)
            o += strf("%s{\"x\":%d,\"y\":%d}", o.empty() ? "" : ",", (int)std::lround(p.x), (int)std::lround(p.y));
    return o.empty() ? "" : "[" + o + "]";
}

static std::string outPrefix(const Job& j, const std::string& kind) { return "dugco/alphagensam/" + j.session + "/" + kind; }

// SAM 3.1 detect of one object on one frame. Node ids base+1..base+4; output L(base+4, 0).
static void addDetect(Graph& g, const Job& j, const Object& o, const std::string& image, int base) {
    std::string pos = coordsJson(o.points, true), neg = coordsJson(o.points, false);
    bool useText = o.points.empty() && !o.text.empty();
    if (useText) g.node(base + 1, "CLIPTextEncode", {{"clip", L(1, 1)}, {"text", Q(o.text)}});
    if (!pos.empty()) g.node(base + 2, "PrimitiveString", {{"value", Q(pos)}});
    if (!neg.empty()) g.node(base + 3, "PrimitiveString", {{"value", Q(neg)}});
    // clicks: a single decoder pass. A second refine pass can collapse the mask to nothing
    // (seen on a soft-focus person); text detections refine fine.
    g.node(base + 4, "SAM3_Detect", {{"model", L(1, 0)},
                                     {"image", image},
                                     {"threshold", F(j.detThresh)},
                                     {"refine_iterations", I(useText ? 2 : 1)},
                                     {"individual_masks", B(false)},
                                     {"conditioning", useText ? L(base + 1, 0) : ""},
                                     {"positive_coords", pos.empty() ? "" : L(base + 2, 0)},
                                     {"negative_coords", neg.empty() ? "" : L(base + 3, 0)}});
}

// One SaveImage per object: object k's preview mask is node 100+10k+6.
static int previewSaveNode(int k) { return 100 + 10 * k + 6; }

static std::string previewGraph(const Job& j, const std::string& refName) {
    Graph g;
    g.node(1, "CheckpointLoaderSimple", {{"ckpt_name", Q(j.models.sam)}});
    g.node(2, "LoadImage", {{"image", Q(refName)}});
    for (int k = 0; k < (int)j.objects.size(); ++k) {
        int base = 100 + 10 * k;
        addDetect(g, j, j.objects[k], L(2, 0), base);
        g.node(base + 5, "MaskToImage", {{"mask", L(base + 4, 0)}});
        g.node(base + 6, "SaveImage", {{"images", L(base + 5, 0)}, {"filename_prefix", Q(outPrefix(j, strf("preview%d", k)))}});
    }
    return g.json();
}

// Output nodes of object k in the track graph.
struct TrackNodes {
    int all = 0, fwd = 0, bwd = 0;
};
static TrackNodes trackNodes(int k) {
    int base = 100 + 20 * k;
    return {base + 5, base + 8, base + 12};
}

// Text objects: SAM 3.1 detects and tracks every instance through the whole range.
// Clicked objects: per key, detect on that frame, then track forward and backward from it
// (videos per key frame v: forward at node 4000+10v, backward at 4000+10v+3).
// The other subjects come back as ONE 16-bit label image per frame: instance i of term t
// is the value (t*8+i+1)/(8*T+1). One download per frame instead of one per instance.
static const int kLabelSave = 2000;
static int keyNode(int k, int m) { return 3000 + 100 * k + 20 * m; }  // fwd save +8, bwd save +12
static std::vector<Key> keysOf(const Job& j, const Object& o) {
    return o.keys.empty() ? std::vector<Key>{Key{j.ref, o.points}} : o.keys;
}
static std::string trackGraph(const Job& j, const std::string& allName, const std::vector<int>& keyFrames,
                              const std::vector<std::string>& fwdNames, const std::vector<std::string>& bwdNames,
                              const std::vector<std::string>& otherTerms) {
    Graph g;
    g.node(1, "CheckpointLoaderSimple", {{"ckpt_name", Q(j.models.sam)}});
    if (!allName.empty()) {
        g.node(2, "LoadVideo", {{"file", Q(allName)}});
        g.node(3, "GetVideoComponents", {{"video", L(2, 0)}});
    }
    for (size_t v = 0; v < keyFrames.size(); ++v) {
        const int n = 4000 + 10 * (int)v;
        g.node(n, "LoadVideo", {{"file", Q(fwdNames[v])}});
        g.node(n + 1, "GetVideoComponents", {{"video", L(n, 0)}});
        g.node(n + 2, "ImageFromBatch", {{"image", L(n + 1, 0)}, {"batch_index", I(0)}, {"length", I(1)}});
        if (!bwdNames[v].empty()) {
            g.node(n + 3, "LoadVideo", {{"file", Q(bwdNames[v])}});
            g.node(n + 4, "GetVideoComponents", {{"video", L(n + 3, 0)}});
        }
    }
    auto track = [&](int id, const std::string& images, const std::string& initMask, const std::string& cond,
                     const std::string& kind, int maxObjects) {
        g.node(id, "SAM3_VideoTrack", {{"images", images}, {"model", L(1, 0)}, {"initial_mask", initMask},
                                       {"conditioning", cond}, {"detection_threshold", F(j.detThresh)},
                                       {"max_objects", I(maxObjects)}, {"detect_interval", I(1)}});
        g.node(id + 1, "SAM3_TrackToMask", {{"track_data", L(id, 0)}, {"object_indices", Q("")}});
        g.node(id + 2, "MaskToImage", {{"mask", L(id + 1, 0)}});
        g.node(id + 3, "SaveImage", {{"images", L(id + 2, 0)}, {"filename_prefix", Q(outPrefix(j, kind))}});
    };
    for (int k = 0; k < (int)j.objects.size(); ++k) {
        const Object& o = j.objects[k];
        if (!o.clicked()) {  // text: every instance, whole range
            int base = 100 + 20 * k;
            g.node(base + 1, "CLIPTextEncode", {{"clip", L(1, 1)}, {"text", Q(o.text)}});
            track(base + 2, L(3, 0), "", L(base + 1, 0), strf("sel%d_all", k), 0);  // saves at base+5
            continue;
        }
        std::vector<Key> keys = keysOf(j, o);
        for (int m = 0; m < (int)keys.size() && m < kMaxKeys; ++m) {
            const int v = (int)(std::find(keyFrames.begin(), keyFrames.end(), keys[m].frame) - keyFrames.begin());
            const int n = 4000 + 10 * v, base = keyNode(k, m);
            Object at;
            at.points = keys[m].points;
            addDetect(g, j, at, L(n + 2, 0), base);  // base+1..base+4
            track(base + 5, L(n + 1, 0), L(base + 4, 0), "", strf("sel%d_k%d_fwd", k, m), 1);  // saves at base+8
            if (!bwdNames[v].empty())
                track(base + 9, L(n + 4, 0), L(base + 4, 0), "", strf("sel%d_k%d_bwd", k, m), 1);  // base+12
        }
    }
    if (!otherTerms.empty() && !allName.empty()) {
        const int T = (int)otherTerms.size(), levels = kMaxInstances * T + 1;
        std::string acc;
        for (int t = 0; t < T; ++t) {
            int b = 1000 + 100 * t;
            g.node(b + 1, "CLIPTextEncode", {{"clip", L(1, 1)}, {"text", Q(otherTerms[t])}});
            g.node(b + 2, "SAM3_VideoTrack", {{"images", L(3, 0)}, {"model", L(1, 0)}, {"conditioning", L(b + 1, 0)},
                                              {"detection_threshold", F(j.detThresh)}, {"max_objects", I(kMaxInstances)},
                                              {"detect_interval", I(1)}});
            for (int i = 0; i < kMaxInstances; ++i) {
                g.node(b + 10 + i, "SAM3_TrackToMask", {{"track_data", L(b + 2, 0)}, {"object_indices", Q(std::to_string(i))}});
                // hard 0/1 first: a soft (bilinear) edge times the label value would read as another label
                g.node(b + 20 + i, "ThresholdMask", {{"mask", L(b + 10 + i, 0)}, {"value", F(0.5)}});
                g.node(b + 30 + i, "SolidMask", {{"value", F((double)(t * kMaxInstances + i + 1) / levels)},
                                                 {"width", I(j.W)}, {"height", I(j.H)}});
                g.node(b + 50 + i, "MaskComposite", {{"destination", L(b + 20 + i, 0)}, {"source", L(b + 30 + i, 0)},
                                                     {"x", I(0)}, {"y", I(0)}, {"operation", Q("multiply")}});
                if (acc.empty()) {
                    acc = L(b + 50 + i, 0);
                } else {  // overwrite, not add: SAM can report the same thing twice (overlapping), and a sum
                          // saturates. acc - acc*mask_i + label_i (core MaskComposite has no max)
                    g.node(b + 80 + i, "MaskComposite", {{"destination", acc}, {"source", L(b + 20 + i, 0)}, {"x", I(0)},
                                                         {"y", I(0)}, {"operation", Q("multiply")}});
                    g.node(b + 90 + i, "MaskComposite", {{"destination", acc}, {"source", L(b + 80 + i, 0)}, {"x", I(0)},
                                                         {"y", I(0)}, {"operation", Q("subtract")}});
                    g.node(b + 70 + i, "MaskComposite", {{"destination", L(b + 90 + i, 0)}, {"source", L(b + 50 + i, 0)},
                                                         {"x", I(0)}, {"y", I(0)}, {"operation", Q("add")}});
                    acc = L(b + 70 + i, 0);
                }
            }
        }
        g.node(kLabelSave - 1, "MaskToImage", {{"mask", acc}});
        g.node(kLabelSave, "SaveImageAdvanced", {{"images", L(kLabelSave - 1, 0)},
                                                 {"filename_prefix", Q(outPrefix(j, "labels"))},
                                                 {"format", Q("png")},
                                                 {"format.bit_depth", Q("16-bit")},
                                                 {"format.input_color_space", Q("sRGB")}});
    }
    return g.json();
}

// LTX-2.5 Alpha-Gen IC-LoRA, stage-1 only at native resolution (mirrors the
// "10b - Alpha Pass on Video" workflow: distilled, 8 steps, cfg 1, empty prompt).
// Core nodes only; verified bit-identical to the ComfyUI-LTXVideo IC-LoRA nodes.
// image: link to the guide frames. Uses node ids 30-51 (30 = the shared transformer).
static void addAlphaGen(Graph& g, const Job& j, const std::string& image, int length, int chunkIndex, const char* kind) {
    g.node(30, "UNETLoader", {{"unet_name", Q(j.models.unet)}, {"weight_dtype", Q("default")}});
    g.node(31, "LoraLoaderModelOnly", {{"model", L(30, 0)}, {"lora_name", Q(j.models.lora)}, {"strength_model", F(1)}});
    g.node(51, "GetICLoRAParameters", {{"iclora_model", L(31, 0)}});  // reads reference_downscale_factor
    g.node(32, "CLIPLoader", {{"clip_name", Q(j.models.textEncoder)}, {"type", Q("ltxv")}, {"device", Q("default")}});
    g.node(33, "VAELoader", {{"vae_name", Q(j.models.videoVae)}});
    g.node(34, "VAELoader", {{"vae_name", Q(j.models.audioVae)}});
    g.node(35, "CLIPTextEncode", {{"clip", L(32, 0)}, {"text", Q("")}});
    g.node(36, "CLIPTextEncode", {{"clip", L(32, 0)}, {"text", Q(" ")}});  // empty negative crashes the tokenizer
    g.node(37, "LTXVConditioning", {{"positive", L(35, 0)}, {"negative", L(36, 0)}, {"frame_rate", F(24)}});
    g.node(38, "EmptyLTXVLatentVideo", {{"width", I(j.W)}, {"height", I(j.H)}, {"length", I(length)}, {"batch_size", I(1)}});
    g.node(39, "LTXVAddGuide", {{"positive", L(37, 0)}, {"negative", L(37, 1)}, {"vae", L(33, 0)}, {"latent", L(38, 0)},
                                {"image", image}, {"frame_idx", I(0)}, {"strength", F(1)},
                                {"iclora_parameters", L(51, 0)}});
    g.node(40, "LTXVEmptyLatentAudio", {{"frames_number", I(length)}, {"frame_rate", I(24)}, {"batch_size", I(1)},
                                        {"audio_vae", L(34, 0)}});
    g.node(41, "LTXVConcatAVLatent", {{"video_latent", L(39, 2)}, {"audio_latent", L(40, 0)}});
    g.node(42, "RandomNoise", {{"noise_seed", I(j.seed)}});
    g.node(43, "CFGGuider", {{"model", L(31, 0)}, {"positive", L(39, 0)}, {"negative", L(39, 1)}, {"cfg", F(1)}});
    g.node(44, "KSamplerSelect", {{"sampler_name", Q("euler_ancestral")}});
    g.node(45, "ManualSigmas", {{"sigmas", Q("1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0")}});
    g.node(46, "SamplerCustomAdvanced", {{"noise", L(42, 0)}, {"guider", L(43, 0)}, {"sampler", L(44, 0)},
                                         {"sigmas", L(45, 0)}, {"latent_image", L(41, 0)}});
    g.node(47, "LTXVSeparateAVLatent", {{"av_latent", L(46, 0)}});
    g.node(48, "LTXVCropGuides", {{"positive", L(39, 0)}, {"negative", L(39, 1)}, {"latent", L(47, 0)}});
    g.node(49, "VAEDecodeTiled", {{"samples", L(48, 2)}, {"vae", L(33, 0)}, {"tile_size", I(768)}, {"overlap", I(128)},
                                  {"temporal_size", I(128)}, {"temporal_overlap", I(16)}});
    g.node(50, "SaveImageAdvanced", {{"images", L(49, 0)},
                                     {"filename_prefix", Q(outPrefix(j, strf("%s_c%03d", kind, chunkIndex)))},
                                     {"format", Q("png")},
                                     {"format.bit_depth", Q("16-bit")},
                                     {"format.input_color_space", Q("sRGB")}});
}

static std::string alphaGenGraph(const Job& j, const std::string& guideName, int length, int chunkIndex,
                                 const char* kind) {
    Graph g;
    g.node(2, "LoadVideo", {{"file", Q(guideName)}});
    g.node(3, "GetVideoComponents", {{"video", L(2, 0)}});
    addAlphaGen(g, j, L(3, 0), length, chunkIndex, kind);
    return g.json();
}

// Clean plate. The LTX-2.5 Clean-Plate IC-LoRA rebuilds what is behind the other
// objects (two stages: half resolution, then latent x2 to the working size, as in the
// E2 "Clean Plate, Rig and Crew Removal" workflow). The result is used only inside the
// comp matte, the untouched plate everywhere else; that composite comes back as PNGs and
// goes to Alpha-Gen as its own job (one job with both ran a 121-frame chunk out of memory).
//   plate: the chunk as it is; guide: the plate with the hole smeared from its
//   surroundings (a flat hole comes back as an object); hole: 1 where the others are
//   removed (the guide's attention is 0 there, so the model can't copy them back);
//   comp: soft matte where the rebuilt pixels are used.
static std::string cleanPlateGraph(const Job& j, const std::string& plate, const std::string& guide,
                                   const std::string& hole, const std::string& comp, int length, int chunkIndex,
                                   const char* kind) {
    Graph g;
    const int w1 = std::max(32, (j.W / 2) / 32 * 32), h1 = std::max(32, (j.H / 2) / 32 * 32);
    g.node(2, "LoadVideo", {{"file", Q(plate)}});
    g.node(3, "GetVideoComponents", {{"video", L(2, 0)}});
    g.node(4, "LoadVideo", {{"file", Q(guide)}});
    g.node(5, "GetVideoComponents", {{"video", L(4, 0)}});
    g.node(6, "LoadVideo", {{"file", Q(hole)}});
    g.node(7, "GetVideoComponents", {{"video", L(6, 0)}});
    g.node(8, "ImageToMask", {{"image", L(7, 0)}, {"channel", Q("red")}});
    g.node(9, "InvertMask", {{"mask", L(8, 0)}});  // stage-2 attention: 0 in the hole
    g.node(10, "ImageScale", {{"image", L(7, 0)}, {"upscale_method", Q("area")}, {"width", I(w1)}, {"height", I(h1)},
                              {"crop", Q("disabled")}});
    g.node(11, "ImageToMask", {{"image", L(10, 0)}, {"channel", Q("red")}});
    g.node(12, "InvertMask", {{"mask", L(11, 0)}});
    g.node(13, "ImageScale", {{"image", L(5, 0)}, {"upscale_method", Q("area")}, {"width", I(w1)}, {"height", I(h1)},
                              {"crop", Q("disabled")}});
    g.node(14, "LoadVideo", {{"file", Q(comp)}});
    g.node(15, "GetVideoComponents", {{"video", L(14, 0)}});
    g.node(16, "ImageToMask", {{"image", L(15, 0)}, {"channel", Q("red")}});

    g.node(30, "UNETLoader", {{"unet_name", Q(j.models.unet)}, {"weight_dtype", Q("default")}});
    g.node(32, "CLIPLoader", {{"clip_name", Q(j.models.textEncoder)}, {"type", Q("ltxv")}, {"device", Q("default")}});
    g.node(33, "VAELoader", {{"vae_name", Q(j.models.videoVae)}});
    g.node(34, "VAELoader", {{"vae_name", Q(j.models.audioVae)}});
    g.node(60, "LoraLoaderModelOnly", {{"model", L(30, 0)}, {"lora_name", Q(j.models.cleanLora)}, {"strength_model", F(1)}});
    g.node(61, "GetICLoRAParameters", {{"iclora_model", L(60, 0)}});
    g.node(62, "CLIPTextEncode", {{"clip", L(32, 0)},
                                  {"text", Q("A clean plate of the exact same shot: the same place, the same light and "
                                             "colours, and the camera moving exactly as in the source video, with nobody "
                                             "and nothing extra in the frame. Natural, photorealistic, sharp.")}});
    g.node(63, "CLIPTextEncode", {{"clip", L(32, 0)},
                                  {"text", Q("person, people, figure, body part, hair, shadow of a person, ghosting, "
                                             "semi-transparent remnants, smudge, blurry, soft, flicker, jitter, worst "
                                             "quality, cartoon")}});
    g.node(64, "LTXVConditioning", {{"positive", L(62, 0)}, {"negative", L(63, 0)}, {"frame_rate", F(24)}});
    // stage 1, half resolution
    g.node(65, "EmptyLTXVLatentVideo", {{"width", I(w1)}, {"height", I(h1)}, {"length", I(length)}, {"batch_size", I(1)}});
    g.node(66, "LTXVAddGuide", {{"positive", L(64, 0)}, {"negative", L(64, 1)}, {"vae", L(33, 0)}, {"latent", L(65, 0)},
                                {"image", L(13, 0)}, {"frame_idx", I(0)}, {"strength", F(1)},
                                {"attention_mask", L(12, 0)}, {"iclora_parameters", L(61, 0)}});
    g.node(67, "LTXVEmptyLatentAudio", {{"frames_number", I(length)}, {"frame_rate", I(24)}, {"batch_size", I(1)},
                                        {"audio_vae", L(34, 0)}});
    g.node(68, "LTXVConcatAVLatent", {{"video_latent", L(66, 2)}, {"audio_latent", L(67, 0)}});
    g.node(69, "RandomNoise", {{"noise_seed", I(j.seed)}});
    g.node(70, "CFGGuider", {{"model", L(60, 0)}, {"positive", L(66, 0)}, {"negative", L(66, 1)}, {"cfg", F(1)}});
    g.node(71, "KSamplerSelect", {{"sampler_name", Q("euler_ancestral")}});
    g.node(72, "ManualSigmas", {{"sigmas", Q("1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0")}});
    g.node(73, "SamplerCustomAdvanced", {{"noise", L(69, 0)}, {"guider", L(70, 0)}, {"sampler", L(71, 0)},
                                         {"sigmas", L(72, 0)}, {"latent_image", L(68, 0)}});
    g.node(74, "LTXVSeparateAVLatent", {{"av_latent", L(73, 0)}});
    g.node(75, "LTXVCropGuides", {{"positive", L(66, 0)}, {"negative", L(66, 1)}, {"latent", L(74, 0)}});
    // stage 2: latent x2 to the working size, 3 refine steps guided by the full-size plate
    g.node(76, "LatentUpscaleModelLoader", {{"model_name", Q(j.models.upscaler)}});
    g.node(77, "LTXVLatentUpsampler", {{"samples", L(75, 2)}, {"upscale_model", L(76, 0)}, {"vae", L(33, 0)}});
    g.node(78, "LTXVAddGuide", {{"positive", L(75, 0)}, {"negative", L(75, 1)}, {"vae", L(33, 0)}, {"latent", L(77, 0)},
                                {"image", L(5, 0)}, {"frame_idx", I(0)}, {"strength", F(1)},
                                {"attention_mask", L(9, 0)}, {"iclora_parameters", L(61, 0)}});
    g.node(79, "LTXVConcatAVLatent", {{"video_latent", L(78, 2)}, {"audio_latent", L(74, 1)}});
    g.node(80, "RandomNoise", {{"noise_seed", I(j.seed)}});
    g.node(81, "CFGGuider", {{"model", L(60, 0)}, {"positive", L(78, 0)}, {"negative", L(78, 1)}, {"cfg", F(1)}});
    g.node(82, "KSamplerSelect", {{"sampler_name", Q("euler")}});
    g.node(83, "ManualSigmas", {{"sigmas", Q("0.909375, 0.725, 0.421875, 0.0")}});
    g.node(84, "SamplerCustomAdvanced", {{"noise", L(80, 0)}, {"guider", L(81, 0)}, {"sampler", L(82, 0)},
                                         {"sigmas", L(83, 0)}, {"latent_image", L(79, 0)}});
    g.node(85, "LTXVSeparateAVLatent", {{"av_latent", L(84, 0)}});
    g.node(86, "LTXVCropGuides", {{"positive", L(78, 0)}, {"negative", L(78, 1)}, {"latent", L(85, 0)}});
    g.node(87, "VAEDecodeTiled", {{"samples", L(86, 2)}, {"vae", L(33, 0)}, {"tile_size", I(768)}, {"overlap", I(128)},
                                  {"temporal_size", I(128)}, {"temporal_overlap", I(16)}});
    // rebuilt pixels only inside the comp matte
    std::string rebuilt = L(87, 0);
    if (2 * w1 != j.W || 2 * h1 != j.H) {
        g.node(88, "ImageScale", {{"image", L(87, 0)}, {"upscale_method", Q("bicubic")}, {"width", I(j.W)},
                                  {"height", I(j.H)}, {"crop", Q("disabled")}});
        rebuilt = L(88, 0);
    }
    g.node(118, "ImageCompositeMasked", {{"destination", L(3, 0)}, {"source", rebuilt}, {"x", I(0)}, {"y", I(0)},
                                         {"resize_source", B(false)}, {"mask", L(16, 0)}});
    g.node(120, "SaveImage", {{"images", L(118, 0)},
                              {"filename_prefix", Q(outPrefix(j, strf("%s_plate_c%03d", kind, chunkIndex)))}});
    return g.json();
}

// Depth Anything 3 (core nodes) on a few frames: who is in front of whom.
static std::string depthGraph(const Job& j, const std::string& video) {
    Graph g;
    g.node(2, "LoadVideo", {{"file", Q(video)}});
    g.node(3, "GetVideoComponents", {{"video", L(2, 0)}});
    g.node(4, "LoadDA3Model", {{"model_name", Q(j.models.depth)}, {"weight_dtype", Q("default")}});
    g.node(5, "DA3Inference", {{"da3_model", L(4, 0)}, {"image", L(3, 0)}, {"resolution", I(1008)},
                               {"resize_method", Q("upper_bound_resize")}, {"mode", Q("mono")}});
    g.node(6, "DA3Render", {{"da3_geometry", L(5, 0)}, {"output", Q("depth")}, {"output.normalization", Q("min_max")},
                            {"output.apply_sky_clip", B(false)}});
    g.node(7, "SaveImageAdvanced", {{"images", L(6, 0)},
                                    {"filename_prefix", Q(outPrefix(j, "depth"))},
                                    {"format", Q("png")},
                                    {"format.bit_depth", Q("16-bit")},
                                    {"format.input_color_space", Q("sRGB")}});
    return g.json();
}

// ----------------------------------------------------------------- driver

static std::wstring uploadPath(const Job& j, const std::string& name) { return (j.dir / name).wstring(); }

struct Ctx {
    const Job& j;
    const Progress& progress;
    double base = 0, span = 1;
    Result res;
    bool step(double frac, const std::string& text) {
        if (progress && !progress(base + span * frac, text)) {
            res.cancelled = true;
            res.error = "Cancelled.";
            return false;
        }
        return true;
    }
    bool fail(const std::string& e) {
        res.error = e;
        return false;
    }
};

static bool runGraph(Ctx& c, const std::string& graph, const char* tag, const std::string& text,
                     std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>>& outputs) {
    {  // keep the last prompt of each kind next to the cache for debugging
        std::ofstream(c.j.dir / strf("prompt_%s.json", tag), std::ios::binary) << graph;
    }
    std::string err;
    bool cancelled = false;
    bool ok = comfy::run(c.j.server, graph, "dugco-alphagensam-" + c.j.session,
                         [&](double f, const std::string&) { return c.step(f * 0.9, text); }, outputs, err, cancelled);
    if (!ok) {
        c.res.cancelled = cancelled;
        return c.fail(err);
    }
    return true;
}

static const std::vector<comfy::OutputFile>* outputsOf(
    const std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>>& outs, int node) {
    for (auto& o : outs)
        if (o.first == std::to_string(node)) return &o.second;
    return nullptr;
}

// Download files[k] -> matte kind index map(k); count limits how many are kept.
// Frames below blendUntil overlap the previous chunk: they are cross-faded into the
// matte already cached there (smoothstep from the old chunk to this one), so chunk
// boundaries don't pop.
static bool fetchMattes(Ctx& c, const std::vector<comfy::OutputFile>& files, const char* kind,
                        const std::function<int(int)>& indexOf, int count, double from, double to,
                        int blendUntil = -1) {
    const int first = indexOf(0), overlap = blendUntil - first;
    for (int k = 0; k < count && k < (int)files.size(); ++k) {
        std::string bytes, err;
        if (!comfy::download(c.j.server, files[k], bytes, err)) return c.fail(err);
        Gray g;
        if (!media::decodePNG(bytes, g, err)) return c.fail(err);
        Gray prev;
        if (indexOf(k) < blendUntil && readPFM(matteFile(c.j.dir, kind, indexOf(k)), prev) && prev.px.size() == g.px.size()) {
            double t = (indexOf(k) - first + 1.0) / (overlap + 1.0);
            float w = (float)(t * t * (3 - 2 * t));
            for (size_t i = 0; i < g.px.size(); ++i) g.px[i] = prev.px[i] + (g.px[i] - prev.px[i]) * w;
        }
        if (!writePFM(matteFile(c.j.dir, kind, indexOf(k)), g)) return c.fail("cannot write matte cache");
        if (!c.step(from + (to - from) * (k + 1) / count, "downloading mattes")) return false;
    }
    if ((int)files.size() < count)
        return c.fail(strf("ComfyUI returned %d %s frames, expected %d.", (int)files.size(), kind, count));
    return true;
}

static bool hasPixels(const fs::path& p) {
    Gray g;
    if (!readPFM(p, g)) return false;
    for (float v : g.px)
        if (v > 0.5f) return true;
    return false;
}

int objectCount(const fs::path& dir) {
    std::ifstream f(dir / "objects.txt");
    int n = 0;
    f >> n;
    return n;
}

std::string selKind(int k) { return strf("sel%d", k); }
std::string isoKind(int k) { return strf("agi%d", k); }
std::string othKind(int k) { return strf("oth%d", k); }
std::string cleanKind(int k) { return strf("agc%d", k); }
std::string holeKind(int k) { return strf("hol%d", k); }
std::string fillKind(int k) { return strf("agx%d", k); }

static std::vector<std::string> splitTerms(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        std::string t = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        size_t x = t.find_first_not_of(" \t"), y = t.find_last_not_of(" \t");
        if (x != std::string::npos) out.push_back(t.substr(x, y - x + 1));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}


std::string pass2Kind(const fs::path& dir, int k) {
    std::ifstream f(dir / strf("pass2_%d.txt", k));
    std::string v;
    f >> v;
    return v;
}

bool pass2Steered(const fs::path& dir, int k) { return pass2Kind(dir, k) == "steered"; }

std::vector<int> outputObjects(const fs::path& dir) {
    std::vector<int> out;
    std::ifstream f(dir / "outputs.txt");
    for (int k; f >> k;) out.push_back(k);
    if (out.empty())  // older caches: every object
        for (int k = 0, n = objectCount(dir); k < n; ++k) out.push_back(k);
    return out;
}

std::string resolveModels(Job& j, bool needSam, bool needLtx, bool needClean, bool needDepth) {
    struct Need {
        bool want;
        const char *node, *input, *what;
        std::string* name;
    };
    Need needs[] = {
        {needSam, "CheckpointLoaderSimple", "ckpt_name", "SAM 3.1 checkpoint", &j.models.sam},
        {needLtx, "UNETLoader", "unet_name", "LTX-2.5 transformer", &j.models.unet},
        {needLtx, "LoraLoaderModelOnly", "lora_name", "LTX-2.5 Alpha-Gen IC-LoRA", &j.models.lora},
        {needLtx, "CLIPLoader", "clip_name", "LTX-2.5 text encoder", &j.models.textEncoder},
        {needLtx, "VAELoader", "vae_name", "LTX-2.5 video VAE", &j.models.videoVae},
        {needLtx, "VAELoader", "vae_name", "LTX-2.5 audio VAE", &j.models.audioVae},
        {needClean, "LoraLoaderModelOnly", "lora_name", "LTX-2.5 Clean-Plate IC-LoRA", &j.models.cleanLora},
        {needClean, "LatentUpscaleModelLoader", "model_name", "LTX-2.5 latent upscaler", &j.models.upscaler},
        {needDepth, "LoadDA3Model", "model_name", "Depth Anything 3", &j.models.depth},
    };
    std::map<std::string, std::vector<std::string>> cache;
    std::string missing;
    for (Need& n : needs) {
        if (!n.want || n.name->empty()) continue;
        std::string key = std::string(n.node) + "|" + n.input;
        if (!cache.count(key)) {
            std::string err;
            if (!comfy::modelOptions(j.server, n.node, n.input, cache[key], err)) return err;
        }
        const auto& opts = cache[key];
        auto base = [](const std::string& v) {
            size_t p = v.find_last_of("/\\");
            std::string b = p == std::string::npos ? v : v.substr(p + 1);
            size_t q = b.rfind("__");  // imported: org__repo__file
            return q == std::string::npos ? b : b.substr(q + 2);
        };
        std::string found;
        for (const auto& o : opts)
            if (o == *n.name) found = o;
        if (found.empty())
            for (const auto& o : opts)
                if (base(o) == base(*n.name)) found = o;
        // Comfy Cloud has only the LTX-2.3 Clean-Plate IC-LoRA; LTX's own 2.5 IC-LoRA
        // workflows load 2.3 IC-LoRA weights on the 2.5 model, so it's a drop-in
        if (found.empty() && n.name == &j.models.cleanLora)
            for (const auto& o : opts)
                if (base(o) == "ltx-2.3-22b-ic-lora-clean-plate-1.0.safetensors") found = o;
        if (found.empty()) missing += strf("\n  %s: %s", n.what, n.name->c_str());
        else *n.name = found;
    }
    // Comfy Cloud also serves catalogue models its loaders don't list yet (they load on
    // demand), so only a local server's list is binding.
    if (missing.empty() || j.server.cloud || getenv("AGSAM_NO_MODEL_CHECK")) return "";
    return (j.server.cloud ? "Comfy Cloud is missing models this needs:" : "ComfyUI is missing models this needs:") +
           missing +
           (j.server.cloud ? "\nImport them into your Comfy Cloud account (Models > Import, from Hugging Face), "
                             "or pick a different mode under Advanced."
                           : "\nDownload them into ComfyUI's models folders.");
}

void warmUp(const comfy::Server& server, const std::string& samModel) {
    std::string info, err;
    if (!comfy::ping(server, info, err)) return;  // no server yet: the first preview loads it instead
    Graph g;
    g.node(1, "CheckpointLoaderSimple", {{"ckpt_name", Q(samModel)}});
    g.node(2, "EmptyImage", {{"width", I(64)}, {"height", I(64)}, {"batch_size", I(1)}, {"color", I(0)}});
    g.node(3, "PrimitiveString", {{"value", Q("[{\"x\":32,\"y\":32}]")}});
    g.node(4, "SAM3_Detect", {{"model", L(1, 0)}, {"image", L(2, 0)}, {"threshold", F(0.5)}, {"refine_iterations", I(0)},
                              {"individual_masks", B(false)}, {"positive_coords", L(3, 0)}});
    g.node(5, "MaskToImage", {{"mask", L(4, 0)}});
    g.node(6, "PreviewImage", {{"images", L(5, 0)}});
    server.request("POST", "/api/prompt", "{\"client_id\":\"dugco-alphagensam-warmup\",\"prompt\":" + g.json() + "}");
}

Result preview(const Job& jIn, const Frame& refFrame, const Progress& p) {
    Job j = jIn;
    {
        std::string err = resolveModels(j, true, false, false, false);
        if (!err.empty()) {
            Result r;
            r.error = err;
            return r;
        }
    }
    Ctx c{j, p};
    std::error_code ec;
    fs::create_directories(j.dir, ec);
    for (auto& e : fs::directory_iterator(j.dir, ec))
        if (e.path().filename().string().rfind("preview", 0) == 0) fs::remove(e.path(), ec);
    std::string err;
    if (j.objects.empty()) return c.fail("Nothing selected: type an object or Ctrl+click it in the viewer."), c.res;
    if (!media::init(err)) return c.fail(err), c.res;
    std::string name = "dugco_agsam_" + j.session + "_ref.png";
    if (!media::writePNG(uploadPath(j, name), refFrame, err)) return c.fail(err), c.res;
    if (!c.step(0.05, "uploading")) return c.res;
    std::string remote = comfy::upload(j.server, uploadPath(j, name), name, err);
    if (remote.empty()) return c.fail(err), c.res;
    std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>> outs;
    c.base = 0.1;
    c.span = 0.8;
    if (!runGraph(c, previewGraph(j, remote), "preview", "SAM 3.1", outs)) return c.res;
    c.base = 0;
    c.span = 1;
    std::string empty;
    for (int k = 0; k < (int)j.objects.size(); ++k) {
        auto files = outputsOf(outs, previewSaveNode(k));
        if (!files || files->empty()) return c.fail("ComfyUI returned no preview mask."), c.res;
        std::string kind = strf("preview%d", k);
        if (!fetchMattes(c, *files, kind.c_str(), [](int) { return 0; }, 1, 0.9 + 0.1 * k / j.objects.size(),
                         0.9 + 0.1 * (k + 1) / j.objects.size()))
            return c.res;
        if (!hasPixels(matteFile(j.dir, kind.c_str(), 0)))
            empty += (empty.empty() ? "" : ", ") + j.objects[k].label();
    }
    std::ofstream(j.dir / "preview_objects.txt") << j.objects.size();
    c.res.ok = true;
    c.res.info = empty;  // labels of objects SAM found nothing for
    return c.res;
}

static bool depthPass(Ctx& c, const FrameSource& frames);

Result analyze(const Job& jIn, const FrameSource& frames, const Progress& p) {
    Job j = jIn;
    {
        const bool sel = j.mode != kAlphaGenOnly, ag = j.mode != kSelectionOnly;
        std::string err = resolveModels(j, sel, ag, ag && j.mode == kClean, ag && j.mode == kClean && j.objects.size() > 1);
        if (!err.empty()) {
            Result r;
            r.error = err;
            return r;
        }
    }
    Ctx c{j, p};
    std::error_code ec;
    fs::create_directories(j.dir, ec);
    for (auto& e : fs::directory_iterator(j.dir, ec)) {  // a new run must not mix with an older range
        std::string n = e.path().filename().string();
        if (n.rfind("ag", 0) == 0 || n.rfind("sel", 0) == 0 || n.rfind("guide_", 0) == 0 || n.rfind("pass2", 0) == 0 ||
            n == "objects.txt")
            fs::remove(e.path(), ec);
    }
    std::string err;
    if (!media::init(err)) return c.fail(err), c.res;

    const bool needSel = j.mode != kAlphaGenOnly;
    const bool needAG = j.mode != kSelectionOnly;
    const int K = needSel ? (int)j.objects.size() : 0;
    if (needSel && K == 0) return c.fail("Nothing selected: type an object or Ctrl+click it in the viewer."), c.res;
    // Alpha-Gen chunks: up to j.chunk frames each, overlapping by j.blend frames that
    // are cross-faded; the last chunk is pulled back to end on the last frame.
    std::vector<int> chunkStart;
    if (needAG) {
        const int len = std::min(j.chunk, j.count);
        const int ov = std::clamp(j.blend, 0, len / 2);
        for (int s = 0;; s += len - ov) {
            chunkStart.push_back(std::min(s, j.count - len));
            if (chunkStart.back() + len >= j.count) break;
        }
    }
    const int chunks = (int)chunkStart.size();
    const double wTrack = needSel ? (needAG ? 0.12 : 1.0) : 0.0;
    const double wAG = 1.0 - wTrack;

    // ---- SAM 3.1 tracking, all objects in one ComfyUI job
    if (needSel) {
        c.base = 0;
        c.span = wTrack;
        bool anyText = false;
        std::vector<int> keyFrames;  // distinct frames with clicks
        for (auto& o : j.objects) {
            if (!o.clicked()) anyText = true;
            else
                for (const Key& kk : keysOf(j, o)) keyFrames.push_back(kk.frame);
        }
        std::sort(keyFrames.begin(), keyFrames.end());
        keyFrames.erase(std::unique(keyFrames.begin(), keyFrames.end()), keyFrames.end());
        for (int f : keyFrames)
            if (f < 0 || f >= j.count) return c.fail(strf("Clicked points on frame %d are outside the range.", j.firstFrame + f)), c.res;
        const std::vector<std::string> otherTerms = splitTerms(j.othersPrompt);
        if (!otherTerms.empty()) anyText = true;  // the others are tracked through the whole range
        auto collect = [&](int from, int to, int step, std::vector<const Frame*>& out) {
            for (int i = from; i != to; i += step) {
                const Frame* f = frames(i);
                if (!f) return false;
                out.push_back(f);
            }
            return true;
        };
        if (!c.step(0.0, "encoding plate")) return c.res;
        auto send = [&](const std::vector<const Frame*>& list, const std::string& name, std::string& remote) {
            if (!media::writeMP4(uploadPath(j, name), list, 24, err)) return c.fail(err);
            remote = comfy::upload(j.server, uploadPath(j, name), name, err);
            return remote.empty() ? c.fail(err) : true;
        };
        std::string allRemote;
        if (anyText) {
            std::vector<const Frame*> all;
            if (!collect(0, j.count, 1, all)) return c.fail("could not read the input frames"), c.res;
            if (!send(all, "dugco_agsam_" + j.session + "_all.mp4", allRemote)) return c.res;
        }
        std::vector<std::string> fwdRemote(keyFrames.size()), bwdRemote(keyFrames.size());
        for (size_t v = 0; v < keyFrames.size(); ++v) {
            const int f = keyFrames[v];
            std::vector<const Frame*> fl, bl;
            if (!collect(f, j.count, 1, fl)) return c.fail("could not read the input frames"), c.res;
            if (!send(fl, strf("dugco_agsam_%s_fwd%d.mp4", j.session.c_str(), f), fwdRemote[v])) return c.res;
            if (f > 0) {
                if (!collect(f, -1, -1, bl)) return c.fail("could not read the input frames"), c.res;
                if (!send(bl, strf("dugco_agsam_%s_bwd%d.mp4", j.session.c_str(), f), bwdRemote[v])) return c.res;
            }
            if (!c.step(0.03 + 0.1 * (v + 1) / keyFrames.size(), "uploading plate")) return c.res;
        }
        std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>> outs;
        c.base = wTrack * 0.15;
        c.span = wTrack * 0.6;
        if (!runGraph(c, trackGraph(j, allRemote, keyFrames, fwdRemote, bwdRemote, otherTerms), "track", "SAM 3.1 tracking",
                      outs))
            return c.res;
        c.base = 0;
        c.span = 1;
        for (int k = 0; k < K; ++k) {
            const Object& o = j.objects[k];
            std::string kind = selKind(k);
            double f0 = wTrack * (0.75 + 0.25 * k / K), f1 = wTrack * (0.75 + 0.25 * (k + 1) / K);
            if (!o.clicked()) {
                auto f = outputsOf(outs, trackNodes(k).all);
                if (!f) return c.fail("ComfyUI returned no selection for " + o.label() + "."), c.res;
                if (!fetchMattes(c, *f, kind.c_str(), [](int i) { return i; }, j.count, f0, f1)) return c.res;
                bool found = false;
                for (int i = 0; i < j.count && !found; i += std::max(1, j.count / 12)) found = hasPixels(matteFile(j.dir, kind.c_str(), i));
                if (!found) return c.fail("SAM 3.1 found no \"" + o.text + "\" in this range."), c.res;
                continue;
            }
            // each key's own track, then per frame the nearest key that finds the object
            std::vector<Key> keys = keysOf(j, o);
            if ((int)keys.size() > kMaxKeys) keys.resize(kMaxKeys);
            const int M = (int)keys.size();
            for (int m = 0; m < M; ++m) {
                const int f = keys[m].frame, base = keyNode(k, m);
                const std::string tk = strf("trk%d_%d", k, m);
                double g0 = f0 + (f1 - f0) * m / M, g1 = f0 + (f1 - f0) * (m + 1) / M;
                auto fw = outputsOf(outs, base + 8);
                if (!fw) return c.fail("ComfyUI returned no selection for the " + o.label() + "."), c.res;
                if (!fetchMattes(c, *fw, tk.c_str(), [&](int i) { return f + i; }, j.count - f, g0, (g0 + g1) / 2)) return c.res;
                if (f > 0) {
                    auto bw = outputsOf(outs, base + 12);
                    if (!bw) return c.fail("ComfyUI returned no backward selection."), c.res;
                    // index 0 is the key frame again: keep the forward copy
                    std::vector<comfy::OutputFile> rest(bw->begin() + std::min<size_t>(1, bw->size()), bw->end());
                    if (!fetchMattes(c, rest, tk.c_str(), [&](int i) { return f - 1 - i; }, f, (g0 + g1) / 2, g1)) return c.res;
                }
                if (!hasPixels(matteFile(j.dir, tk.c_str(), f)))
                    return c.fail(strf("SAM 3.1 found nothing at the %s's points on frame %d. Press Preview there to "
                                       "check them.", o.label().c_str(), j.firstFrame + f)),
                           c.res;
            }
            for (int i = 0; i < j.count; ++i) {
                std::vector<int> order(M);
                for (int m = 0; m < M; ++m) order[m] = m;
                std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
                    return std::abs(keys[x].frame - i) < std::abs(keys[y].frame - i);
                });
                Gray pick;
                for (int m : order) {
                    Gray g;
                    if (!readPFM(matteFile(j.dir, strf("trk%d_%d", k, m).c_str(), i), g)) continue;
                    bool any = false;
                    for (float v : g.px) if (v > 0.5f) { any = true; break; }
                    if (pick.px.empty() || any) pick = std::move(g);
                    if (any) break;
                }
                if (!pick.px.empty()) writePFM(matteFile(j.dir, kind.c_str(), i), pick);
            }
        }
        // ---- the other subjects of each object: SAM instances that aren't it + the other selected objects
        const int T = (int)otherTerms.size(), levels = kMaxInstances * T + 1;
        if (T > 0) {
            auto lf = outputsOf(outs, kLabelSave);
            if (!lf) return c.fail("ComfyUI returned no labels for the other subjects."), c.res;
            if (!fetchMattes(c, *lf, "labels", [](int i) { return i; }, j.count, wTrack * 0.95, wTrack)) return c.res;
        }
        // which instances are the object itself (mostly inside its selection, over the whole range)
        std::vector<std::vector<double>> inside(K, std::vector<double>(levels, 0.0));
        std::vector<double> area(levels, 0.0);
        auto labelAt = [&](const Gray& lab, size_t p) {  // value = id / levels
            return std::min(levels - 1, (int)std::lround(lab.px[p] * levels));
        };
        if (T > 0)
            for (int i = 0; i < j.count; ++i) {
                Gray lab;
                if (!readPFM(matteFile(j.dir, "labels", i), lab)) continue;
                std::vector<Gray> sels(K);
                for (int k = 0; k < K; ++k) {
                    readPFM(matteFile(j.dir, selKind(k).c_str(), i), sels[k]);
                    if (sels[k].w != lab.w || sels[k].h != lab.h) sels[k] = resized(sels[k], lab.w, lab.h);
                }
                for (size_t p = 0; p < lab.px.size(); ++p) {
                    int id = labelAt(lab, p);
                    if (id <= 0 || id >= levels) continue;
                    area[id] += 1;
                    for (int k = 0; k < K; ++k) inside[k][id] += sels[k].px[p] > 0.5f;
                }
            }
        // SAM's masks come from a ~1008 px grid: round off their staircase edges first
        const double smooth = std::max(1.0, j.W / 1920.0 * 2.5);
        auto smoothed = [&](Gray g) {
            g = growBlur(g, 0, smooth);
            for (float& v : g.px) v = v > 0.5f ? 1.f : 0.f;
            return g;
        };
        for (int k = 0; k < K; ++k)
            for (int i = 0; i < j.count; ++i) {
                Gray g;
                if (readPFM(matteFile(j.dir, selKind(k).c_str(), i), g))
                    writePFM(matteFile(j.dir, selKind(k).c_str(), i), smoothed(g));
            }
        for (int k = 0; k < K; ++k) {
            std::vector<char> isSelf(levels, 0);
            for (int id = 1; id < levels; ++id) isSelf[id] = area[id] > 0 && inside[k][id] / area[id] > 0.4;
            for (int i = 0; i < j.count; ++i) {
                Gray lab, oth;
                bool hasLab = T > 0 && readPFM(matteFile(j.dir, "labels", i), lab);
                Gray selK;
                readPFM(matteFile(j.dir, selKind(k).c_str(), i), selK);
                oth = blankLike(selK);
                if (hasLab) {
                    if (lab.w != oth.w || lab.h != oth.h) lab = resized(lab, oth.w, oth.h);
                    for (size_t p = 0; p < lab.px.size(); ++p) {
                        int id = labelAt(lab, p);
                        if (id > 0 && id < levels && !isSelf[id]) oth.px[p] = 1.f;
                    }
                }
                for (int k2 = 0; k2 < K; ++k2) {  // the other selected objects count as others too
                    if (k2 == k) continue;
                    Gray s2;
                    if (!readPFM(matteFile(j.dir, selKind(k2).c_str(), i), s2)) continue;
                    if (s2.w != oth.w || s2.h != oth.h) s2 = resized(s2, oth.w, oth.h);
                    for (size_t p = 0; p < oth.px.size(); ++p) oth.px[p] = std::max(oth.px[p], s2.px[p]);
                }
                oth = smoothed(oth);
                for (size_t p = 0; p < oth.px.size(); ++p)  // never the object itself
                    if (selK.px[p] > 0.5f) oth.px[p] = 0.f;
                writePFM(matteFile(j.dir, othKind(k).c_str(), i), oth);
            }
        }
        std::ofstream(j.dir / "objects.txt") << K;
    }

#include "passes.inc"
    c.res.ok = true;
    return c.res;
}

void cleanHoles(const Job& j, const FrameSource& frames) {
    const int K = objectCount(j.dir);
    const std::vector<char> front = frontOrder(j, K);
    if (frames) temporalFront(j, K, frames, front);
    seeThroughFront(j, K, front);
    for (int a = 0; a < K; ++a)
        for (int b = 0; b < K; ++b)
            if (front[a * K + b]) printf("object %d is in front of object %d\n", a, b);
    for (int k : outputObjects(j.dir))
        if (k < K && pass2Kind(j.dir, k) == "clean")
            for (int i = 0; i < j.count; ++i) writeHole(j, K, k, i, front);
}

// Depth on up to 12 frames spread over the range -> depth_<i>.
static bool depthPass(Ctx& c, const FrameSource& frames) {
    const Job& j = c.j;
    std::vector<int> idx;
    const int n = std::min(12, j.count);
    for (int t = 0; t < n; ++t) idx.push_back(n == 1 ? 0 : (int)std::lround(t * (j.count - 1.0) / (n - 1)));
    std::vector<const Frame*> list;
    for (int i : idx) {
        const Frame* f = frames(i);
        if (!f) return c.fail("could not read the input frames");
        list.push_back(f);
    }
    std::string err, name = "dugco_agsam_" + j.session + "_depth.mp4";
    if (!media::writeMP4(uploadPath(j, name), list, 24, err)) return c.fail(err);
    std::string remote = comfy::upload(j.server, uploadPath(j, name), name, err);
    if (remote.empty()) return c.fail(err);
    std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>> outs;
    if (!runGraph(c, depthGraph(j, remote), "depth", "Depth Anything 3: who is in front", outs)) return false;
    auto f = outputsOf(outs, 7);
    if (!f) return c.fail("ComfyUI returned no depth.");
    return fetchMattes(c, *f, "depth", [&](int k) { return idx[k]; }, (int)idx.size(), c.base, c.base);
}

Result depthOnly(const Job& j, const FrameSource& frames) {
    Ctx c{j, Progress()};
    if (depthPass(c, frames)) c.res.ok = true;
    return c.res;
}

}  // namespace engine
