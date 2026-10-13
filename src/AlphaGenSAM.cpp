// DUGco AlphaGenSAM - OpenFX filter for Nuke (and other OFX hosts).
// Part of the DUGco OFX tool pack.
//
// Type what to matte ("car", "person, diner") and/or Ctrl+click it in the viewer,
// press Process, and the plug-in runs SAM 3.1 + LTX-2.5 Alpha-Gen on a ComfyUI
// backend, then puts the matte in the alpha channel.
//
// The plug-in is the whole user interface; ComfyUI is only the compute backend
// (local server today, Comfy Cloud later). See engine.h for the backend design.

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

#include "engine.h"
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"

using engine::Gray;
namespace fs = std::filesystem;

// DUGco OFX tool pack: every tool lives under the "DUGco" menu with a com.dugco.* id
// (the id stays lowercase: ids are case-sensitive and saved in Nuke scripts)
#define PLUGIN_ID "com.dugco.AlphaGenSAM"
#define PLUGIN_LABEL "AlphaGenSAM"
#define PLUGIN_GROUPING "DUGco"

static OfxHost* gHost = nullptr;
static OfxPropertySuiteV1* gProp = nullptr;
static OfxImageEffectSuiteV1* gEffect = nullptr;
static OfxParameterSuiteV1* gParam = nullptr;
static OfxMessageSuiteV1* gMessage = nullptr;
static OfxMessageSuiteV2* gMessage2 = nullptr;  // persistent (red node) messages
static OfxProgressSuiteV1* gProgress = nullptr;
static OfxInteractSuiteV1* gInteract = nullptr;

enum Backend { kBackendLocal = 0, kBackendCloud };
// What the viewer shows (RGB); the alpha channel always carries the matte.
enum Show { kShowMatte = 0, kShowCutout, kShowPlate, kShowPreview };
// Advanced > debug: replaces the final matte with an intermediate stage.
enum Debug { kDebugOff = 0, kDebugSelection, kDebugPlain, kDebugIsolated };
// matte sources (internal)
enum MatteOut { kOutFinal = 0, kOutPreview, kOutSelection, kOutPlain, kOutIsolated };
enum Colour { kColLinear = 0, kColSRGB, kColRaw };
enum Range { kRangeInput = 0, kRangeCustom };

// A viewer point, in canonical coordinates, on the frame it was clicked.
struct VPoint {
    double x = 0, y = 0;
    bool positive = true;
    int frame = 0;
    int obj = 1;  // which clicked object (1-6)
};

constexpr int kClickObjects = 6;

// Preview colours: 0-5 the click objects (as named in "click object"), 6+ typed objects.
static const float kTint[12][3] = {{0.2f, 1.f, 0.3f},  {1.f, 0.6f, 0.1f},  {0.2f, 0.8f, 1.f},  {1.f, 0.3f, 0.9f},
                                   {1.f, 1.f, 0.2f},   {0.6f, 0.4f, 1.f},  {0.f, 0.9f, 0.8f},  {1.f, 0.45f, 0.45f},
                                   {0.7f, 1.f, 0.5f},  {0.4f, 0.55f, 1.f}, {1.f, 0.8f, 0.6f},  {0.85f, 0.85f, 0.85f}};


struct Instance {
    OfxImageClipHandle src = nullptr, dst = nullptr;
    OfxParamHandle select, points, autoPreview, rangeMode, frameStart, frameEnd, show, debug, status;
    OfxParamHandle mode, workRes, chunk, blend, seed, colour, detThresh, others, reach, blackPt, whitePt;
    OfxParamHandle backend, cloudKey, serverUrl, samModel, unetModel, icLora, textEnc, videoVae, audioVae, cacheDir;
    OfxParamHandle session, anaStart, anaCount, cacheVersion;

    // viewer drag state (lives on the effect instance, shared by its overlay)
    int dragIndex = -1;
    std::vector<VPoint> dragPoints;

    std::mutex lock;
    std::map<std::string, std::shared_ptr<Gray>> cache;  // decoded/composed mattes
    std::vector<std::string> order;
};

// ----------------------------------------------------------------- helpers

static std::string strf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static Instance* getInstance(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    void* p = nullptr;
    gProp->propGetPointer(props, kOfxPropInstanceData, 0, &p);
    return (Instance*)p;
}

static void message(OfxImageEffectHandle effect, const char* type, const std::string& text) {
    if (gMessage) gMessage->message(effect, type, nullptr, "%s", text.c_str());
}

static std::string getString(OfxParamHandle p, double t = 0) {
    const char* s = nullptr;
    gParam->paramGetValueAtTime(p, t, &s);
    return s ? s : "";
}
static int getInt(OfxParamHandle p, double t = 0) {
    int v = 0;
    gParam->paramGetValueAtTime(p, t, &v);
    return v;
}
static double getDouble(OfxParamHandle p, double t = 0) {
    double v = 0;
    gParam->paramGetValueAtTime(p, t, &v);
    return v;
}

// Cache root: the "cache folder" knob, by default "<script folder>/dugco_cache" (a TCL
// expression Nuke expands in path knobs), so paid-for results live with the shot. Falls
// back to %TEMP%\dugco\AlphaGenSAM when the script isn't saved yet (or the host didn't
// expand the expression), and keeps using a session folder already in Temp (older nodes).
static fs::path sessionRoot(Instance* inst) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path temp = fs::path(tmp) / L"dugco" / L"AlphaGenSAM" / getString(inst->session);
    std::string root = getString(inst->cacheDir);
    std::error_code ec;
    const bool usable = !root.empty() && root.find('[') == std::string::npos && fs::path(root).is_absolute() &&
                        fs::path(root).parent_path() != fs::path(root).root_path() / "";
    if (!usable) return temp;
    const fs::path here = fs::path(root) / getString(inst->session);
    if (!fs::exists(here, ec) && fs::exists(temp, ec)) return temp;
    return here;
}

// Copies of a node share its session. Their caches are also keyed by what they select
// (typed objects + clicked points), so a copy that selects something else gets its own
// analysis instead of overwriting the original's, while copies with the same selection
// still share one (and "alpha for" picks between its objects).
static std::string selectionPrint(Instance* inst) {
    std::string key = getString(inst->select) + "|" + getString(inst->points);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    return strf("%08llx", (unsigned long long)(h & 0xffffffffull));
}

static fs::path sessionDir(Instance* inst) { return sessionRoot(inst) / selectionPrint(inst); }

static std::string newSessionId() {
    std::random_device rd;
    std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)GetTickCount64());
    return strf("%016llx", (unsigned long long)rng());
}

static void setStatus(Instance* inst, const std::string& s) { gParam->paramSetValue(inst->status, s.c_str()); }

static void bumpCache(Instance* inst) {
    {
        std::lock_guard<std::mutex> g(inst->lock);
        inst->cache.clear();
        inst->order.clear();
    }
    gParam->paramSetValue(inst->cacheVersion, getInt(inst->cacheVersion) + 1);
}

static double pixelAspect(OfxImageClipHandle clip) {
    OfxPropertySetHandle cp;
    gEffect->clipGetPropertySet(clip, &cp);
    double par = 1.0;
    gProp->propGetDouble(cp, kOfxImagePropPixelAspectRatio, 0, &par);
    return par > 0 ? par : 1.0;
}

static void inputRange(Instance* inst, int& a, int& b) {
    OfxPropertySetHandle cp;
    gEffect->clipGetPropertySet(inst->src, &cp);
    double r[2] = {1, 1};
    gProp->propGetDoubleN(cp, kOfxImageEffectPropFrameRange, 2, r);
    a = (int)std::lround(r[0]);
    b = (int)std::lround(r[1]);
}

// ---- background selection preview
//
// A click must never freeze Nuke: the hero frame is grabbed on the main thread, SAM
// runs on a worker thread, and the next render of the preview view waits for it (on
// Nuke's render thread, so the UI stays live and the dot appears at once). Jobs are
// keyed by session because Nuke renders through other instances than the UI one;
// each job waits for the previous one so the newest result always lands last.
struct PreviewJob {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error, info;
};
static std::mutex gJobLock;
static std::map<std::string, std::shared_ptr<PreviewJob>> gPreviewJobs;

static std::shared_ptr<PreviewJob> previewJob(const std::string& session) {
    std::lock_guard<std::mutex> g(gJobLock);
    auto it = gPreviewJobs.find(session);
    return it == gPreviewJobs.end() ? nullptr : it->second;
}

// ---- viewer points: stored in a hidden string knob as "x y +|- frame object;..."

