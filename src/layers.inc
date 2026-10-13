// ------------------------------------------------------ layer separation
//
// Alpha-Gen draws beautiful edges but mattes every subject it considers foreground.
// The selection says which one we want. SAM 3.1 also finds the OTHER subjects
// (instances of the "others" prompt plus your other selected objects), and only those
// are treated as someone else:
//   * pass 2 paints them out of the plate, so Alpha-Gen draws the target's own edges,
//     including parts SAM's outline missed;
//   * the final keeps what Alpha-Gen drew that is connected to the target (within a
//     reach), uses an ownership split only right next to other subjects, and cuts
//     holes where other subjects are in front.
// The approach follows Rikin Patel's Comfy Pro Alpha Generator (used with permission):
// paint out the others with an inpainted (never flat) fill, level the fill's residue,
// split ownership only near other subjects, keep what is connected to the target.
// (Included into engine.cpp.)

// Distance (3-4 chamfer, in pixels) from every pixel to the nearest seed.
static std::vector<float> distanceTo(const std::vector<char>& seed, int w, int h) {
    const float INF = 1e9f, a = 1.f, b = 1.41421356f;
    std::vector<float> d((size_t)w * h);
    for (size_t i = 0; i < d.size(); ++i) d[i] = seed[i] ? 0.f : INF;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float& v = d[(size_t)y * w + x];
            if (x > 0) v = std::min(v, d[(size_t)y * w + x - 1] + a);
            if (y > 0) {
                v = std::min(v, d[(size_t)(y - 1) * w + x] + a);
                if (x > 0) v = std::min(v, d[(size_t)(y - 1) * w + x - 1] + b);
                if (x < w - 1) v = std::min(v, d[(size_t)(y - 1) * w + x + 1] + b);
            }
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            float& v = d[(size_t)y * w + x];
            if (x < w - 1) v = std::min(v, d[(size_t)y * w + x + 1] + a);
            if (y < h - 1) {
                v = std::min(v, d[(size_t)(y + 1) * w + x] + a);
                if (x < w - 1) v = std::min(v, d[(size_t)(y + 1) * w + x + 1] + b);
                if (x > 0) v = std::min(v, d[(size_t)(y + 1) * w + x - 1] + b);
            }
        }
    return d;
}

static float remap(float v, double bp, double wp) { return (float)std::clamp(v * (wp - bp) + bp, 0.0, 1.0); }

static Gray blankLike(const Gray& g) {
    Gray o;
    o.w = g.w;
    o.h = g.h;
    o.px.assign(g.px.size(), 0.f);
    return o;
}

// Keep only what is connected to the target: components of (matte > floor) or the
// target that contain target pixels, limited to `reach` pixels from the target.
static void keepConnected(Gray& out, const Gray& tgt, double reach, float floor = 0.02f) {
    const int w = out.w, h = out.h;
    std::vector<char> on((size_t)w * h), seed((size_t)w * h), seen((size_t)w * h, 0);
    for (size_t i = 0; i < on.size(); ++i) {
        seed[i] = tgt.px[i] > 0.5f;
        on[i] = out.px[i] > floor || seed[i];
    }
    std::vector<float> d = distanceTo(seed, w, h);
    std::vector<int> stack;
    for (int s = 0; s < w * h; ++s) {
        if (!seed[s] || seen[s]) continue;
        stack.assign(1, s);
        seen[s] = 1;
        while (!stack.empty()) {
            int p = stack.back();
            stack.pop_back();
            int x = p % w, y = p / w;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if ((dx | dy) == 0 || xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    int q = yy * w + xx;
                    if (on[q] && !seen[q] && d[q] < reach) {
                        seen[q] = 1;
                        stack.push_back(q);
                    }
                }
        }
    }
    for (size_t i = 0; i < out.px.size(); ++i)
        if (!seen[i]) out.px[i] = 0.f;
}

// Soft ownership: ~1 where a pixel is closer to the other subjects than to the target.
static Gray otherOwnership(const Gray& tgt, const Gray& others) {
    const int w = tgt.w, h = tgt.h;
    std::vector<char> t((size_t)w * h), o((size_t)w * h);
    bool any = false;
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = tgt.px[i] > 0.5f;
        o[i] = others.px[i] > 0.5f && !t[i];
        any |= o[i] != 0;
    }
    Gray own = blankLike(tgt);
    if (!any) return own;
    std::vector<float> dT = distanceTo(t, w, h), dO = distanceTo(o, w, h);
    for (size_t i = 0; i < own.px.size(); ++i) {
        float z = std::clamp((dO[i] - dT[i]) / 1.5f, -50.f, 50.f);  // > 0: closer to the target
        own.px[i] = 1.f / (1.f + std::exp(z));
    }
    return own;
}

