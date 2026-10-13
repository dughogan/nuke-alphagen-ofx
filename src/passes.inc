    // ---- Alpha-Gen
    // Pass 1 ("ag"): the plain plate, shared by every object. It finds everything
    // Alpha-Gen treats as foreground and gives the other objects' alpha for holes.
    // Pass 2, once per object:
    //   clean mode ("agc<k>"): the other objects removed with the LTX-2.5 Clean-Plate
    //     IC-LoRA, so Alpha-Gen draws the object's whole shape, even where something
    //     was in front of it; holes ("hol<k>") are then the alpha of whatever is in front.
    //   otherwise ("agi<k>"): the other objects painted out (and, for background
    //     objects, the surroundings defocused), so Alpha-Gen draws that object's own edges.
    // guideMode: 0 plain, 1 isolate, 2 isolate + steer; obj: object index (pass 2)
    std::vector<std::string> plateRemote(chunks);  // pass 1's upload = the untouched plate
    auto runChunk = [&](int ci, const std::string& kind, int guideMode, int obj, double base, double span) -> bool {
        const int s = chunkStart[ci], n = std::min(j.chunk, j.count - s);
        const int blendUntil = ci > 0 ? chunkStart[ci - 1] + std::min(j.chunk, j.count) : -1;
        const int length = ((n - 1 + 7) / 8) * 8 + 1;  // LTX frame rule
        c.base = base;
        c.span = span;
        std::string label = guideMode == 0 ? strf("Alpha-Gen pass 1, chunk %d/%d", ci + 1, chunks)
                                           : strf("Alpha-Gen pass 2 (%s), chunk %d/%d", j.objects[obj].label().c_str(),
                                                  ci + 1, chunks);
        std::vector<Frame> built;
        std::vector<const Frame*> guide;
        if (guideMode == 0) {
            for (int k = 0; k < n; ++k) {
                const Frame* f = frames(s + k);
                if (!f) return c.fail("could not read the input frames");
                guide.push_back(f);
            }
        } else {
            if (!c.step(0.0, label + ": painting out other objects")) return false;
            built.resize(n);
            for (int k = 0; k < n; ++k) {
                const Frame* f = frames(s + k);
                Gray sel, oth;
                if (!f) return c.fail("could not read the input frames");
                if (!readPFM(matteFile(j.dir, selKind(obj).c_str(), s + k), sel) ||
                    !readPFM(matteFile(j.dir, othKind(obj).c_str(), s + k), oth))
                    return c.fail("selection cache missing");
                built[k] = isolatedGuide(*f, sel, oth, guideMode == 2);
                guide.push_back(&built[k]);
            }
            if (getenv("AGSAM_DUMP_GUIDES")) {  // debugging: keep the isolated plates
                std::string e2;
                media::writePNG((j.dir / strf("guide_%s_%06d.png", kind.c_str(), s)).wstring(), built[0], e2);
                media::writePNG((j.dir / strf("guide_%s_%06d.png", kind.c_str(), s + n / 2)).wstring(), built[n / 2], e2);
            }
        }
        while ((int)guide.size() < length) guide.push_back(guide.back());  // pad to 8k+1
        std::string name = strf("dugco_agsam_%s_%s_%03d.mp4", j.session.c_str(), kind.c_str(), ci);
        if (!c.step(0.02, label + ": encoding")) return false;
        if (!media::writeMP4(uploadPath(j, name), guide, 24, err)) return c.fail(err);
        if (!c.step(0.04, label + ": uploading")) return false;
        std::string remote = comfy::upload(j.server, uploadPath(j, name), name, err);
        if (remote.empty()) return c.fail(err);
        if (guideMode == 0) plateRemote[ci] = remote;
        std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>> outs;
        c.base = base + span * 0.06;
        c.span = span * 0.84;
        if (!runGraph(c, alphaGenGraph(j, remote, length, ci, kind.c_str()), "alphagen", label, outs)) return false;
        c.base = 0;
        c.span = 1;
        auto f = outputsOf(outs, 50);
        if (!f) return c.fail("ComfyUI returned no Alpha-Gen frames.");
        return fetchMattes(c, *f, kind.c_str(), [&](int k) { return s + k; }, n, base + span * 0.9, base + span,
                           blendUntil);
    };

    // clean mode, pass 2 of one object: build the hole (the other objects plus their
    // loose strands), send plate + smeared guide + hole + comp matte, get Alpha-Gen of
    // the cleaned plate back.
    auto runClean = [&](int ci, int obj, double base, double span) -> bool {
        const int s = chunkStart[ci], n = std::min(j.chunk, j.count - s);
        const int blendUntil = ci > 0 ? chunkStart[ci - 1] + std::min(j.chunk, j.count) : -1;
        const int length = ((n - 1 + 7) / 8) * 8 + 1;
        const std::string kind = cleanKind(obj);
        c.base = base;
        c.span = span;
        std::string label = strf("Clean plate + Alpha-Gen (%s), chunk %d/%d", j.objects[obj].label().c_str(), ci + 1, chunks);
        if (!c.step(0.0, label + ": building the hole")) return false;
        std::vector<Frame> guideF(n), holeF(n), compF(n);
        for (int k = 0; k < n; ++k) {
            const Frame* f = frames(s + k);
            Gray sel, oth, plain;
            if (!f) return c.fail("could not read the input frames");
            if (!readPFM(matteFile(j.dir, selKind(obj).c_str(), s + k), sel) ||
                !readPFM(matteFile(j.dir, othKind(obj).c_str(), s + k), oth))
                return c.fail("selection cache missing");
            readPFM(matteFile(j.dir, "ag", s + k), plain);
            Gray hole, comp;
            cleanMasks(f->w, f->h, sel, oth, plain.px.empty() ? nullptr : &plain, hole, comp);
            guideF[k] = isolatedGuide(*f, sel, hole, false);
            holeF[k] = grayFrame(hole);
            compF[k] = grayFrame(comp);
        }
        if (getenv("AGSAM_DUMP_GUIDES")) {
            std::string e2;
            media::writePNG((j.dir / strf("guide_%s_%06d.png", kind.c_str(), s + n / 2)).wstring(), guideF[n / 2], e2);
            media::writePNG((j.dir / strf("comp_%s_%06d.png", kind.c_str(), s + n / 2)).wstring(), compF[n / 2], e2);
        }
        const char* tags[3] = {"guide", "hole", "comp"};
        std::vector<Frame>* lists[3] = {&guideF, &holeF, &compF};
        std::string remote[3];
        for (int v = 0; v < 3; ++v) {
            std::vector<const Frame*> ptrs;
            for (auto& fr : *lists[v]) ptrs.push_back(&fr);
            while ((int)ptrs.size() < length) ptrs.push_back(ptrs.back());
            std::string name = strf("dugco_agsam_%s_%s_%s_%03d.mp4", j.session.c_str(), kind.c_str(), tags[v], ci);
            if (!c.step(0.01 + 0.01 * v, label + ": encoding")) return false;
            if (!media::writeMP4(uploadPath(j, name), ptrs, 24, err)) return c.fail(err);
            remote[v] = comfy::upload(j.server, uploadPath(j, name), name, err);
            if (remote[v].empty()) return c.fail(err);
        }
        // job 1: the clean plate
        std::vector<std::pair<std::string, std::vector<comfy::OutputFile>>> outs;
        c.base = base + span * 0.04;
        c.span = span * 0.6;
        if (!runGraph(c, cleanPlateGraph(j, plateRemote[ci], remote[0], remote[1], remote[2], length, ci, kind.c_str()),
                      "clean", label + ": clean plate", outs))
            return false;
        c.base = base;
        c.span = span;
        auto pf = outputsOf(outs, 120);
        if (!pf || (int)pf->size() < n) return c.fail("ComfyUI returned no clean plate.");
        std::vector<Frame> plateF(n);
        for (int k = 0; k < n; ++k) {
            std::string bytes;
            if (!comfy::download(j.server, (*pf)[k], bytes, err) || !media::decodePNG(bytes, plateF[k], err))
                return c.fail(err);
            if (!c.step(0.64 + 0.04 * (k + 1) / n, label + ": downloading the clean plate")) return false;
        }
        if (getenv("AGSAM_DUMP_GUIDES")) {
            std::string e2;
            media::writePNG((j.dir / strf("plate_%s_%06d.png", kind.c_str(), s + n / 2)).wstring(), plateF[n / 2], e2);
        }
        // job 2: Alpha-Gen on it
        std::vector<const Frame*> ptrs;
        for (auto& fr : plateF) ptrs.push_back(&fr);
        while ((int)ptrs.size() < length) ptrs.push_back(ptrs.back());
        std::string name = strf("dugco_agsam_%s_%s_plate_%03d.mp4", j.session.c_str(), kind.c_str(), ci);
        if (!media::writeMP4(uploadPath(j, name), ptrs, 24, err)) return c.fail(err);
        std::string cleanRemote = comfy::upload(j.server, uploadPath(j, name), name, err);
        if (cleanRemote.empty()) return c.fail(err);
        outs.clear();
        c.base = base + span * 0.7;
        c.span = span * 0.24;
        if (!runGraph(c, alphaGenGraph(j, cleanRemote, length, ci, kind.c_str()), "alphagen", label + ": Alpha-Gen", outs))
            return false;
        c.base = 0;
        c.span = 1;
        auto f = outputsOf(outs, 50);
        if (!f) return c.fail("ComfyUI returned no Alpha-Gen frames.");
        return fetchMattes(c, *f, kind.c_str(), [&](int k) { return s + k; }, n, base + span * 0.95, base + span,
                           blendUntil);
    };

    if (chunks > 0) {
        const bool clean = K > 0 && j.mode == kClean;
        const bool twoPass = K > 0 && (j.mode == kAuto || j.mode == kIsolate || j.mode == kSteer || clean);
        // who gets a pass 2: every output object; in clean mode also any other selected
        // object that touches one (its own matte cuts the holes it makes)
        std::vector<char> needPass(K, 0);
        {
            std::ofstream outF(j.dir / "outputs.txt"), names(j.dir / "object_names.txt");
            for (int k = 0; k < K; ++k) names << j.objects[k].slot << "\t" << j.objects[k].text << "\n";
            for (int k = 0; k < K; ++k)
                if (j.objects[k].output) {
                    needPass[k] = 1;
                    outF << k << "\n";
                }
        }
        if (clean)
            for (int o = 0; o < K; ++o) {
                if (needPass[o]) continue;
                for (int i = 0; i < j.count && !needPass[o]; i += std::max(1, j.count / 24)) {
                    Gray so;
                    if (!readPFM(matteFile(j.dir, selKind(o).c_str(), i), so)) continue;
                    so = growBlur(so, std::max(4, so.w / 48), 0.0);
                    for (int k = 0; k < K && !needPass[o]; ++k) {
                        if (!j.objects[k].output) continue;
                        Gray sk;
                        if (!readPFM(matteFile(j.dir, selKind(k).c_str(), i), sk) || sk.px.size() != so.px.size()) continue;
                        for (size_t p = 0; p < sk.px.size(); p += 2)
                            if (sk.px[p] > 0.5f && so.px[p] > 0.5f) {
                                needPass[o] = 1;
                                break;
                            }
                    }
                }
            }
        int nPass = 0;
        for (char v : needPass) nPass += v;
        // a clean pass costs about three Alpha-Gen passes
        const double units = 1 + (twoPass ? nPass * (clean ? 3.0 : 1.0) : 0);
        const double wPass = wAG / units;
        for (int ci = 0; ci < chunks; ++ci)
            if (!runChunk(ci, "ag", 0, -1, wTrack + wPass * ci / chunks, wPass / chunks)) return c.res;
        if (twoPass) {
            std::string info;
            double b0 = wTrack + wPass;
            for (int k = 0; k < K; ++k) {
                if (!needPass[k]) {
                    std::ofstream(j.dir / strf("pass2_%d.txt", k)) << "none";
                    continue;
                }
                // coverage of this object by the plain pass decides whether Alpha-Gen
                // treats it as background (then pass 2 is steered)
                double hit = 0;
                int nfr = 0;
                for (int i = 0; i < std::min(j.count, j.chunk); ++i) {
                    Gray plain, sel;
                    if (!readPFM(matteFile(j.dir, "ag", i), plain) || !readPFM(matteFile(j.dir, selKind(k).c_str(), i), sel))
                        continue;
                    hit += coverageOf(plain, sel);
                    ++nfr;
                }
                double cov = nfr ? hit / nfr : 1.0;
                bool steer = j.mode == kSteer || ((j.mode == kAuto || clean) && cov < kAutoCoverage);
                const char* how = steer ? "steered" : clean ? "clean" : "isolated";
                std::ofstream(j.dir / strf("pass2_%d.txt", k)) << how;
                info += strf("%s%s%s: %s", info.empty() ? "" : "; ", j.objects[k].label().c_str(),
                             j.objects[k].output ? "" : " (occluder)", how);
                const double w = wPass * (clean && !steer ? 3.0 : 1.0);
                for (int ci = 0; ci < chunks; ++ci) {
                    bool ok = clean && !steer ? runClean(ci, k, b0 + w * ci / chunks, w / chunks)
                                              : runChunk(ci, isoKind(k), steer ? 2 : 1, k, b0 + w * ci / chunks, w / chunks);
                    if (!ok) return c.res;
                }
                b0 += w;
            }
            c.res.info = info;
            if (clean && !c.step(0.99, "cutting holes")) return c.res;
            if (clean && K > 1 && !depthPass(c, frames)) return c.res;
            if (clean) {
                const std::vector<char> front = frontOrder(j, K);
                if (!c.step(0.995, "difference matte where a still object is covered")) return c.res;
                temporalFront(j, K, frames, front);
                seeThroughFront(j, K, front);
                for (int k = 0; k < K; ++k)
                    if (j.objects[k].output && pass2Kind(j.dir, k) == "clean")
                        for (int i = 0; i < j.count; ++i) writeHole(j, K, k, i, front);
            }
        }
    }