static std::vector<VPoint> parsePoints(const std::string& s) {
    std::vector<VPoint> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        VPoint p;
        char sign = '+';
        if (sscanf(item.c_str(), "%lf %lf %c %d %d", &p.x, &p.y, &sign, &p.frame, &p.obj) >= 4) {
            p.positive = sign != '-';
            p.obj = 1;  // one node = one matte: every click belongs to the one selection
            out.push_back(p);
        }
    }
    return out;
}

static std::string formatPoints(const std::vector<VPoint>& pts) {
    std::string s;
    for (auto& p : pts)
        s += strf("%s%.2f %.2f %c %d %d", s.empty() ? "" : ";", p.x, p.y, p.positive ? '+' : '-', p.frame, p.obj);
    return s;
}

// The clicked object lives on one frame: the frame of the most recent click.
static int activeFrame(const std::vector<VPoint>& pts) { return pts.empty() ? 0 : pts.back().frame; }

static std::vector<std::string> splitTerms(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) {
        size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(t.substr(a, b - a + 1));
    }
    return out;
}

// ------------------------------------------------------------- image access

struct ImageView {
    void* data = nullptr;
    OfxRectI bounds{};
    int rowBytes = 0, nComp = 4, bytesPerComp = 4;
    bool ok = false;
};

static ImageView viewImage(OfxPropertySetHandle img) {
    ImageView v;
    if (!img) return v;
    gProp->propGetPointer(img, kOfxImagePropData, 0, &v.data);
    gProp->propGetIntN(img, kOfxImagePropBounds, 4, &v.bounds.x1);
    gProp->propGetInt(img, kOfxImagePropRowBytes, 0, &v.rowBytes);
    char* comps = nullptr;
    char* depth = nullptr;
    gProp->propGetString(img, kOfxImageEffectPropComponents, 0, &comps);
    gProp->propGetString(img, kOfxImageEffectPropPixelDepth, 0, &depth);
    std::string c = comps ? comps : "", d = depth ? depth : "";
    v.nComp = c == kOfxImageComponentRGBA ? 4 : c == kOfxImageComponentRGB ? 3 : c == kOfxImageComponentAlpha ? 1 : 4;
    v.bytesPerComp = d == kOfxBitDepthFloat ? 4 : d == kOfxBitDepthShort ? 2 : d == kOfxBitDepthByte ? 1 : 4;
    v.ok = v.data != nullptr;
    return v;
}

static float pixelComp(const ImageView& v, int x, int y, int c) {
    x = std::clamp(x, v.bounds.x1, v.bounds.x2 - 1);
    y = std::clamp(y, v.bounds.y1, v.bounds.y2 - 1);
    if (c >= v.nComp) c = v.nComp - 1;
    const char* row = (const char*)v.data + (ptrdiff_t)(y - v.bounds.y1) * v.rowBytes;
    size_t i = (size_t)(x - v.bounds.x1) * v.nComp + c;
    switch (v.bytesPerComp) {
        case 1: return ((const unsigned char*)row)[i] / 255.f;
        case 2: return ((const unsigned short*)row)[i] / 65535.f;
        default: return ((const float*)row)[i];
    }
}

static void storeComp(void* row, int bytesPerComp, size_t i, float v) {
    switch (bytesPerComp) {
        case 1: ((unsigned char*)row)[i] = (unsigned char)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); break;
        case 2: ((unsigned short*)row)[i] = (unsigned short)std::lround(std::clamp(v, 0.f, 1.f) * 65535.f); break;
        default: ((float*)row)[i] = v;
    }
}

static float srgbEncode(float v) {
    if (!(v > 0.f)) return 0.f;
    if (v >= 1.f) return 1.f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

static void workingSize(const OfxRectD& rod, int longEdge, int& W, int& H) {
    double w = std::max(1.0, rod.x2 - rod.x1), h = std::max(1.0, rod.y2 - rod.y1);
    auto r32 = [](double v) { return std::max(64, (int)std::lround(v / 32.0) * 32); };
    if (w >= h) {
        W = r32(longEdge);
        H = r32(longEdge * h / w);
    } else {
        H = r32(longEdge);
        W = r32(longEdge * w / h);
    }
}

// Fetch one input frame and downscale it (supersampled box) to the working grid,
// as 8-bit sRGB-encoded RGB, rows top-to-bottom.
static bool fetchFrame(Instance* inst, double t, int W, int H, int colour, engine::Frame& out, std::string& err) {
    OfxRectD rod;
    gEffect->clipGetRegionOfDefinition(inst->src, t, &rod);
    OfxPropertySetHandle img = nullptr;
    // Nuke can refuse an image while it is busy rendering (e.g. another node's viewer
    // update): give it a moment before failing.
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (gEffect->clipGetImage(inst->src, t, &rod, &img) == kOfxStatOK && img) break;
        img = nullptr;
        Sleep(150);
    }
    if (!img) {
        err = strf("Nuke was busy and didn't hand over frame %d of the input. Press Preview (or Process) again; "
                   "if it keeps happening, check that the input has an image on that frame.",
                   (int)t);
        return false;
    }
    ImageView v = viewImage(img);
    double rs[2] = {1, 1};
    gProp->propGetDoubleN(img, kOfxImageEffectPropRenderScale, 2, rs);
    double par = pixelAspect(inst->src);
    double x1 = rod.x1 * rs[0] / par, x2 = rod.x2 * rs[0] / par, y1 = rod.y1 * rs[1], y2 = rod.y2 * rs[1];
    double sx = (x2 - x1) / W, sy = (y2 - y1) / H;
    int nx = std::clamp((int)std::ceil(sx), 1, 4), ny = std::clamp((int)std::ceil(sy), 1, 4);
    out.w = W;
    out.h = H;
    out.rgb.assign((size_t)W * H * 3, 0);
    if (v.ok) {
        for (int y = 0; y < H; ++y) {
            int oy = H - 1 - y;  // OFX rows are bottom-up; frames are top-down
            for (int x = 0; x < W; ++x) {
                float acc[3] = {0, 0, 0};
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        int px = (int)std::floor(x1 + (x + (i + 0.5) / nx) * sx);
                        int py = (int)std::floor(y1 + (y + (j + 0.5) / ny) * sy);
                        for (int c = 0; c < 3; ++c) acc[c] += pixelComp(v, px, py, c);
                    }
                for (int c = 0; c < 3; ++c) {
                    float val = acc[c] / (nx * ny);
                    val = colour == kColLinear ? srgbEncode(val) : std::clamp(val, 0.f, 1.f);
                    out.rgb[((size_t)oy * W + x) * 3 + c] = (uint8_t)std::lround(val * 255.f);
                }
            }
        }
    }
    gEffect->clipReleaseImage(img);
    if (!v.ok) {
        err = "Input image has no data.";
        return false;
    }
    return true;
}

// --------------------------------------------------------------- describe

static OfxPropertySetHandle defParam(OfxParamSetHandle ps, const char* type, const char* name, const char* label,
                                     const char* hint, const char* parent) {
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, type, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    if (hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    if (parent) gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    return p;
}
static void defGroup(OfxParamSetHandle ps, const char* name, const char* label, bool open) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeGroup, name, label, nullptr, nullptr);
    gProp->propSetInt(p, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
}
static void defInt(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                   int def, int dmin, int dmax) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeInteger, name, label, hint, parent);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropDisplayMin, 0, dmin);
    gProp->propSetInt(p, kOfxParamPropDisplayMax, 0, dmax);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}
static void defDouble(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                      double def, double dmin, double dmax) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeDouble, name, label, hint, parent);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, dmin);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, dmax);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}
static void defBool(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                    bool def) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeBoolean, name, label, hint, parent);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def ? 1 : 0);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}
static OfxPropertySetHandle defString(OfxParamSetHandle ps, const char* name, const char* label, const char* hint,
                                      const char* parent, const char* def, const char* mode = kOfxParamStringIsSingleLine,
                                      bool secret = false) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeString, name, label, hint, parent);
    gProp->propSetString(p, kOfxParamPropDefault, 0, def);
    gProp->propSetString(p, kOfxParamPropStringMode, 0, mode);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    if (secret) gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);
    return p;
}
static void defReadOnly(OfxParamSetHandle ps, const char* name, const char* label, const char* parent, const char* def) {
    OfxPropertySetHandle p = defString(ps, name, label, nullptr, parent, def);
    gProp->propSetInt(p, kOfxParamPropEnabled, 0, 0);
    gProp->propSetInt(p, kOfxParamPropEvaluateOnChange, 0, 0);
}
static void defChoice(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                      std::initializer_list<const char*> options, int def) {
    OfxPropertySetHandle p = defParam(ps, kOfxParamTypeChoice, name, label, hint, parent);
    int i = 0;
    for (const char* o : options) gProp->propSetString(p, kOfxParamPropChoiceOption, i++, o);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}