// Push-pull fill: colours of known pixels pulled up a pyramid and pushed back down
// into the holes. Smooth, structure-free, never a flat colour. rgb is w*h*3 floats.
static void pushPullFill(std::vector<float>& rgb, const std::vector<float>& known, int w, int h) {
    struct Level {
        int w, h;
        std::vector<float> c, a;  // premultiplied colour, weight
    };
    std::vector<Level> lv(1);
    lv[0] = {w, h, std::vector<float>(rgb.size()), known};
    for (size_t i = 0; i < known.size(); ++i)
        for (int ch = 0; ch < 3; ++ch) lv[0].c[i * 3 + ch] = rgb[i * 3 + ch] * known[i];
    while (lv.back().w > 1 || lv.back().h > 1) {
        const Level& f = lv.back();
        Level n{std::max(1, (f.w + 1) / 2), std::max(1, (f.h + 1) / 2), {}, {}};
        n.c.assign((size_t)n.w * n.h * 3, 0.f);
        n.a.assign((size_t)n.w * n.h, 0.f);
        for (int y = 0; y < n.h; ++y)
            for (int x = 0; x < n.w; ++x) {
                float sa = 0, sc[3] = {0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int fx = std::min(2 * x + dx, f.w - 1), fy = std::min(2 * y + dy, f.h - 1);
                        size_t fi = (size_t)fy * f.w + fx;
                        sa += f.a[fi];
                        for (int ch = 0; ch < 3; ++ch) sc[ch] += f.c[fi * 3 + ch];
                    }
                size_t ni = (size_t)y * n.w + x;
                if (sa > 0) {
                    float na = std::min(1.f, sa), k = na / sa;
                    n.a[ni] = na;
                    for (int ch = 0; ch < 3; ++ch) n.c[ni * 3 + ch] = sc[ch] * k;
                }
            }
        lv.push_back(std::move(n));
    }
    for (int l = (int)lv.size() - 2; l >= 0; --l) {
        Level& f = lv[l];
        const Level& n = lv[l + 1];
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                double cx = (x + 0.5) / 2 - 0.5, cy = (y + 0.5) / 2 - 0.5;
                int x0 = std::clamp((int)std::floor(cx), 0, n.w - 1), y0 = std::clamp((int)std::floor(cy), 0, n.h - 1);
                int x1 = std::min(x0 + 1, n.w - 1), y1 = std::min(y0 + 1, n.h - 1);
                float ax = (float)std::clamp(cx - std::floor(cx), 0.0, 1.0);
                float ay = (float)std::clamp(cy - std::floor(cy), 0.0, 1.0);
                auto lerp = [&](const std::vector<float>& v, int stride, int ch) {
                    auto at = [&](int xx, int yy) { return v[((size_t)yy * n.w + xx) * stride + ch]; };
                    return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                };
                size_t i = (size_t)y * f.w + x;
                float miss = 1 - f.a[i];
                if (miss <= 0) continue;
                for (int ch = 0; ch < 3; ++ch) f.c[i * 3 + ch] += miss * lerp(n.c, 3, ch);
                f.a[i] += miss * lerp(n.a, 1, 0);
            }
    }
    for (size_t i = 0; i < known.size(); ++i)
        if (lv[0].a[i] > 1e-6f)
            for (int ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] = lv[0].c[i * 3 + ch] / lv[0].a[i];
}

// Relax the hole towards a harmonic (Laplace) fill so it meets the plate continuously.
static void harmonicFill(std::vector<float>& rgb, const std::vector<float>& known, int w, int h, int iters) {
    std::vector<float> next = rgb;
    for (int it = 0; it < iters; ++it) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                if (known[i] >= 1.f) continue;
                size_t l = (size_t)y * w + std::max(x - 1, 0), r = (size_t)y * w + std::min(x + 1, w - 1);
                size_t u = (size_t)std::max(y - 1, 0) * w + x, d = (size_t)std::min(y + 1, h - 1) * w + x;
                for (int ch = 0; ch < 3; ++ch)
                    next[i * 3 + ch] = 0.25f * (rgb[l * 3 + ch] + rgb[r * 3 + ch] + rgb[u * 3 + ch] + rgb[d * 3 + ch]);
            }
        rgb.swap(next);
    }
}

static std::vector<float> boxDown(const Frame& f, int k, int& w, int& h) {
    w = std::max(1, f.w / k);
    h = std::max(1, f.h / k);
    std::vector<float> out((size_t)w * h * 3, 0.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                float s = 0;
                for (int dy = 0; dy < k; ++dy)
                    for (int dx = 0; dx < k; ++dx)
                        s += f.rgb[((size_t)std::min(y * k + dy, f.h - 1) * f.w + std::min(x * k + dx, f.w - 1)) * 3 + ch];
                out[((size_t)y * w + x) * 3 + ch] = s / (k * k * 255.f);
            }
    return out;
}

// Separable gaussian on a w*h*3 float image (values 0-1).
static void blurRGB(std::vector<float>& img, int w, int h, double sigma) {
    Gray g;
    g.w = w;
    g.h = h;
    g.px.resize((size_t)w * h);
    for (int ch = 0; ch < 3; ++ch) {
        for (size_t i = 0; i < g.px.size(); ++i) g.px[i] = img[i * 3 + ch];
        Gray b = growBlur(g, 0, sigma);
        for (size_t i = 0; i < g.px.size(); ++i) img[i * 3 + ch] = b.px[i];
    }
}

