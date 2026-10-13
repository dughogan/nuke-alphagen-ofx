// Headless engine test: runs Preview + Process against a ComfyUI server using raw
// RGB frames from disk, then writes the composed final mattes.
//
//   agsam_selftest <frames_dir> <count> <ref> <mode> <server_url> [x,y,+|-[,slot]]... [text:term]...
//   frames_dir holds frame_%06d.rgb (W*H*3 bytes, top-to-bottom) and size.txt "W H".
//   Points with the same slot (default 1) form one clicked object; every text:term is
//   its own object. Objects are numbered text terms first, then clicked slots.
//   AGSAM_OUTPUTS="0,2": only these objects get a matte (the rest are just removed).
//   Env: AGSAM_SESSION (cache subfolder), AGSAM_COMPOSE_ONLY (skip ComfyUI),
//        AGSAM_GROW / AGSAM_BLUR / AGSAM_NO_ISLANDS (compose), AGSAM_DUMP_GUIDES.
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace engine;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: agsam_selftest frames_dir count ref mode server_url [x,y,+] [text:term] ...\n";
        return 2;
    }
    fs::path dir = argv[1];
    Job j;
    j.count = atoi(argv[2]);
    j.ref = atoi(argv[3]);
    j.mode = atoi(argv[4]);
    if (std::string(argv[5]) == "cloud") {  // Comfy Cloud, key from COMFY_CLOUD_API_KEY or the key file
        std::string err;
        j.server = comfy::Server::cloudServer(err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 1;
        }
    } else {
        j.server = comfy::Server::fromUrl(argv[5]);
    }
    std::map<int, Object> clicked;
    for (int a = 6; a < argc; ++a) {
        double x, y;
        char s;
        int slot = 1, kf = -1;
        if (strncmp(argv[a], "text:", 5) == 0) {
            Object o;
            o.text = argv[a] + 5;
            j.objects.push_back(o);
        } else if (sscanf(argv[a], "%lf,%lf,%c,%d,%d", &x, &y, &s, &slot, &kf) >= 3) {
            Object& o = clicked[slot];
            o.slot = slot;
            const int f = kf < 0 ? -1 : kf;
            if (f < 0) o.points.push_back({x, y, s == '+'});
            auto it = std::find_if(o.keys.begin(), o.keys.end(), [&](const Key& k) { return k.frame == f; });
            if (it == o.keys.end()) o.keys.push_back(Key{f, {}}), it = o.keys.end() - 1;
            it->points.push_back({x, y, s == '+'});
        }
    }
    for (auto& [slot, o] : clicked) {
        for (auto& k : o.keys)
            if (k.frame < 0) k.frame = atoi(argv[3]);  // points without a frame: the reference frame
        for (auto& k : o.keys)
            if (k.frame == atoi(argv[3])) o.points = k.points;
        if (o.points.empty() && !o.keys.empty()) o.points = o.keys[0].points;
        j.objects.push_back(o);
    }
    if (const char* outs = getenv("AGSAM_OUTPUTS")) {
        for (auto& o : j.objects) o.output = false;
        std::stringstream ss(outs);
        for (std::string t; std::getline(ss, t, ',');)
            if (atoi(t.c_str()) < (int)j.objects.size()) j.objects[atoi(t.c_str())].output = true;
    }
    std::ifstream(dir / "size.txt") >> j.W >> j.H;
    j.session = getenv("AGSAM_SESSION") ? getenv("AGSAM_SESSION") : "selftest";
    j.dir = dir / ("cache_" + j.session);
    if (getenv("AGSAM_OTHERS")) j.othersPrompt = getenv("AGSAM_OTHERS");
    if (getenv("AGSAM_CHUNK")) j.chunk = atoi(getenv("AGSAM_CHUNK"));
    if (getenv("AGSAM_BLEND")) j.blend = atoi(getenv("AGSAM_BLEND"));

    std::vector<Frame> frames(j.count);
    for (int i = 0; i < j.count; ++i) {
        char n[64];
        snprintf(n, sizeof(n), "frame_%06d.rgb", i);
        std::ifstream f(dir / n, std::ios::binary);
        frames[i].w = j.W;
        frames[i].h = j.H;
        frames[i].rgb.resize((size_t)j.W * j.H * 3);
        f.read((char*)frames[i].rgb.data(), frames[i].rgb.size());
        if (!f) {
            std::cerr << "missing " << n << "\n";
            return 1;
        }
    }
    if (!getenv("AGSAM_COMPOSE_ONLY")) {
        std::string info, err;
        if (!comfy::ping(j.server, info, err)) {
            std::cerr << "server: " << err << "\n";
            return 1;
        }
        std::cout << "server " << info << "\n";
        auto progress = [](double v, const std::string& t) {
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last).count() > 5) {
                last = now;
                printf("  %5.1f%% %s\n", v * 100, t.c_str());
                fflush(stdout);
            }
            return true;
        };
        auto t0 = std::chrono::steady_clock::now();
        Result r = preview(j, frames[j.ref], progress);
        printf("preview: ok=%d missing=[%s] err=%s (%.1fs)\n", r.ok, r.info.c_str(), r.error.c_str(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (!r.ok) return 1;
        t0 = std::chrono::steady_clock::now();
        r = analyze(j, [&](int i) -> const Frame* { return i >= 0 && i < j.count ? &frames[i] : nullptr; }, progress);
        printf("analyze: ok=%d info=%s err=%s (%.1fs)\n", r.ok, r.info.c_str(), r.error.c_str(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (!r.ok) return 1;
    }
    const int K = objectCount(j.dir);
    const double reach = getenv("AGSAM_REACH") ? atof(getenv("AGSAM_REACH")) : 150.0;
    if (getenv("AGSAM_COMPOSE_ONLY") && getenv("AGSAM_RECUT")) {
        if (getenv("AGSAM_DEPTH")) {
            Result r = depthOnly(j, [&](int i) -> const Frame* { return i >= 0 && i < j.count ? &frames[i] : nullptr; });
            printf("depth: ok=%d err=%s\n", r.ok, r.error.c_str());
        }
        cleanHoles(j, [&](int i) -> const Frame* { return i >= 0 && i < j.count ? &frames[i] : nullptr; });
    }
    const std::vector<int> outputs = outputObjects(j.dir);
    for (int i = 0; i < j.count; ++i) {
        Gray ag;
        bool hasAg = readPFM(matteFile(j.dir, "ag", i), ag);
        Gray acc;
        for (int k : outputs) {
            if (k >= K) continue;
            if (pass2Kind(j.dir, k) == "clean") {
                Gray agc, hol, sel;
                if ((!readPFM(matteFile(j.dir, fillKind(k).c_str(), i), agc) &&
                     !readPFM(matteFile(j.dir, cleanKind(k).c_str(), i), agc)) ||
                    !readPFM(matteFile(j.dir, selKind(k).c_str(), i), sel))
                    continue;
                bool hasHole = readPFM(matteFile(j.dir, holeKind(k).c_str(), i), hol);
                Gray f = composeClean(agc, hasHole ? &hol : nullptr, sel, reach, -0.04, 1.03);
                writePFM(matteFile(j.dir, ("final" + std::to_string(k)).c_str(), i), f);
                if (acc.px.empty()) acc = f;
                else
                    for (size_t p = 0; p < f.px.size(); ++p) acc.px[p] = std::max(acc.px[p], f.px[p]);
                continue;
            }
            Gray agi, sel, oth;
            bool hasAgi = readPFM(matteFile(j.dir, isoKind(k).c_str(), i), agi);
            bool hasSel = readPFM(matteFile(j.dir, selKind(k).c_str(), i), sel);
            bool hasOth = readPFM(matteFile(j.dir, othKind(k).c_str(), i), oth);
            Gray f = composeFinal(j.mode, hasAg ? &ag : nullptr, hasAgi ? &agi : nullptr, hasSel ? &sel : nullptr,
                                  hasOth ? &oth : nullptr, reach, -0.04, 1.03, pass2Steered(j.dir, k));
            if (f.px.empty()) continue;
            writePFM(matteFile(j.dir, ("final" + std::to_string(k)).c_str(), i), f);
            if (acc.px.empty()) acc = f;
            else
                for (size_t p = 0; p < f.px.size(); ++p) acc.px[p] = std::max(acc.px[p], f.px[p]);
        }
        if (!acc.px.empty()) writePFM(matteFile(j.dir, "final", i), acc);
    }
    printf("done (%d objects)\n", K);
    return 0;
}