static void defButton(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent) {
    defParam(ps, kOfxParamTypePushButton, name, label, hint, parent);
}

// A param followed by a divider line in the panel (Nuke honours OfxParamPropLayoutHint 1).
static void defDivider(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* type) {
    OfxPropertySetHandle p = defParam(ps, type, name, label, hint, nullptr);
    gProp->propSetInt(p, "OfxParamPropLayoutHint", 0, 1);
}

static OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                             OfxPropertySetHandle outArgs);

static OfxStatus describe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, PLUGIN_LABEL);
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, PLUGIN_GROUPING);
    gProp->propSetString(props, kOfxPropPluginDescription, 0,
                         "DUGco AlphaGenSAM: type or click the objects to matte; SAM 3.1 selects them and LTX-2.5 "
                         "Alpha-Gen draws the matte, computed on a ComfyUI backend. Part of the DUGco OFX tool pack.");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthByte);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
    gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)overlayMain);
    return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle) {
    OfxPropertySetHandle cp;
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
    gProp->propSetInt(cp, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetInt(cp, kOfxImageEffectPropSupportsTiles, 0, 1);
    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetInt(cp, kOfxImageEffectPropSupportsTiles, 0, 1);

    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);

    // ---- main controls (no group: always visible). Short lines: Nuke doesn't wrap labels.
    defChoice(ps, "show", "show",
              "What the viewer shows. The alpha channel always carries the matte.\n"
              "matte: white on black.\ncutout: the plate premultiplied by the matte.\n"
              "plate + alpha: the untouched plate (for comping downstream).\n"
              "selection preview: SAM 3.1's selection on the hero frame, one colour per object.",
              nullptr, {"matte", "cutout (premultiplied)", "plate + alpha", "selection preview"}, kShowPreview);
    defString(ps, "howto1", "", nullptr, nullptr, "1  What do you want a matte of?  Type it, or Ctrl+click it in the viewer",
              kOfxParamStringIsLabel);
    defString(ps, "howto2", "", nullptr, nullptr,
              "    Ctrl+Shift+click = exclude     Alt+click = delete     (re-click on later frames if it gets lost)",
              kOfxParamStringIsLabel);
    defString(ps, "select", "select",
              "What this node pulls the matte of: \"car\", \"person\", \"diner\" (every instance SAM 3.1 finds), "
              "and/or Ctrl+click it in the viewer. One node = one matte: for another object, branch another "
              "node off the same plate. Everything else in the shot is handled automatically (see Advanced > "
              "also remove).",
              nullptr, "");
    defString(ps, "points", "points", nullptr, nullptr, "", kOfxParamStringIsSingleLine, true);
    defDivider(ps, "clearPoints", "Clear Points", "Remove all clicked points", kOfxParamTypePushButton);
    defString(ps, "howto3", "", nullptr, nullptr, "2  Check the selection on this frame:", kOfxParamStringIsLabel);
    defBool(ps, "autoPreview", "auto preview",
            "Refresh the selection preview (on this hero frame only) after every click or edit", nullptr, true);
    defDivider(ps, "preview", "Preview", "Show what SAM 3.1 selects on the current frame (coloured per object)",
               kOfxParamTypePushButton);
    defString(ps, "howto4", "", nullptr, nullptr, "3  Pull the matte through the shot:", kOfxParamStringIsLabel);
    defChoice(ps, "rangeMode", "frame range", nullptr, nullptr, {"input range", "custom"}, kRangeInput);
    defInt(ps, "frameStart", "first frame", "Used when frame range is custom", nullptr, 1, 0, 200);
    defInt(ps, "frameEnd", "last frame", "Used when frame range is custom", nullptr, 100, 0, 200);
    defDivider(ps, "process", "Process",
               "Track the selection through the range and pull the matte with Alpha-Gen on the ComfyUI backend "
               "(Nuke waits behind a progress bar; Cancel stops it).",
               kOfxParamTypePushButton);
    defString(ps, "status", "status", nullptr, nullptr, "", kOfxParamStringIsSingleLine, true);

    // ---- advanced
    defGroup(ps, "advancedGroup", "Advanced", false);
    defChoice(ps, "mode", "mode",
              "clean plate: per object, the other objects are removed with the LTX-2.5 Clean-Plate IC-LoRA, "
              "Alpha-Gen mattes the object's whole shape, and the holes are the alpha of whatever is in front.\n"
              "auto: per object, the plain Alpha-Gen pass then a pass with every other object painted out "
              "(faster); objects Alpha-Gen treats as background are steered.\n"
              "isolate / steer: force steering off / on.\nfast: one pass, split between objects.\n"
              "alpha-gen only: no selection.\nselection only: SAM 3.1 mask, no Alpha-Gen.",
              "advancedGroup",
              {"auto (paint out)", "isolate", "steer", "fast (one pass)", "alpha-gen only", "selection only",
               "clean plate (best)"},
              engine::kClean);
    defInt(ps, "workRes", "working long edge", "Resolution the models run at (rounded to /32). Alpha-Gen is tuned for 1920.",
           "advancedGroup", 1920, 512, 1920);
    defInt(ps, "chunk", "frames per chunk", "Alpha-Gen runs in chunks of up to this many frames (max 145).",
           "advancedGroup", 121, 9, 145);
    defInt(ps, "blend", "chunk blend", "Longer shots: neighbouring chunks overlap by this many frames and cross-fade.",
           "advancedGroup", 16, 0, 48);
    defInt(ps, "seed", "seed", nullptr, "advancedGroup", 42, 0, 1000);
    defChoice(ps, "colour", "input colour", "How plate values are encoded for the models (they expect sRGB).",
              "advancedGroup", {"linear -> sRGB", "already sRGB / display", "raw (clamp only)"}, kColLinear);
    defDouble(ps, "detThresh", "detection threshold", "SAM 3.1 score threshold (lower finds more)", "advancedGroup",
              0.5, 0, 1);
    defString(ps, "others", "also remove",
              "Foreground things SAM 3.1 finds automatically (comma-separated) and removes from your object's plate "
              "with the LTX clean plate; where they are in front of it they cut its holes. Background (buildings, "
              "sky, road) is left alone: Alpha-Gen already ignores it.",
              "advancedGroup", "person, car, animal");
    defInt(ps, "reach", "reach (px)",
           "How far Alpha-Gen's matte may extend beyond SAM's outline and still count as your object "
           "(hair, loose clothing, parts SAM missed). Pixels at 1920 wide.",
           "advancedGroup", 150, 10, 400);
    defDouble(ps, "blackPt", "haze black point", "Matte stretched onto [black, white] and clipped: removes Alpha-Gen's haze",
              "advancedGroup", -0.04, -0.2, 0.2);
    defDouble(ps, "whitePt", "haze white point", nullptr, "advancedGroup", 1.03, 0.8, 1.2);
    defChoice(ps, "debug", "debug view", "Replace the final matte with one stage of the process", "advancedGroup",
              {"off", "tracked selection (SAM 3.1)", "alpha-gen pass 1 (plain)", "alpha-gen pass 2 (isolated)"}, kDebugOff);
    defButton(ps, "clearCache", "Clear Cache", "Delete this node's cached mattes", "advancedGroup");

    // ---- backend
    defGroup(ps, "serverGroup", "ComfyUI Backend", false);
    defChoice(ps, "backend", "backend",
              "Where the models run.\nComfy Cloud: cloud GPUs through your Comfy Cloud subscription and an API key "
              "from the Comfy platform; no GPU needed on this machine.\nComfy Local: your own ComfyUI (server "
              "address below) with the models downloaded; needs a big GPU.",
              "serverGroup", {"Comfy Local", "Comfy Cloud"}, kBackendCloud);
    defString(ps, "cloudKey", "API key", "Comfy Cloud API key status. The key itself is kept in "
              "Documents\\DUGco\\comfy_cloud_api_key.txt, never in the Nuke script, so scripts are safe to share.",
              "serverGroup", "", kOfxParamStringIsSingleLine);
    defButton(ps, "setKey", "Set API Key...", "Paste your Comfy Cloud API key (it is hidden while you type)",
              "serverGroup");
    defButton(ps, "getKey", "Get an API Key", "Opens the Comfy platform in your browser: sign in with your Comfy "
              "Cloud account and create a key", "serverGroup");
    defString(ps, "serverUrl", "local server", "Comfy Local only: the ComfyUI address (this machine or another on the "
              "network).", "serverGroup", "http://127.0.0.1:8000");
    defButton(ps, "testServer", "Test Connection", nullptr, "serverGroup");
    defString(ps, "samModel", "SAM 3.1 checkpoint", nullptr, "serverGroup", "sam3.1_multiplex_fp16.safetensors");
    defString(ps, "unetModel", "LTX transformer", nullptr, "serverGroup",
              "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors");
    defString(ps, "icLora", "Alpha-Gen IC-LoRA", nullptr, "serverGroup", "ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors");
    defString(ps, "textEnc", "text encoder", nullptr, "serverGroup",
              "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors");
    defString(ps, "videoVae", "video VAE", nullptr, "serverGroup", "ltx-2.5-video-vae-bf16.safetensors");
    defString(ps, "audioVae", "audio VAE", nullptr, "serverGroup", "ltx-2.5-audio-vae-bf16.safetensors");
    defString(ps, "cacheDir", "cache folder",
              "Where this node keeps its downloaded mattes (and the plates it uploads). Default: a dugco_cache "
              "folder next to the Nuke script, so results travel with the shot and reopening the script never "
              "re-runs (or re-pays for) processing. Unsaved scripts use %TEMP%\\dugco\\AlphaGenSAM.",
              "serverGroup", "[file dirname [value root.name]]/dugco_cache", kOfxParamStringIsDirectoryPath);

    // ---- hidden state (saved with the script)
    defString(ps, "session", "session", nullptr, nullptr, "", kOfxParamStringIsSingleLine, true);
    for (const char* n : {"anaStart", "anaCount", "cacheVersion"}) {
        OfxPropertySetHandle p;
        gParam->paramDefine(ps, kOfxParamTypeInteger, n, &p);
        gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    }
    return kOfxStatOK;
}