// Where to paint for this object: the other subjects (grown a little), never the target.
static Gray paintMask(const Gray& sel, const Gray& others, int grow) {
    Gray paint = growBlur(others, grow, 0.0);
    for (size_t i = 0; i < paint.px.size(); ++i) paint.px[i] = std::clamp(paint.px[i] * (1.f - sel.px[i]), 0.f, 1.f);
    return paint;
}

Frame isolatedGuide(const Frame& plate, const Gray& selIn, const Gray& othersIn, bool steer) {
    const int W = plate.w, H = plate.h;
    Gray sel = resized(selIn, W, H), others = resized(othersIn, W, H);
    Gray paint = paintMask(sel, others, std::max(2, W / 320));

    // inpaint at 1/4 resolution. The fill is solved from the plate with BOTH the target
    // and the others removed, so it never borrows their colours (a fill that looks like
    // the target makes Alpha-Gen extend it; one that looks like the other subject makes
    // Alpha-Gen matte it as a ghost).
    const int k = 4;
    int w, h;
    std::vector<float> lowres = boxDown(plate, k, w, h);
    Gray paintS = resized(growBlur(paint, 2, 0.0), w, h), selS = resized(growBlur(sel, 2, 0.0), w, h);
    std::vector<float> known(paintS.px.size());
    for (size_t i = 0; i < known.size(); ++i) known[i] = (paintS.px[i] > 0.02f || selS.px[i] > 0.02f) ? 0.f : 1.f;
    std::vector<float> fill = lowres;
    pushPullFill(fill, known, w, h);
    harmonicFill(fill, known, w, h, 150);
    blurRGB(fill, w, h, 1.5);

    // steer (background objects): defocus everything outside the selection, without the
    // selection's own colours bleeding out
    std::vector<float> defocus;
    if (steer) {
        std::vector<float> base = lowres;
        for (size_t i = 0; i < known.size(); ++i)
            if (paintS.px[i] > 0.02f)
                for (int ch = 0; ch < 3; ++ch) base[i * 3 + ch] = fill[i * 3 + ch];
        Gray selG = resized(growBlur(sel, 6, 0.0), w, h), den = blankLike(selG);
        for (size_t i = 0; i < den.px.size(); ++i) den.px[i] = 1.f - std::min(1.f, selG.px[i] * 2.f);
        std::vector<float> num = base;
        for (size_t i = 0; i < den.px.size(); ++i)
            for (int ch = 0; ch < 3; ++ch) num[i * 3 + ch] *= den.px[i];
        blurRGB(num, w, h, 10.0);
        den = growBlur(den, 0, 10.0);
        defocus = base;
        for (size_t i = 0; i < den.px.size(); ++i)
            for (int ch = 0; ch < 3; ++ch)
                defocus[i * 3 + ch] = den.px[i] > 1e-3f ? num[i * 3 + ch] / den.px[i] : base[i * 3 + ch];
    }

    Gray paintSoft = growBlur(paint, 0, 1.0);
    Gray outside;
    if (steer) {
        outside = growBlur(sel, 10, 4.0);
        for (float& v : outside.px) v = 1.f - v;
    }
    auto sampleSmall = [&](const std::vector<float>& img, int x, int y, int ch) {
        double sx = (x + 0.5) / k - 0.5, sy = (y + 0.5) / k - 0.5;
        int x0 = std::clamp((int)std::floor(sx), 0, w - 1), y0 = std::clamp((int)std::floor(sy), 0, h - 1);
        int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        float ax = (float)std::clamp(sx - std::floor(sx), 0.0, 1.0), ay = (float)std::clamp(sy - std::floor(sy), 0.0, 1.0);
        auto at = [&](int xx, int yy) { return img[((size_t)yy * w + xx) * 3 + ch]; };
        return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
    };
    Frame out = plate;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            float hm = paintSoft.px[i] * (1.f - sel.px[i]);
            float om = steer ? outside.px[i] : 0.f;
            if (hm <= 0.f && om <= 0.f) continue;
            for (int ch = 0; ch < 3; ++ch) {
                float v = plate.rgb[i * 3 + ch] / 255.f;
                if (hm > 0) v = v * (1 - hm) + sampleSmall(fill, x, y, ch) * hm;
                if (om > 0) v = v * (1 - om) + sampleSmall(defocus, x, y, ch) * om;
                out.rgb[i * 3 + ch] = (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f);
            }
        }
    return out;
}

double coverageOf(const Gray& matte, const Gray& selIn) {
    Gray sel = resized(selIn, matte.w, matte.h);
    double hit = 0, total = 0;
    for (size_t i = 0; i < matte.px.size(); ++i) {
        hit += matte.px[i] * sel.px[i];
        total += sel.px[i];
    }
    return total > 0 ? hit / total : 1.0;
}

