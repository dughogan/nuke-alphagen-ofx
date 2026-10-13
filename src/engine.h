// AlphaGenSAM engine: everything except the OFX glue, so it can be tested headless.
//
// All ComfyUI graphs use core nodes only (they run unchanged on local ComfyUI and,
// later, Comfy Cloud). Frames go up as an uploaded H.264 MP4 (or PNG for single
// frames); mattes come back as PNG outputs and are cached locally as float PFM.
//
// Grow / softness / haze remap / multiply happen here in the plug-in, so those
// knobs are live in Nuke and never re-run anything in ComfyUI.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "comfy.h"
#include "media.h"

namespace engine {

namespace fs = std::filesystem;
using media::Frame;
using media::Gray;

// auto/isolate/steer: two Alpha-Gen passes (plain, then other objects painted out);
// fast: one pass, split by ownership; alpha-gen only; selection only.
// clean: the other objects are removed with the LTX-2.5 Clean-Plate IC-LoRA before
// Alpha-Gen, and holes are cut by the occluding objects' own Alpha-Gen mattes
// (appended so saved knob values keep their meaning).
enum Mode { kAuto = 0, kIsolate, kSteer, kFast, kAlphaGenOnly, kSelectionOnly, kClean };

// auto mode: steer pass 2 when the plain Alpha-Gen matte covers less than this share
// of the selection (car/person on the diner test: 0.95+; the diner itself: 0.01).
constexpr double kAutoCoverage = 0.5;

struct Point {
    double x, y;  // working-resolution pixels, top-left origin
    bool positive;
};

// Clicked points on one frame (index within the analysed range).
struct Key {
    int frame = 0;
    std::vector<Point> points;
};
constexpr int kMaxKeys = 5;  // keyframes per clicked object

// One thing to matte: a typed term ("car": every car SAM finds) or a clicked object.
// A clicked object can be clicked on several frames (keys): SAM detects it on each and
// tracks forward and backward from there; every frame uses the nearest key that finds it.
// points: its clicks on the reference frame (preview; also the only key if keys is empty).
struct Object {
    std::string text;
    std::vector<Point> points;
    std::vector<Key> keys;
    int slot = 0;         // clicked objects: their number in the viewer (1-based)
    bool output = true;   // false: only removed from the other objects' plates (and cuts their holes)
    bool clicked() const { return !points.empty() || !keys.empty(); }
    std::string label() const { return !clicked() ? "\"" + text + "\"" : "clicked object " + std::to_string(slot); }
};

struct Models {
    std::string sam = "sam3.1_multiplex_fp16.safetensors";
    std::string unet = "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors";
    std::string lora = "ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors";
    std::string textEncoder = "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors";
    std::string videoVae = "ltx-2.5-video-vae-bf16.safetensors";
    std::string audioVae = "ltx-2.5-audio-vae-bf16.safetensors";
    std::string cleanLora = "ltx-2.5-22b-ic-lora-clean-plate-1.0.safetensors";
    std::string upscaler = "ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors";
    std::string depth = "depth_anything_3_mono_large.safetensors";
};

struct Job {
    comfy::Server server;
    std::string session;  // per-node id; names uploads and output folders
    fs::path dir;         // local cache for this node (mattes as PFM)
    Models models;
    int W = 0, H = 0;     // working resolution (multiples of 32)
    int count = 0;        // frames in the analysed range
    int ref = 0;          // reference frame index within the range (clicked objects)
    int firstFrame = 0;   // host frame number of index 0 (messages only)
    std::vector<Object> objects;
    // Other subjects SAM 3.1 should find (comma terms): their instances that aren't the
    // selected object are painted out of pass 2 and cut as holes.
    std::string othersPrompt = "person, car, animal";
    double detThresh = 0.5;
    int mode = kClean;
    int chunk = 121;      // Alpha-Gen frames per chunk (model max 145)
    int blend = 16;       // frames of overlap cross-faded between chunks
    int seed = 42;
};

// Returns frame i (0..count-1) at working resolution, or nullptr on failure.
using FrameSource = std::function<const Frame*(int)>;
// value 0-1 over the whole operation; return false to cancel.
using Progress = std::function<bool(double, const std::string&)>;

struct Result {
    bool ok = false, cancelled = false;
    std::string error;
    std::string info;
};

// Check every model the job needs exists on the server (an imported model may carry a
// prefix, e.g. "Lightricks__repo__file.safetensors": matched by its file name) and
// rewrite the names to what the server calls them. "" or a message naming what's missing.
std::string resolveModels(Job& j, bool needSam, bool needLtx, bool needClean, bool needDepth);

Result preview(const Job& j, const Frame& refFrame, const Progress& p);
// Fire-and-forget: queue a tiny SAM 3.1 job so the model is loaded before the first click.
void warmUp(const comfy::Server& server, const std::string& samModel);
Result analyze(const Job& j, const FrameSource& frames, const Progress& p);

// ---- matte maths + cache files (shared with render) ----
bool readPFM(const fs::path& path, Gray& out);  // top-to-bottom rows
bool writePFM(const fs::path& path, const Gray& g);
Gray growBlur(const Gray& in, int grow, double blur);
// Final alpha for one frame from the cached layers: plain pass ("ag"), isolated pass
// ("agi"), SAM selection ("sel"). grow/blur make only a loose garbage matte; islands
// drops matte regions that don't touch the selection.
// Final alpha of one object for one frame. others: the other subjects (oth<k>); reach:
// how far (px at 1920 wide) Alpha-Gen's matte may extend from the selection; background:
// pass 2 was steered (pass2Steered()).
Gray composeFinal(int mode, const Gray* plain, const Gray* isolated, const Gray* selection, const Gray* others,
                  double reach, double blackPt, double whitePt, bool background);
// Cache layout per analysis: objects.txt (count), sel<k>_*, agi<k>_*, pass2_<k>.txt; ag_* shared.
int objectCount(const fs::path& dir);
std::string selKind(int k);
std::string isoKind(int k);
std::string othKind(int k);
std::string cleanKind(int k);  // clean-plate Alpha-Gen pass: the object's whole shape
std::string holeKind(int k);   // alpha of whatever is in front of the object
std::string temporalKind(int k);  // front object + difference matte over a still object
std::string fillKind(int k);   // clean pass completed behind its occluders (what the final uses)
// Re-cut the holes of a finished clean analysis (compose-only tests).
void cleanHoles(const Job& j, const FrameSource& frames = nullptr);
Result depthOnly(const Job& j, const FrameSource& frames);  // the depth step alone (tests)
bool pass2Steered(const fs::path& dir, int k);
std::string pass2Kind(const fs::path& dir, int k);    // "isolated", "steered", "clean" or "" (none)
std::vector<int> outputObjects(const fs::path& dir);  // objects whose mattes are output (outputs.txt)
// clean mode: remap(clean) * (1 - remap(hole)), kept where connected to the selection
Gray composeClean(const Gray& clean, const Gray* hole, const Gray& selection, double reach, double blackPt,
                  double whitePt);
// The plate Alpha-Gen sees in pass 2: other objects painted out (+ defocused surroundings if steer).
Frame isolatedGuide(const Frame& plate, const Gray& sel, const Gray& others, bool steer);
double coverageOf(const Gray& matte, const Gray& sel);

fs::path matteFile(const fs::path& dir, const char* kind, int index);

}  // namespace engine