// ---------------------------------------------------------------- instance

static OfxParamHandle ph(OfxParamSetHandle ps, const std::string& name) {
    OfxParamHandle h = nullptr;
    gParam->paramGetHandle(ps, name.c_str(), &h, nullptr);
    return h;
}

static std::string keyStatus();

static void updateRangeEnabled(Instance* inst) {
    int custom = getInt(inst->rangeMode) == kRangeCustom;
    for (OfxParamHandle p : {inst->frameStart, inst->frameEnd}) {
        OfxPropertySetHandle pp;
        gParam->paramGetPropertySet(p, &pp);
        gProp->propSetInt(pp, kOfxParamPropEnabled, 0, custom);
    }
}

static OfxStatus createInstance(OfxImageEffectHandle effect) {
    auto* inst = new Instance();
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &inst->src, nullptr);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &inst->dst, nullptr);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    inst->select = ph(ps, "select");
    inst->points = ph(ps, "points");
    inst->autoPreview = ph(ps, "autoPreview");
    inst->rangeMode = ph(ps, "rangeMode");
    inst->frameStart = ph(ps, "frameStart");
    inst->frameEnd = ph(ps, "frameEnd");
    inst->show = ph(ps, "show");
    inst->debug = ph(ps, "debug");
    inst->status = ph(ps, "status");
    inst->mode = ph(ps, "mode");
    inst->workRes = ph(ps, "workRes");
    inst->chunk = ph(ps, "chunk");
    inst->blend = ph(ps, "blend");
    inst->seed = ph(ps, "seed");
    inst->colour = ph(ps, "colour");
    inst->detThresh = ph(ps, "detThresh");
    inst->others = ph(ps, "others");
    inst->reach = ph(ps, "reach");
    inst->blackPt = ph(ps, "blackPt");
    inst->whitePt = ph(ps, "whitePt");
    inst->backend = ph(ps, "backend");
    inst->serverUrl = ph(ps, "serverUrl");
    inst->cloudKey = ph(ps, "cloudKey");
    gParam->paramSetValue(inst->cloudKey, keyStatus().c_str());
    {
        OfxPropertySetHandle kp;
        gParam->paramGetPropertySet(inst->cloudKey, &kp);
        gProp->propSetInt(kp, kOfxParamPropEnabled, 0, 0);
    }
    inst->samModel = ph(ps, "samModel");
    inst->unetModel = ph(ps, "unetModel");
    inst->icLora = ph(ps, "icLora");
    inst->textEnc = ph(ps, "textEnc");
    inst->videoVae = ph(ps, "videoVae");
    inst->audioVae = ph(ps, "audioVae");
    inst->cacheDir = ph(ps, "cacheDir");
    inst->session = ph(ps, "session");
    inst->anaStart = ph(ps, "anaStart");
    inst->anaCount = ph(ps, "anaCount");
    inst->cacheVersion = ph(ps, "cacheVersion");
    if (getString(inst->session).empty()) gParam->paramSetValue(inst->session, newSessionId().c_str());
    updateRangeEnabled(inst);
    // Load SAM 3.1 in ComfyUI now, so the first click previews quickly (once per server per Nuke session).
    if (getInt(inst->backend) == kBackendLocal) {
        static std::mutex warmLock;
        static std::map<std::string, bool> warmed;
        std::string url = getString(inst->serverUrl), sam = getString(inst->samModel);
        std::lock_guard<std::mutex> g(warmLock);
        if (!warmed[url]) {
            warmed[url] = true;
            std::thread([url, sam] { engine::warmUp(comfy::Server::fromUrl(url), sam); }).detach();
        }
    }

    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetPointer(props, kOfxPropInstanceData, 0, inst);
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle effect) {
    delete getInstance(effect);
    return kOfxStatOK;
}

// Backend selection. Returns "" or an error to show.
static std::string keyStatus() {
    std::string hint = comfy::Server::cloudKeyHint();
    return hint.empty() ? "not set - press Set API Key..." : "saved (" + hint + ")";
}