Gray composeFinal(int mode, const Gray* plainIn, const Gray* isolatedIn, const Gray* selIn, const Gray* othersIn,
                  double reach, double blackPt, double whitePt, bool background) {
    Gray out;
    if (mode == kSelectionOnly) return selIn ? *selIn : out;
    if (mode == kAlphaGenOnly) {
        if (!plainIn) return out;
        out = *plainIn;
        for (float& v : out.px) v = remap(v, blackPt, whitePt);
        return out;
    }
    const Gray* edgeSrc = (mode == kFast || !isolatedIn) ? plainIn : isolatedIn;
    if (!edgeSrc || !selIn) return out;
    out = *edgeSrc;
    for (float& v : out.px) v = remap(v, blackPt, whitePt);
    const int W = out.w, H = out.h;
    Gray sel = resized(*selIn, W, H);
    Gray others = othersIn ? resized(*othersIn, W, H) : blankLike(out);
    Gray plain = blankLike(out);
    if (plainIn) {
        plain = resized(*plainIn, W, H);
        for (float& v : plain.px) v = remap(v, blackPt, whitePt);
    }
    const double s = W / 1920.0;  // the pixel defaults were tuned at 1920 wide
    const int nearPx = std::max(4, (int)std::lround(15 * s));

    if (background) {  // behind the others: its holes are their pass-1 alpha
        // Alpha-Gen is a foreground matter: even steered it can miss most of a building. Then
        // SAM's selection is the body (hard edges suit buildings and signs) and Alpha-Gen adds
        // the soft detail it does find (glow, lettering above the outline).
        if (coverageOf(out, sel) < 0.6) {
            Gray body = growBlur(sel, 0, std::max(0.7, 1.0 * s));
            for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = std::max(out.px[i], body.px[i]);
        }
        Gray own = otherOwnership(sel, others);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] *= 1.f - plain.px[i] * own.px[i];
        keepConnected(out, sel, reach * s);
        return out;
    }

    Gray paint = paintMask(sel, others, std::max(2, (int)std::lround(6 * s)));
    bool anyPaint = false;
    for (float v : paint.px) anyPaint |= v > 0.5f;
    if (mode != kFast && anyPaint) {
        // level: Alpha-Gen gives the painted fill a little alpha; take its median off near it
        Gray core = growBlur(paint, -std::max(2, (int)std::lround(15 * s)), 0.0);
        std::vector<float> vals;
        for (size_t i = 0; i < core.px.size(); i += 3)
            if (core.px[i] > 0.5f) vals.push_back(out.px[i]);
        float g = 0.f;
        if (!vals.empty()) {
            std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
            g = std::min(0.9f, vals[vals.size() / 2]);
        }
        Gray nearPaint = growBlur(paint, nearPx, 4.0 * s + 1.0);
        std::vector<char> tseed(out.px.size());
        for (size_t i = 0; i < tseed.size(); ++i) tseed[i] = sel.px[i] > 0.5f;
        std::vector<float> dT = distanceTo(tseed, W, H);
        const float ramp = (float)(6 * s);
        for (size_t i = 0; i < out.px.size(); ++i) {
            float v = out.px[i];
            if (g > 0) v = nearPaint.px[i] * std::clamp((v - g) / (1 - g), 0.f, 1.f) + (1 - nearPaint.px[i]) * v;
            v *= 1.f - paint.px[i] * std::clamp((dT[i] - ramp) / 2.f, 0.f, 1.f);  // painted area beyond the target
            out.px[i] = v;
        }
    }
    // right next to other subjects Alpha-Gen is solid on both sides, so there the edge is
    // the ownership split of the plain pass instead
    if (anyPaint && plainIn) {
        Gray own = otherOwnership(sel, others);
        Gray nearZone = growBlur(paint, nearPx, 5.0 * s + 1.0);
        for (size_t i = 0; i < out.px.size(); ++i) {
            float split = plain.px[i] * (1.f - own.px[i]);
            out.px[i] = nearZone.px[i] * split + (1.f - nearZone.px[i]) * out.px[i];
        }
    }
    keepConnected(out, sel, reach * s);
    return out;
}

// ------------------------------------------------------ clean mode
//
// Pass 2 sees a clean plate: the other objects are rebuilt away by the LTX-2.5
// Clean-Plate IC-LoRA, so Alpha-Gen mattes the object's whole shape, including parts
// that were behind someone. The final then cuts holes with the alpha of whatever is in
// front of it: the other selected objects' own clean-pass mattes (true hair edges),
// or SAM's mask for subjects found by the "others" prompt.

static Frame grayFrame(const Gray& g) {
    Frame f;
    f.w = g.w;
    f.h = g.h;
    f.rgb.resize((size_t)g.w * g.h * 3);
    for (size_t i = 0; i < g.px.size(); ++i)
        f.rgb[i * 3] = f.rgb[i * 3 + 1] = f.rgb[i * 3 + 2] = (uint8_t)std::lround(std::clamp(g.px[i], 0.f, 1.f) * 255.f);
    return f;
}