// A small modal password box for the API key (Win32, no resources): returns false on cancel.
static LRESULT CALLBACK keyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            SetWindowLongPtrW(h, GWLP_USERDATA, LOWORD(w));
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        SetWindowLongPtrW(h, GWLP_USERDATA, IDCANCEL);
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool askApiKey(std::string& key) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = keyWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DUGcoApiKeyBox";
        RegisterClassW(&wc);
        registered = true;
    }
    HWND owner = GetForegroundWindow();
    RECT r{};
    GetWindowRect(owner ? owner : GetDesktopWindow(), &r);
    const int W = 520, H = 190;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"DUGcoApiKeyBox", L"DUGco AlphaGenSAM - Comfy Cloud API key",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, (r.left + r.right - W) / 2, (r.top + r.bottom - H) / 2, W, H,
                               owner, nullptr, inst, nullptr);
    if (!dlg) return false;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id) {
        HWND c = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x,
                                 y, w, hgt, dlg, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    add(L"STATIC",
        L"Paste your Comfy Cloud API key (create one at platform.comfy.org while signed in to your Comfy Cloud "
        L"account). It is saved to Documents\\DUGco on this computer, never in the Nuke script.",
        0, 14, 12, 480, 44, 0);
    HWND edit = add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, 14, 64, 480, 24, 100);
    add(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 300, 104, 94, 28, IDOK);
    add(L"BUTTON", L"Cancel", WS_TABSTOP, 400, 104, 94, 28, IDCANCEL);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(edit);
    std::wstring typed;
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && IsChild(dlg, msg.hwnd)) {
            if (msg.wParam == VK_RETURN) {
                int n = GetWindowTextLengthW(edit);
                typed.resize(n);
                if (n) GetWindowTextW(edit, &typed[0], n + 1);
            }
            SendMessageW(dlg, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
            continue;
        }
        if (msg.message == WM_LBUTTONUP && GetDlgCtrlID(msg.hwnd) == IDOK && IsChild(dlg, msg.hwnd)) {
            int n = GetWindowTextLengthW(edit);
            typed.resize(n);
            if (n) GetWindowTextW(edit, &typed[0], n + 1);
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (typed.empty()) return false;
    int len = WideCharToMultiByte(CP_UTF8, 0, typed.c_str(), (int)typed.size(), nullptr, 0, nullptr, nullptr);
    key.assign(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, typed.c_str(), (int)typed.size(), &key[0], len, nullptr, nullptr);
    SecureZeroMemory(&typed[0], typed.size() * sizeof(wchar_t));
    return true;
}

static std::string backendServer(Instance* inst, comfy::Server& s) {
    if (getInt(inst->backend) == kBackendCloud) {
        std::string err;
        s = comfy::Server::cloudServer(err);
        return err;
    }
    s = comfy::Server::fromUrl(getString(inst->serverUrl));
    return "";
}

// Gather params + geometry into an engine job. now = current frame. Returns "" or an error.
static std::string buildJob(Instance* inst, double now, engine::Job& j, int& first, bool forPreview) {
    std::string err = backendServer(inst, j.server);
    if (!err.empty()) return err;
    // Nuke can drop a value set while the node is being created: make sure it has its id
    if (getString(inst->session).empty()) gParam->paramSetValue(inst->session, newSessionId().c_str());
    j.session = getString(inst->session) + "_" + selectionPrint(inst);
    j.dir = sessionDir(inst);
    j.models.sam = getString(inst->samModel);
    j.models.unet = getString(inst->unetModel);
    j.models.lora = getString(inst->icLora);
    j.models.textEncoder = getString(inst->textEnc);
    j.models.videoVae = getString(inst->videoVae);
    j.models.audioVae = getString(inst->audioVae);
    j.mode = getInt(inst->mode);
    j.chunk = std::clamp(getInt(inst->chunk), 9, 145);
    j.blend = std::clamp(getInt(inst->blend), 0, 48);
    j.seed = getInt(inst->seed);
    j.detThresh = getDouble(inst->detThresh);
    j.othersPrompt = getString(inst->others);

    int a, b;
    if (getInt(inst->rangeMode) == kRangeCustom) {
        a = getInt(inst->frameStart);
        b = getInt(inst->frameEnd);
    } else {
        inputRange(inst, a, b);
    }
    if (b < a) std::swap(a, b);
    first = a;
    j.count = b - a + 1;

    std::vector<VPoint> pts = parsePoints(getString(inst->points));
    int ref = (int)std::lround(now);  // preview frame; clicked objects carry their own key frames
    if (ref < a || ref > b) {
        if (forPreview) return strf("Frame %d is outside the range %d-%d.", ref, a, b);
        ref = std::clamp(ref, a, b);
    }
    for (const VPoint& p : pts)
        if (p.frame < a || p.frame > b)
            return strf("Clicked points on frame %d are outside the range %d-%d (Clear Points or change the range).",
                        p.frame, a, b);
    j.ref = ref - a;
    j.firstFrame = a;
    OfxRectD rod;
    if (gEffect->clipGetRegionOfDefinition(inst->src, ref, &rod) != kOfxStatOK || rod.x2 <= rod.x1)
        return "Input is not connected.";
    workingSize(rod, std::min(getInt(inst->workRes), 1920), j.W, j.H);

    for (const std::string& t : splitTerms(getString(inst->select))) {
        engine::Object o;
        o.text = t;
        j.objects.push_back(o);
    }
    std::vector<int> tint;  // preview colour per object: clicked n -> n-1, typed -> 6+
    for (size_t t = 0; t < j.objects.size(); ++t) tint.push_back(kClickObjects + (int)t);
    for (int slot = 1; slot <= 1; ++slot) {
        engine::Object clicked;
        clicked.slot = slot;
        bool anyPositive = false;
        std::map<int, engine::Key> keys;  // per frame
        for (const VPoint& p : pts) {
            if (p.obj != slot) continue;
            double px = (p.x - rod.x1) / (rod.x2 - rod.x1) * j.W;
            double py = (rod.y2 - p.y) / (rod.y2 - rod.y1) * j.H;  // top-left origin for SAM
            engine::Point q{std::clamp(px, 0.0, j.W - 1.0), std::clamp(py, 0.0, j.H - 1.0), p.positive};
            engine::Key& key = keys[p.frame - a];
            key.frame = p.frame - a;
            key.points.push_back(q);
        }
        for (auto& [f, key] : keys) {
            bool pos = false;
            for (auto& q : key.points) pos |= q.positive;
            if (!pos) continue;  // a frame with only exclude points isn't a key
            if (f == j.ref) clicked.points = key.points;
            clicked.keys.push_back(key);
        }
        if ((int)clicked.keys.size() > engine::kMaxKeys)
            return strf("Your clicks are on %d frames; the most is %d (Alt+click to delete some).",
                        (int)clicked.keys.size(), engine::kMaxKeys);
        anyPositive = forPreview ? !clicked.points.empty() : !clicked.keys.empty();
        if (!forPreview && clicked.points.empty() && !clicked.keys.empty()) clicked.points = clicked.keys[0].points;
        if (anyPositive) {
            j.objects.push_back(clicked);
            tint.push_back(slot - 1);
        }
    }
    if (j.objects.empty() && j.mode != engine::kAlphaGenOnly)
        return "Nothing selected: type what you want (e.g. car) in \"select\", or Ctrl+click it in the viewer.";
    {  // colours for the preview tint (read by render)
        std::error_code ec;
        fs::create_directories(j.dir, ec);
        std::ofstream f(j.dir / "tint_colours.txt");
        for (int v : tint) f << v << "\n";
    }
    return "";
}

static engine::Progress progressFor(OfxImageEffectHandle effect) {
    return [effect](double v, const std::string&) {
        return !(gProgress && gProgress->progressUpdate(effect, std::clamp(v, 0.0, 1.0)) == kOfxStatReplyNo);
    };
}

// Start a hero-frame selection preview in the background and switch the viewer to it.
static void startPreview(OfxImageEffectHandle effect, Instance* inst, double now, bool quiet) {
    engine::Job j;
    int first = 0;
    std::string err = buildJob(inst, now, j, first, true);
    if (!err.empty()) {
        if (!quiet) message(effect, kOfxMessageError, err);
        return;
    }
    // Clicks come in bursts on one frame. While the viewer waits on the previous click's
    // preview Nuke won't hand over the input again ("busy"), so reuse the frame the first
    // click fetched (same node, frame, size and colour; for 2 minutes).
    struct CachedFrame {
        ULONGLONG at;
        std::shared_ptr<engine::Frame> frame;
    };
    static std::mutex frameLock;
    static std::map<std::string, CachedFrame> frameCache;
    const std::string fkey = strf("%s|%d|%dx%d|%d", getString(inst->session).c_str(), first + j.ref, j.W, j.H,
                                  getInt(inst->colour));
    std::shared_ptr<engine::Frame> frame;
    {
        std::lock_guard<std::mutex> g(frameLock);
        auto it = frameCache.find(fkey);
        if (it != frameCache.end() && GetTickCount64() - it->second.at < 120000) frame = it->second.frame;
    }
    if (!frame) {
        frame = std::make_shared<engine::Frame>();
        if (!fetchFrame(inst, first + j.ref, j.W, j.H, getInt(inst->colour), *frame, err)) {
            if (quiet) return setStatus(inst, "preview skipped: Nuke was busy - press Preview");
            return message(effect, kOfxMessageError, err);
        }
        std::lock_guard<std::mutex> g(frameLock);
        frameCache[fkey] = {GetTickCount64(), frame};
    }
    auto job = std::make_shared<PreviewJob>();
    std::shared_ptr<PreviewJob> prev;
    {
        std::lock_guard<std::mutex> g(gJobLock);
        prev = gPreviewJobs[j.session];
        gPreviewJobs[j.session] = job;
    }
    std::thread([j, frame, job, prev] {
        while (prev && !prev->done) Sleep(20);  // newest result must land last
        engine::Result r = engine::preview(j, *frame, nullptr);
        job->ok = r.ok;
        job->error = r.error;
        job->info = r.info;
        job->done = true;
    }).detach();
    if (gMessage2) gMessage2->clearPersistentMessage(effect);
    gParam->paramSetValue(inst->show, (int)kShowPreview);
    setStatus(inst, strf("previewing frame %d", first + j.ref));
    bumpCache(inst);  // the viewer re-renders and waits for the job
}

static void doProcess(OfxImageEffectHandle effect, Instance* inst, double now) {
    engine::Job j;
    int first = 0;
    std::string err = buildJob(inst, now, j, first, false);
    if (!err.empty()) return message(effect, kOfxMessageError, err);

    // frames are fetched from the host on demand and kept for the whole operation
    std::map<int, engine::Frame> frames;
    std::string frameErr;
    const int colour = getInt(inst->colour);
    auto source = [&](int i) -> const engine::Frame* {
        auto it = frames.find(i);
        if (it != frames.end()) return &it->second;
        engine::Frame f;
        if (!fetchFrame(inst, first + i, j.W, j.H, colour, f, frameErr)) return nullptr;
        return &(frames[i] = std::move(f));
    };
    if (gProgress) gProgress->progressStart(effect, "AlphaGenSAM: processing");
    setStatus(inst, "processing...");
    DWORD t0 = GetTickCount();
    engine::Result r = engine::analyze(j, source, progressFor(effect));
    if (gProgress) gProgress->progressEnd(effect);
    if (!r.ok) {
        if (!r.cancelled) message(effect, kOfxMessageError, frameErr.empty() ? r.error : frameErr + "\n" + r.error);
        return setStatus(inst, r.cancelled ? "cancelled" : "failed - see error");
    }
    gParam->paramSetValue(inst->anaStart, first);
    gParam->paramSetValue(inst->anaCount, j.count);
    gParam->paramSetValue(inst->show, (int)kShowMatte);
    int secs = (int)((GetTickCount() - t0) / 1000);
    setStatus(inst, strf("processed %d-%d in %dm%02ds - %s", first, first + j.count - 1, secs / 60, secs % 60,
                         r.info.empty() ? "done" : r.info.c_str()));
    bumpCache(inst);
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    char* type = nullptr;
    char* name = nullptr;
    char* reason = nullptr;
    gProp->propGetString(inArgs, kOfxPropType, 0, &type);
    gProp->propGetString(inArgs, kOfxPropName, 0, &name);
    gProp->propGetString(inArgs, kOfxPropChangeReason, 0, &reason);
    if (!type || strcmp(type, kOfxTypeParameter) != 0 || !name) return kOfxStatReplyDefault;
    const bool byUser = reason && strcmp(reason, kOfxChangeUserEdited) == 0;
    const bool byPlugin = reason && strcmp(reason, kOfxChangePluginEdited) == 0;
    double now = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &now);
    Instance* inst = getInstance(effect);
    std::string n = name;
    if (n == "points" && (byUser || byPlugin)) {  // set by the viewer overlay
        std::vector<VPoint> pts = parsePoints(getString(inst->points));
        if (getInt(inst->autoPreview) && !pts.empty()) startPreview(effect, inst, activeFrame(pts), true);
        else if (pts.empty()) setStatus(inst, "points cleared");
        return kOfxStatOK;
    }
    if (!byUser) return kOfxStatReplyDefault;
    if (n == "select") {
        if (getInt(inst->autoPreview) && !splitTerms(getString(inst->select)).empty()) startPreview(effect, inst, now, true);
    } else if (n == "preview") {
        startPreview(effect, inst, now, false);
    } else if (n == "process") {
        doProcess(effect, inst, now);
    } else if (n == "clearPoints") {
        gParam->paramSetValue(inst->points, "");
    } else if (n == "rangeMode") {
        updateRangeEnabled(inst);
        if (getInt(inst->rangeMode) == kRangeCustom) {
            int a, b;
            inputRange(inst, a, b);
            gParam->paramSetValue(inst->frameStart, a);
            gParam->paramSetValue(inst->frameEnd, b);
        }
    } else if (n == "testServer") {
        comfy::Server s;
        std::string err = backendServer(inst, s), info;
        if (err.empty() && comfy::ping(s, info, err))
            message(effect, kOfxMessageMessage, "Connected to ComfyUI " + info + " at " + s.label());
        else
            message(effect, kOfxMessageError, err);
    } else if (n == "setKey") {
        std::string key, err;
        if (askApiKey(key)) {
            if (comfy::Server::saveCloudKey(key, err)) {
                gParam->paramSetValue(inst->cloudKey, keyStatus().c_str());
                comfy::Server cs = comfy::Server::cloudServer(err);
                std::string info;
                if (comfy::ping(cs, info, err))
                    message(effect, kOfxMessageMessage, "API key saved. Connected to Comfy Cloud.");
                else
                    message(effect, kOfxMessageError, "API key saved, but Comfy Cloud refused it:\n" + err);
            } else {
                message(effect, kOfxMessageError, "Could not save the API key: " + err);
            }
            SecureZeroMemory(&key[0], key.size());
        }
    } else if (n == "getKey") {
        ShellExecuteW(nullptr, L"open", L"https://platform.comfy.org/login", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (n == "backend") {
        if (getInt(inst->backend) == kBackendCloud) {  // say straight away if the key is missing
            comfy::Server cs;
            std::string err;
            cs = comfy::Server::cloudServer(err);
            if (!err.empty()) message(effect, kOfxMessageWarning, err);
        }
    } else if (n == "clearCache") {
        std::error_code ec;
        fs::remove_all(sessionRoot(inst), ec);
        gParam->paramSetValue(inst->anaCount, 0);
        setStatus(inst, "cache cleared");
        bumpCache(inst);
    } else {
        return kOfxStatReplyDefault;
    }
    return kOfxStatOK;
}

// ------------------------------------------------------------ viewer overlay
//
// Ctrl+click adds an include point (green), Ctrl+Shift+click an exclude point (red),
// plain drag on a dot moves it, Alt+click on a dot deletes it. Modifier state comes
// straight from Windows, so it works however the host forwards key events.

static Instance* overlayInstance(const void* handle, OfxImageEffectHandle* effectOut = nullptr) {
    OfxPropertySetHandle props;
    gInteract->interactGetPropertySet((OfxInteractHandle)handle, &props);
    void* eff = nullptr;
    gProp->propGetPointer(props, kOfxPropEffectInstance, 0, &eff);
    if (effectOut) *effectOut = (OfxImageEffectHandle)eff;
    return eff ? getInstance((OfxImageEffectHandle)eff) : nullptr;
}

static void pixelScale(const void* handle, OfxPropertySetHandle inArgs, double& sx, double& sy) {
    double ps[2] = {0, 0};
    if (gProp->propGetDoubleN(inArgs, kOfxInteractPropPixelScale, 2, ps) != kOfxStatOK || ps[0] <= 0) {
        OfxPropertySetHandle props;
        gInteract->interactGetPropertySet((OfxInteractHandle)handle, &props);
        gProp->propGetDoubleN(props, kOfxInteractPropPixelScale, 2, ps);
    }
    sx = ps[0] > 0 ? ps[0] : 1.0;
    sy = ps[1] > 0 ? ps[1] : 1.0;
}

static bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void disc(double cx, double cy, double rx, double ry) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(cx, cy);
    for (int i = 0; i <= 32; ++i) {
        double a = i * 6.283185307 / 32;
        glVertex2d(cx + rx * std::cos(a), cy + ry * std::sin(a));
    }
    glEnd();
}
static void ring(double cx, double cy, double rx, double ry) {
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 32; ++i) {
        double a = i * 6.283185307 / 32;
        glVertex2d(cx + rx * std::cos(a), cy + ry * std::sin(a));
    }
    glEnd();
}

static OfxStatus overlayDraw(const void* handle, OfxPropertySetHandle inArgs) {
    Instance* inst = overlayInstance(handle);
    if (!inst) return kOfxStatReplyDefault;
    double t = 0, sx, sy;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    pixelScale(handle, inArgs, sx, sy);
    std::vector<VPoint> pts = inst->dragIndex >= 0 ? inst->dragPoints : parsePoints(getString(inst->points));
    if (pts.empty()) return kOfxStatOK;
    const int frame = (int)std::lround(t);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    const double R = 9.0;  // marker radius in screen pixels
    for (size_t i = 0; i < pts.size(); ++i) {
        const VPoint& p = pts[i];
        const bool live = false;  // every frame's points count (keyframes); other frames show as rings
        if (p.frame != frame) {  // a key on another frame: thin ring only
            glLineWidth(1.5f);
            const float* col = p.positive ? kTint[(p.obj - 1) % 12] : nullptr;
            if (col) glColor4f(col[0], col[1], col[2], live ? 0.9f : 0.35f);
            else glColor4f(1.f, 0.35f, 0.3f, live ? 0.9f : 0.35f);
            ring(p.x, p.y, R * sx, R * sy);
            continue;
        }
        const float* col = kTint[(p.obj - 1) % 12];
        glColor4f(0.f, 0.f, 0.f, 0.85f);  // dark halo so it reads on any plate
        disc(p.x, p.y, (R + 3) * sx, (R + 3) * sy);
        if (p.positive) {
            glColor4f(col[0], col[1], col[2], 1.f);  // the click object's colour
            disc(p.x, p.y, R * sx, R * sy);
        } else {  // exclude: red, ringed in the object's colour
            glColor4f(col[0], col[1], col[2], 1.f);
            disc(p.x, p.y, R * sx, R * sy);
            glColor4f(1.f, 0.25f, 0.2f, 1.f);
            disc(p.x, p.y, (R - 3) * sx, (R - 3) * sy);
        }
        glColor4f(0.f, 0.f, 0.f, 1.f);  // + / - glyph
        glLineWidth(2.5f);
        glBegin(GL_LINES);
        glVertex2d(p.x - 5 * sx, p.y);
        glVertex2d(p.x + 5 * sx, p.y);
        if (p.positive) {
            glVertex2d(p.x, p.y - 5 * sy);
            glVertex2d(p.x, p.y + 5 * sy);
        }
        glEnd();
    }
    glPopAttrib();
    return kOfxStatOK;
}