// hole: what the clean plate rebuilds (1) -- the other subjects, plus the loose strands
// of them that Alpha-Gen's plain pass sees just outside SAM's outline, never the target.
// comp: the soft matte where rebuilt pixels replace the plate.
static void cleanMasks(int W, int H, const Gray& selIn, const Gray& othIn, const Gray* plainIn, Gray& hole, Gray& comp) {
    const double s = W / 1920.0;
    Gray sel = resized(selIn, W, H), oth = resized(othIn, W, H);
    std::vector<char> so(oth.px.size()), ss(sel.px.size());
    for (size_t i = 0; i < so.size(); ++i) {
        so[i] = oth.px[i] > 0.5f;
        ss[i] = sel.px[i] > 0.5f;
    }
    std::vector<float> dO = distanceTo(so, W, H), dS = distanceTo(ss, W, H);
    Gray plain = plainIn ? resized(*plainIn, W, H) : blankLike(sel);
    hole = blankLike(sel);
    const float strandReach = (float)(40 * s), keepOff = (float)(10 * s);
    for (size_t i = 0; i < hole.px.size(); ++i)
        hole.px[i] = so[i] || (plain.px[i] > 0.15f && dO[i] < strandReach && dS[i] > keepOff) ? 1.f : 0.f;
    hole = growBlur(hole, std::max(2, (int)std::lround(8 * s)), 0.0);
    const float protect = (float)(3 * s);
    for (size_t i = 0; i < hole.px.size(); ++i)
        if (dS[i] < protect) hole.px[i] = 0.f;
    comp = growBlur(hole, std::max(1, (int)std::lround(4 * s)), 3.0 * s + 0.5);
    Gray selSoft = growBlur(sel, 0, 1.5);
    for (size_t i = 0; i < comp.px.size(); ++i) comp.px[i] *= 1.f - selSoft.px[i];
}

// Who is in front, per pair of objects: Depth Anything 3 on a few frames of the shot
// ("depth_*": DA3 renders nearness, larger = closer), compared only where the two objects touch, voted over
// those frames. front[a*K+b] = a is in front of b.
static std::vector<char> frontOrder(const Job& j, int K) {
    std::vector<double> votes(K * K, 0.0);
    for (int i = 0; i < j.count; ++i) {
        Gray depth;
        if (!readPFM(matteFile(j.dir, "depth", i), depth)) continue;
        const int W = depth.w, H = depth.h;
        const float touch = (float)std::max(4.0, 25.0 * W / 1920.0);
        std::vector<Gray> sel(K);
        std::vector<std::vector<float>> dist(K);
        for (int k = 0; k < K; ++k) {
            if (!readPFM(matteFile(j.dir, selKind(k).c_str(), i), sel[k])) continue;
            sel[k] = resized(sel[k], W, H);
            std::vector<char> seed(sel[k].px.size());
            for (size_t p = 0; p < seed.size(); ++p) seed[p] = sel[k].px[p] > 0.5f;
            dist[k] = distanceTo(seed, W, H);
        }
        for (int a = 0; a < K; ++a)
            for (int b = a + 1; b < K; ++b) {
                if (dist[a].empty() || dist[b].empty()) continue;
                {  // one seen through the other (SAM gives both the pixels): the one containing it is in front
                    double inter = 0, areaA = 0, areaB = 0;
                    for (size_t p = 0; p < sel[a].px.size(); p += 2) {
                        const bool inA = sel[a].px[p] > 0.5f, inB = sel[b].px[p] > 0.5f;
                        inter += inA && inB;
                        areaA += inA;
                        areaB += inB;
                    }
                    if (inter > 0.5 * std::min(areaA, areaB) && std::min(areaA, areaB) > 0) {
                        votes[(areaA > areaB ? a : b) * K + (areaA > areaB ? b : a)] += 4 * inter;
                        continue;
                    }
                }
                std::vector<float> da, db;
                for (size_t p = 0; p < depth.px.size(); ++p) {
                    if (dist[a][p] > touch || dist[b][p] > touch) continue;  // where they touch
                    const bool inA = sel[a].px[p] > 0.5f, inB = sel[b].px[p] > 0.5f;
                    if (inA && !inB) da.push_back(depth.px[p]);  // overlap (one seen through the other) says nothing
                    else if (inB && !inA) db.push_back(depth.px[p]);
                }
                if (da.size() < 30 || db.size() < 30) continue;
                std::nth_element(da.begin(), da.begin() + da.size() / 2, da.end());
                std::nth_element(db.begin(), db.begin() + db.size() / 2, db.end());
                double w = (double)std::min(da.size(), db.size());
                if (da[da.size() / 2] > db[db.size() / 2]) votes[a * K + b] += w;
                else votes[b * K + a] += w;
            }
    }
    std::vector<char> front(K * K, 0);
    for (int a = 0; a < K; ++a)
        for (int b = 0; b < K; ++b)
            if (a != b) front[a * K + b] = votes[a * K + b] > votes[b * K + a];
    if (getenv("AGSAM_RECUT"))
        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b)
                if (a != b) printf("depth vote %d in front of %d: %.0f\n", a, b, votes[a * K + b]);
    return front;
}