static int nearestPoint(const std::vector<VPoint>& pts, double x, double y, double sx, double sy, int frame) {
    int best = -1;
    double bestD = 14.0;  // screen pixels
    for (size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].frame != frame) continue;
        double dx = (pts[i].x - x) / sx, dy = (pts[i].y - y) / sy, d = std::sqrt(dx * dx + dy * dy);
        if (d < bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    return best;
}

static void commitPoints(OfxImageEffectHandle effect, Instance* inst, const std::vector<VPoint>& pts, const char* what) {
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    gParam->paramEditBegin(ps, what);
    gParam->paramSetValue(inst->points, formatPoints(pts).c_str());
    gParam->paramEditEnd(ps);
}

static OfxStatus overlayPen(const char* action, const void* handle, OfxPropertySetHandle inArgs) {
    OfxImageEffectHandle effect = nullptr;
    Instance* inst = overlayInstance(handle, &effect);
    if (!inst) return kOfxStatReplyDefault;
    double pos[2] = {0, 0}, t = 0, sx, sy;
    gProp->propGetDoubleN(inArgs, kOfxInteractPropPenPosition, 2, pos);
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    pixelScale(handle, inArgs, sx, sy);
    const int frame = (int)std::lround(t);

    if (!strcmp(action, kOfxInteractActionPenDown)) {
        std::vector<VPoint> pts = parsePoints(getString(inst->points));
        int hit = nearestPoint(pts, pos[0], pos[1], sx, sy, frame);
        if (keyDown(VK_CONTROL)) {
            VPoint p;
            p.x = pos[0];
            p.y = pos[1];
            p.positive = !keyDown(VK_SHIFT);
            p.frame = frame;
            p.obj = 1;
            pts.push_back(p);
            commitPoints(effect, inst, pts, p.positive ? "Add point" : "Add exclude point");
            gInteract->interactRedraw((OfxInteractHandle)handle);
            return kOfxStatOK;
        }
        if (hit >= 0 && keyDown(VK_MENU)) {  // Alt+click a dot: delete it
            pts.erase(pts.begin() + hit);
            commitPoints(effect, inst, pts, "Delete point");
            gInteract->interactRedraw((OfxInteractHandle)handle);
            return kOfxStatOK;
        }
        if (hit >= 0) {  // start dragging a dot
            inst->dragIndex = hit;
            inst->dragPoints = pts;
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    }
    if (inst->dragIndex < 0) return kOfxStatReplyDefault;
    inst->dragPoints[inst->dragIndex].x = pos[0];
    inst->dragPoints[inst->dragIndex].y = pos[1];
    gInteract->interactRedraw((OfxInteractHandle)handle);
    if (!strcmp(action, kOfxInteractActionPenUp)) {
        std::vector<VPoint> pts = inst->dragPoints;
        inst->dragIndex = -1;
        commitPoints(effect, inst, pts, "Move point");  // the preview refreshes once, on release
    }
    return kOfxStatOK;
}

static OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                             OfxPropertySetHandle) {
    try {
        if (!strcmp(action, kOfxActionDescribe) || !strcmp(action, kOfxActionCreateInstance) ||
            !strcmp(action, kOfxActionDestroyInstance))
            return kOfxStatOK;
        if (!gInteract) return kOfxStatReplyDefault;
        if (!strcmp(action, kOfxInteractActionDraw)) return overlayDraw(handle, inArgs);
        if (!strcmp(action, kOfxInteractActionPenDown) || !strcmp(action, kOfxInteractActionPenMotion) ||
            !strcmp(action, kOfxInteractActionPenUp))
            return overlayPen(action, handle, inArgs);
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ------------------------------------------------------------------ render

static std::shared_ptr<Gray> cached(Instance* inst, const std::string& key,
                                    const std::function<std::shared_ptr<Gray>()>& make) {
    {
        std::lock_guard<std::mutex> g(inst->lock);
        auto it = inst->cache.find(key);
        if (it != inst->cache.end()) return it->second;
    }
    auto v = make();
    if (!v) return v;  // don't remember misses: the file may arrive with the next run
    std::lock_guard<std::mutex> g(inst->lock);
    inst->cache[key] = v;
    inst->order.push_back(key);
    while (inst->order.size() > 24) {
        inst->cache.erase(inst->order.front());
        inst->order.erase(inst->order.begin());
    }
    return v;
}

static std::shared_ptr<Gray> loadMatte(Instance* inst, const fs::path& dir, const std::string& kind, int idx) {
    fs::path p = engine::matteFile(dir, kind.c_str(), idx);
    // Nuke renders through other instances than the one that ran Process, so the key
    // carries the shared cacheVersion knob instead of relying on bumpCache().
    std::string key = strf("v%d|", getInt(inst->cacheVersion)) + p.string();
    return cached(inst, key, [&]() -> std::shared_ptr<Gray> {
        auto g = std::make_shared<Gray>();
        return engine::readPFM(p, *g) ? g : nullptr;
    });
}

static void unionInto(std::shared_ptr<Gray>& acc, const Gray& g) {
    if (!acc) {
        acc = std::make_shared<Gray>(g);
        return;
    }
    if (acc->w != g.w || acc->h != g.h) return;
    for (size_t i = 0; i < g.px.size(); ++i) acc->px[i] = std::max(acc->px[i], g.px[i]);
}

static int previewCount(const fs::path& dir) {
    std::ifstream f(dir / "preview_objects.txt");
    int n = 0;
    f >> n;
    return n;
}

static int matteSource(Instance* inst, double time) {
    if (getInt(inst->show, time) == kShowPreview) return kOutPreview;
    switch (getInt(inst->debug, time)) {
        case kDebugSelection: return kOutSelection;
        case kDebugPlain: return kOutPlain;
        case kDebugIsolated: return kOutIsolated;
        default: return kOutFinal;
    }
}

// Which processed objects this node outputs (its whole selection).
static std::vector<int> renderOutputs(Instance*, const fs::path& dir) { return engine::outputObjects(dir); }

static std::shared_ptr<Gray> matteFor(Instance* inst, double time) {
    int out = matteSource(inst, time);
    fs::path dir = sessionDir(inst);
    int ver = getInt(inst->cacheVersion);
    if (out == kOutPreview) {
        int n = previewCount(dir);
        return cached(inst, strf("v%d|preview-union", ver), [&]() -> std::shared_ptr<Gray> {
            std::shared_ptr<Gray> acc;
            for (int k = 0; k < n; ++k)
                if (auto m = loadMatte(inst, dir, strf("preview%d", k), 0)) unionInto(acc, *m);
            return acc;
        });
    }
    int anaStart = getInt(inst->anaStart), anaCount = getInt(inst->anaCount);
    int idx = (int)std::lround(time) - anaStart;
    if (anaCount <= 0 || idx < 0 || idx >= anaCount) return nullptr;
    int K = engine::objectCount(dir);
    if (out == kOutPlain) return loadMatte(inst, dir, "ag", idx);
    if (out == kOutSelection || out == kOutIsolated) {
        return cached(inst, strf("v%d|%d|union%d", ver, idx, out), [&]() -> std::shared_ptr<Gray> {
            std::shared_ptr<Gray> acc;
            for (int k : renderOutputs(inst, dir)) {
                std::string kind = out == kOutSelection ? engine::selKind(k)
                                   : engine::pass2Kind(dir, k) == "clean" ? engine::cleanKind(k)
                                                                           : engine::isoKind(k);
                if (auto m = loadMatte(inst, dir, kind, idx)) unionInto(acc, *m);
            }
            return acc;
        });
    }
    int mode = getInt(inst->mode, time), reach = getInt(inst->reach, time);
    double bp = getDouble(inst->blackPt, time), wp = getDouble(inst->whitePt, time);
    std::string key = strf("v%d|final|%d|%d|%d|%g|%g", ver, idx, mode, reach, bp, wp);
    return cached(inst, key, [&]() -> std::shared_ptr<Gray> {
        auto plain = loadMatte(inst, dir, "ag", idx);
        std::shared_ptr<Gray> acc;
        if (K == 0) {  // alpha-gen only
            Gray f = engine::composeFinal(mode, plain.get(), nullptr, nullptr, nullptr, reach, bp, wp, false);
            return f.px.empty() ? nullptr : std::make_shared<Gray>(std::move(f));
        }
        for (int k : renderOutputs(inst, dir)) {  // each object composed on its own, then merged
            if (k >= K) continue;
            auto sel = loadMatte(inst, dir, engine::selKind(k), idx);
            if (engine::pass2Kind(dir, k) == "clean") {
                auto clean = loadMatte(inst, dir, engine::fillKind(k), idx);
                if (!clean) clean = loadMatte(inst, dir, engine::cleanKind(k), idx);
                if (!clean || !sel) continue;
                auto hole = loadMatte(inst, dir, engine::holeKind(k), idx);
                Gray f = engine::composeClean(*clean, hole.get(), *sel, reach, bp, wp);
                if (!f.px.empty()) unionInto(acc, f);
                continue;
            }
            auto iso = loadMatte(inst, dir, engine::isoKind(k), idx);
            auto oth = loadMatte(inst, dir, engine::othKind(k), idx);
            Gray f = engine::composeFinal(mode, plain.get(), iso.get(), sel.get(), oth.get(), reach, bp, wp,
                                          engine::pass2Steered(dir, k));
            if (!f.px.empty()) unionInto(acc, f);
        }
        return acc;
    });
}

static std::vector<int> previewTints(const fs::path& dir) {
    std::vector<int> out;
    std::ifstream f(dir / "tint_colours.txt");
    for (int v; f >> v;) out.push_back(v);
    return out;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    Instance* inst = getInstance(effect);
    double time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxRectI win;
    gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &win.x1);
    double rs[2] = {1, 1};
    gProp->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, rs);

    OfxPropertySetHandle dstImg = nullptr, srcImg = nullptr;
    if (gEffect->clipGetImage(inst->dst, time, nullptr, &dstImg) != kOfxStatOK) return kOfxStatFailed;
    gEffect->clipGetImage(inst->src, time, nullptr, &srcImg);
    ImageView d = viewImage(dstImg), s = viewImage(srcImg);

    const int show = getInt(inst->show, time);
    const int out = matteSource(inst, time);
    if (out == kOutPreview) {  // a background preview may still be running: wait for it here, off the UI thread
        if (auto job = previewJob(getString(inst->session) + "_" + selectionPrint(inst))) {
            while (!job->done && !gEffect->abort(effect)) Sleep(30);
            if (job->done && gMessage2) {
                if (!job->ok) gMessage2->setPersistentMessage(effect, kOfxMessageError, nullptr, "%s", job->error.c_str());
                else if (!job->info.empty())
                    gMessage2->setPersistentMessage(effect, kOfxMessageWarning, nullptr, "SAM 3.1 found nothing for %s",
                                                    job->info.c_str());
            }
        }
    }
    std::shared_ptr<Gray> m = matteFor(inst, time);
    bool analyzed = getInt(inst->anaCount) > 0 || out == kOutPreview;

    // selection preview: tint each object in its own colour
    std::vector<std::shared_ptr<Gray>> tints;
    bool tint = out == kOutPreview;
    std::vector<int> tintIndex;
    if (tint) {
        fs::path dir = sessionDir(inst);
        for (int k = 0, n = previewCount(dir); k < n; ++k) tints.push_back(loadMatte(inst, dir, strf("preview%d", k), 0));
        tintIndex = previewTints(dir);
    }

    OfxRectD rod{0, 0, 1, 1};
    gEffect->clipGetRegionOfDefinition(inst->src, time, &rod);
    double par = pixelAspect(inst->src);
    double rx1 = rod.x1 * rs[0] / par, rx2 = rod.x2 * rs[0] / par, ry1 = rod.y1 * rs[1], ry2 = rod.y2 * rs[1];
    bool toRGB = show == kShowMatte, pre = show == kShowCutout;

    auto sample = [&](const Gray& g, int x, int y) {
        double fy = (1.0 - (y + 0.5 - ry1) / (ry2 - ry1)) * g.h - 0.5;  // mattes are top-to-bottom
        double fx = (x + 0.5 - rx1) / (rx2 - rx1) * g.w - 0.5;
        int y0 = std::clamp((int)std::floor(fy), 0, g.h - 1), y1 = std::min(y0 + 1, g.h - 1);
        int x0 = std::clamp((int)std::floor(fx), 0, g.w - 1), x1 = std::min(x0 + 1, g.w - 1);
        double ay = std::clamp(fy - std::floor(fy), 0.0, 1.0), ax = std::clamp(fx - std::floor(fx), 0.0, 1.0);
        const float* r0 = &g.px[(size_t)y0 * g.w];
        const float* r1 = &g.px[(size_t)y1 * g.w];
        return (float)((r0[x0] * (1 - ax) + r0[x1] * ax) * (1 - ay) + (r1[x0] * (1 - ax) + r1[x1] * ax) * ay);
    };

    for (int y = win.y1; y < win.y2; ++y) {
        if (gEffect->abort(effect)) break;
        if (y < d.bounds.y1 || y >= d.bounds.y2) continue;
        void* row = (char*)d.data + (ptrdiff_t)(y - d.bounds.y1) * d.rowBytes;
        for (int x = win.x1; x < win.x2; ++x) {
            if (x < d.bounds.x1 || x >= d.bounds.x2) continue;
            float rgb[3] = {0, 0, 0}, srcA = 1.f;
            if (s.ok && x >= s.bounds.x1 && x < s.bounds.x2 && y >= s.bounds.y1 && y < s.bounds.y2) {
                for (int c = 0; c < 3; ++c) rgb[c] = pixelComp(s, x, y, c);
                if (s.nComp == 4) srcA = pixelComp(s, x, y, 3);
            }
            float a = m ? sample(*m, x, y) : (analyzed ? 0.f : srcA);  // pass-through before processing
            if (tint) {
                for (size_t k = 0; k < tints.size(); ++k) {
                    if (!tints[k]) continue;
                    float w = 0.5f * sample(*tints[k], x, y);
                    const float* col = kTint[(k < tintIndex.size() ? tintIndex[k] : (int)k) % 12];
                    for (int c = 0; c < 3; ++c) rgb[c] = rgb[c] * (1 - w) + col[c] * w;
                }
            } else if (toRGB) {
                rgb[0] = rgb[1] = rgb[2] = a;
            } else if (pre) {
                for (float& c : rgb) c *= a;
            }
            size_t i = (size_t)(x - d.bounds.x1) * d.nComp;
            for (int c = 0; c < 3 && c < d.nComp; ++c) storeComp(row, d.bytesPerComp, i + c, rgb[c]);
            if (d.nComp == 4) storeComp(row, d.bytesPerComp, i + 3, a);
        }
    }
    if (srcImg) gEffect->clipReleaseImage(srcImg);
    gEffect->clipReleaseImage(dstImg);
    return kOfxStatOK;
}

static OfxStatus getClipPreferences(OfxImageEffectHandle, OfxPropertySetHandle outArgs) {
    gProp->propSetString(outArgs, "OfxImageClipPropComponents_Output", 0, kOfxImageComponentRGBA);
    gProp->propSetString(outArgs, kOfxImageEffectPropPreMultiplication, 0, kOfxImageUnPreMultiplied);
    return kOfxStatOK;
}

// ------------------------------------------------------------------- entry

static OfxStatus onLoad() {
    gProp = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEffect = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParam = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gMessage = (OfxMessageSuiteV1*)gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1);
    gMessage2 = (OfxMessageSuiteV2*)gHost->fetchSuite(gHost->host, kOfxMessageSuite, 2);
    gProgress = (OfxProgressSuiteV1*)gHost->fetchSuite(gHost->host, kOfxProgressSuite, 1);
    gInteract = (OfxInteractSuiteV1*)gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
    return (gProp && gEffect && gParam) ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    try {
        if (!strcmp(action, kOfxActionLoad)) return onLoad();
        if (!strcmp(action, kOfxActionDescribe)) return describe(effect);
        if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect, inArgs);
        if (!strcmp(action, kOfxActionCreateInstance)) return createInstance(effect);
        if (!strcmp(action, kOfxActionDestroyInstance)) return destroyInstance(effect);
        if (!strcmp(action, kOfxActionInstanceChanged)) return instanceChanged(effect, inArgs);
        if (!strcmp(action, kOfxImageEffectActionGetClipPreferences)) return getClipPreferences(effect, outArgs);
        if (!strcmp(action, kOfxImageEffectActionRender)) return render(effect, inArgs);
    } catch (const std::exception& e) {
        message(effect, kOfxMessageError, std::string("AlphaGenSAM: ") + e.what());
        return kOfxStatFailed;
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

static void setHost(OfxHost* h) { gHost = h; }

static OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, PLUGIN_ID, 1, 0, setHost, mainEntry};

extern "C" {
__declspec(dllexport) int OfxGetNumberOfPlugins(void) { return 1; }
__declspec(dllexport) OfxPlugin* OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : nullptr; }
}