// ---- see-through: an object seen through the front one (SAM gives both of them the
// pixels, e.g. a man behind a sheer cape). The front object's own pass still sees the man
// there and mattes him solid, so its alpha over him is filled smoothly from its alpha all
// around him (a sheet's opacity carries on across a silhouette). Written to agt<f>.
static void seeThroughFront(const Job& j, int K, const std::vector<char>& front) {
    const double s = j.W / 1920.0;
    for (int f = 0; f < K; ++f)
        for (int i = 0; i < j.count; ++i) {
            Gray Af, sf;
            bool hasT = readPFM(matteFile(j.dir, temporalKind(f).c_str(), i), Af);
            if (!hasT && !readPFM(matteFile(j.dir, fillKind(f).c_str(), i), Af) &&
                !readPFM(matteFile(j.dir, cleanKind(f).c_str(), i), Af))
                continue;
            if (pass2Kind(j.dir, f) != "clean" || !readPFM(matteFile(j.dir, selKind(f).c_str(), i), sf)) continue;
            const int W = Af.w, H = Af.h;
            sf = resized(sf, W, H);
            Gray inside = blankLike(Af);  // behind objects seen through f
            bool any = false;
            for (int b = 0; b < K; ++b) {
                Gray sb;
                if (b == f || !front[f * K + b] || !readPFM(matteFile(j.dir, selKind(b).c_str(), i), sb)) continue;
                sb = resized(sb, W, H);
                for (size_t p = 0; p < sb.px.size(); ++p)
                    if (sb.px[p] > 0.5f && sf.px[p] > 0.5f) inside.px[p] = 1.f, any = true;
            }
            if (!any) continue;
            inside = growBlur(inside, std::max(2, (int)std::lround(6 * s)), 0.0);
            // fill at 1/4 resolution from f's alpha outside the seen-through area
            const int k4 = 4;
            const int w4 = (W + k4 - 1) / k4, h4 = (H + k4 - 1) / k4;
            std::vector<float> img((size_t)w4 * h4 * 3, 0.f), known((size_t)w4 * h4, 0.f), cnt((size_t)w4 * h4, 0.f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    size_t p = (size_t)y * W + x, q = (size_t)(y / k4) * w4 + x / k4;
                    if (inside.px[p] > 0.5f) continue;
                    img[q * 3] += Af.px[p];
                    cnt[q] += 1;
                }
            for (size_t q = 0; q < cnt.size(); ++q)
                if (cnt[q] >= k4 * k4 * 0.75f) {
                    known[q] = 1.f;
                    img[q * 3] /= cnt[q];
                    img[q * 3 + 1] = img[q * 3 + 2] = img[q * 3];
                } else {
                    img[q * 3] = img[q * 3 + 1] = img[q * 3 + 2] = 0.f;
                }
            pushPullFill(img, known, w4, h4);
            harmonicFill(img, known, w4, h4, 200);
            Gray soft = growBlur(inside, 0, 2.0 * s + 0.5);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    size_t p = (size_t)y * W + x;
                    if (soft.px[p] <= 0.f) continue;
                    double fx = (x + 0.5) / k4 - 0.5, fy = (y + 0.5) / k4 - 0.5;
                    int x0 = std::clamp((int)std::floor(fx), 0, w4 - 1), y0 = std::clamp((int)std::floor(fy), 0, h4 - 1);
                    int x1 = std::min(x0 + 1, w4 - 1), y1 = std::min(y0 + 1, h4 - 1);
                    float ax = (float)std::clamp(fx - x0, 0.0, 1.0), ay = (float)std::clamp(fy - y0, 0.0, 1.0);
                    auto at = [&](int xx, int yy) { return img[((size_t)yy * w4 + xx) * 3]; };
                    float v = (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                    Af.px[p] = Af.px[p] * (1 - soft.px[p]) + std::clamp(v, 0.f, 1.f) * soft.px[p];
                }
            writePFM(matteFile(j.dir, temporalKind(f).c_str(), i), Af);
        }
}

// ---- temporal difference matte: an object in front, over an object that holds still
//
// The clean plate removes the person behind together with whatever of the front object
// lies on top of them (hair, a sheer cape). When the person behind holds still and the
// camera is locked off, other frames show those same pixels uncovered. With that
// "uncovered" plate U and the front object's colour C nearby:
//     plate = a*C + (1-a)*U   ->   a = (plate-U).(C-U) / |C-U|^2
// The result is folded into the front object's alpha (agt<k>) there, which also cuts the
// hole in the object behind.
std::string temporalKind(int k) { return strf("agt%d", k); }

static void temporalFront(const Job& j, int K, const FrameSource& frames, const std::vector<char>& front) {
    const int W = j.W, H = j.H;
    const size_t N = (size_t)W * H;
    const double s = W / 1920.0;
    const bool debug = getenv("AGSAM_RECUT") != nullptr;
    // reset: every front object starts from its own (completed) clean pass
    for (int f = 0; f < K; ++f)
        for (int i = 0; i < j.count; ++i) fs::remove(matteFile(j.dir, temporalKind(f).c_str(), i));
    for (int f = 0; f < K; ++f)
        for (int b = 0; b < K; ++b) {
            if (f == b || !front[f * K + b] || pass2Kind(j.dir, f) != "clean") continue;
            // 1. frames where f is far from b, sampled over the shot
            const float farPx = (float)(60 * s);
            std::vector<int> freeIdx;
            std::vector<double> cx, cy;
            for (int i = 0; i < j.count; i += std::max(1, j.count / 40)) {
                Gray sf, sb;
                if (!readPFM(matteFile(j.dir, selKind(f).c_str(), i), sf) || !readPFM(matteFile(j.dir, selKind(b).c_str(), i), sb))
                    continue;
                sf = resized(sf, W, H);
                sb = resized(sb, W, H);
                std::vector<char> seed(N);
                for (size_t p = 0; p < N; ++p) seed[p] = sf.px[p] > 0.5f;
                std::vector<float> d = distanceTo(seed, W, H);
                double nearCnt = 0, area = 0, sx = 0, sy = 0;
                for (size_t p = 0; p < N; ++p)
                    if (sb.px[p] > 0.5f) {
                        area += 1;
                        nearCnt += d[p] < farPx;
                        sx += p % W;
                        sy += p / W;
                    }
                if (area > 0 && nearCnt / area < 0.02) {
                    freeIdx.push_back(i);
                    cx.push_back(sx / area);
                    cy.push_back(sy / area);
                }
            }
            if (freeIdx.size() < 3) {
                if (debug) printf("temporal %d over %d: only %d free frames, skipped\n", f, b, (int)freeIdx.size());
                continue;
            }
            // 2. does b hold still? (its centroid over the free frames)
            double mx = 0, my = 0, spread = 0;
            for (size_t q = 0; q < cx.size(); ++q) mx += cx[q] / cx.size(), my += cy[q] / cy.size();
            for (size_t q = 0; q < cx.size(); ++q) spread = std::max(spread, std::hypot(cx[q] - mx, cy[q] - my));
            if (spread > 25 * s) {
                if (debug) printf("temporal %d over %d: moves %.0f px, skipped\n", f, b, spread);
                continue;
            }
            // 3. uncovered plate U = per-pixel median of the free frames, plus where b was
            const size_t M = freeIdx.size();
            std::vector<uint8_t> stack(N * 3 * M);
            Gray bFree = Gray{W, H, std::vector<float>(N, 0.f)};
            for (size_t q = 0; q < M; ++q) {
                const Frame* fr = frames(freeIdx[q]);
                if (!fr || fr->w != W || fr->h != H) return;
                for (size_t p = 0; p < N * 3; ++p) stack[p * M + q] = fr->rgb[p];
                Gray sb;
                if (readPFM(matteFile(j.dir, selKind(b).c_str(), freeIdx[q]), sb)) {
                    sb = resized(sb, W, H);
                    for (size_t p = 0; p < N; ++p) bFree.px[p] += (sb.px[p] > 0.5f ? 1.f : 0.f) / (float)M;
                }
            }
            std::vector<float> U(N * 3), noise(N, 0.f);
            for (size_t p = 0; p < N * 3; ++p) {
                uint8_t* v = &stack[p * M];
                std::nth_element(v, v + M / 2, v + M);
                U[p] = v[M / 2] / 255.f;
            }
            stack.clear();
            stack.shrink_to_fit();
            for (size_t p = 0; p < N; ++p) bFree.px[p] = bFree.px[p] > 0.5f ? 1.f : 0.f;
            bFree = growBlur(bFree, std::max(2, (int)std::lround(4 * s)), 0.0);
            if (debug) printf("temporal %d over %d: %d free frames, still (%.0f px)\n", f, b, (int)M, spread);

            // 4. per frame: a over b's area near f
            for (int i = 0; i < j.count; ++i) {
                const Frame* fr = frames(i);
                Gray sf, Af;
                if (!fr || !readPFM(matteFile(j.dir, selKind(f).c_str(), i), sf)) continue;
                bool hasT = readPFM(matteFile(j.dir, temporalKind(f).c_str(), i), Af);
                if (!hasT && !readPFM(matteFile(j.dir, fillKind(f).c_str(), i), Af) &&
                    !readPFM(matteFile(j.dir, cleanKind(f).c_str(), i), Af))
                    continue;
                sf = resized(sf, W, H);
                Af = resized(Af, W, H);
                std::vector<char> seed(N);
                for (size_t p = 0; p < N; ++p) seed[p] = sf.px[p] > 0.5f;
                std::vector<float> d = distanceTo(seed, W, H);
                // f's colour near each pixel: blurred average of its solid pixels outside b
                std::vector<float> num(N * 3, 0.f);
                Gray den = blankLike(sf);
                for (size_t p = 0; p < N; ++p)
                    if (Af.px[p] > 0.9f && bFree.px[p] < 0.5f) {
                        den.px[p] = 1.f;
                        for (int c = 0; c < 3; ++c) num[p * 3 + c] = fr->rgb[p * 3 + c] / 255.f;
                    }
                const int k4 = 4;
                int w4, h4;
                Frame numF = *fr;
                for (size_t p = 0; p < N * 3; ++p) numF.rgb[p] = (uint8_t)std::lround(num[p] * 255.f);
                std::vector<float> numS = boxDown(numF, k4, w4, h4);
                Gray denS = resized(den, w4, h4);
                blurRGB(numS, w4, h4, 40 * s / k4);
                denS = growBlur(denS, 0, 40 * s / k4);
                bool any = false;
                const float reachPx = (float)(150 * s);
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        size_t p = (size_t)y * W + x;
                        if (bFree.px[p] < 0.5f || d[p] > reachPx) continue;
                        size_t q = (size_t)std::min(h4 - 1, y / k4) * w4 + std::min(w4 - 1, x / k4);
                        if (denS.px[q] < 0.02f) continue;
                        float C[3], dI[3], dC[3], dot = 0, cc = 0, ii = 0;
                        for (int c = 0; c < 3; ++c) {
                            C[c] = numS[q * 3 + c] / denS.px[q];
                            dI[c] = fr->rgb[p * 3 + c] / 255.f - U[p * 3 + c];
                            dC[c] = C[c] - U[p * 3 + c];
                            dot += dI[c] * dC[c];
                            cc += dC[c] * dC[c];
                            ii += dI[c] * dI[c];
                        }
                        if (cc < 0.02f) continue;                 // colours too close to tell
                        if (std::sqrt(ii) < 0.04f) continue;      // no change: grain
                        float a = std::clamp(dot / cc, 0.f, 1.f);
                        a = std::clamp((a - 0.06f) / 0.94f, 0.f, 1.f);
                        if (a > Af.px[p]) Af.px[p] = a, any = true;
                    }
                if (any || hasT) writePFM(matteFile(j.dir, temporalKind(f).c_str(), i), Af);
            }
        }
}

// For one object and frame: hol<k> = alpha of what is in front of it; agx<k> = its
// clean-pass alpha, completed where the others were removed. The Clean-Plate LoRA
// rebuilds the background there (it removes every person), so behind an occluder the
// object's alpha comes from the plain pass instead: plain = front + (1 - front) * object,
// so object = (plain - front) / (1 - front). Through loose hair that is exactly the part
// of the object that shows.
static void writeHole(const Job& j, int K, int k, int i, const std::vector<char>& front) {
    Gray A, sk, plain, oth;
    if (!readPFM(matteFile(j.dir, cleanKind(k).c_str(), i), A) || !readPFM(matteFile(j.dir, selKind(k).c_str(), i), sk))
        return;
    const int W = A.w, H = A.h;
    sk = resized(sk, W, H);
    bool hasPlain = readPFM(matteFile(j.dir, "ag", i), plain);
    if (hasPlain) plain = resized(plain, W, H);
    bool hasOth = readPFM(matteFile(j.dir, othKind(k).c_str(), i), oth);
    if (hasOth) oth = resized(oth, W, H);
    Gray hole = blankLike(A), anySel = blankLike(A);
    for (int o = 0; o < K; ++o) {
        if (o == k) continue;
        Gray so, Ao;
        if (!readPFM(matteFile(j.dir, selKind(o).c_str(), i), so)) continue;
        so = resized(so, W, H);
        for (size_t p = 0; p < so.px.size(); ++p) anySel.px[p] = std::max(anySel.px[p], so.px[p]);
        if (!front[o * K + k]) continue;
        if (pass2Kind(j.dir, o) == "clean" && (readPFM(matteFile(j.dir, temporalKind(o).c_str(), i), Ao) ||
                                                readPFM(matteFile(j.dir, cleanKind(o).c_str(), i), Ao)))
            Ao = resized(Ao, W, H);
        else Ao = so;
        for (size_t p = 0; p < hole.px.size(); ++p) hole.px[p] = std::max(hole.px[p], Ao.px[p]);
    }
    if (hasOth)  // subjects the "others" prompt found (not selected): SAM's mask where they cover it
        for (size_t p = 0; p < hole.px.size(); ++p)
            if (oth.px[p] > 0.5f && anySel.px[p] < 0.5f && sk.px[p] < 0.5f) hole.px[p] = std::max(hole.px[p], oth.px[p]);
    writePFM(matteFile(j.dir, holeKind(k).c_str(), i), hole);

    Gray X = A;  // completed alpha
    {
        Gray T;
        if (readPFM(matteFile(j.dir, temporalKind(k).c_str(), i), T)) X = resized(T, W, H);
    }
    if (hasPlain && hasOth) {
        Gray rebuilt, comp;
        cleanMasks(W, H, sk, oth, &plain, rebuilt, comp);
        std::vector<char> ss(sk.px.size());
        for (size_t p = 0; p < ss.size(); ++p) ss[p] = sk.px[p] > 0.5f;
        std::vector<float> dS = distanceTo(ss, W, H);
        const float nearPx = (float)(80 * W / 1920.0);
        for (size_t p = 0; p < X.px.size(); ++p) {
            float w = comp.px[p], F = hole.px[p];
            if (w <= 0.f || F <= 0.02f || dS[p] > nearPx) continue;  // only behind something
            float behind = std::clamp((plain.px[p] - F) / std::max(1.f - F, 0.05f), 0.f, 1.f);
            X.px[p] = X.px[p] * (1 - w) + behind * w;
        }
    }
    writePFM(matteFile(j.dir, fillKind(k).c_str(), i), X);
}

Gray composeClean(const Gray& clean, const Gray* holeIn, const Gray& selIn, double reach, double blackPt,
                  double whitePt) {
    Gray out = clean;
    const int W = out.w, H = out.h;
    for (float& v : out.px) v = remap(v, blackPt, whitePt);
    if (holeIn) {
        Gray hole = resized(*holeIn, W, H);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] *= 1.f - remap(hole.px[i], blackPt, whitePt);
    }
    keepConnected(out, resized(selIn, W, H), reach * W / 1920.0);
    return out;
}
