// How a poster's picture is laid on its mesh. Stock posters read the texture the usual way:
// +u runs to the viewer's right, +v down. The social lobby's central display stores its art
// rotated: +u runs DOWN the panel and +v to the viewer's LEFT (a portrait screen fed from a
// landscape texture). A PosterTurn says which, as each texture axis's share of "right" and
// "down" (-1, 0 or 1). tools/build_poster_table.py works it out from the mesh; the runtime
// paints the frame turned to match and maps touches back the same way.
#pragma once
#include <algorithm>

struct PosterTurn {
    int ur, ud, vr, vd;
};
constexpr PosterTurn POSTER_UPRIGHT{1, 0, 0, 1};
inline bool turned(PosterTurn t) { return !(t.ur == 1 && t.ud == 0 && t.vr == 0 && t.vd == 1); }
inline bool sameTurn(PosterTurn a, PosterTurn b) { return a.ur == b.ur && a.ud == b.ud && a.vr == b.vr && a.vd == b.vd; }

// The picture area as the viewer sees it (width along "right", height along "down", in
// pixels of the W x H frame region), and the frame fitted upright into it: as big as it
// goes, centred.
struct TurnFit {
    float panelW, panelH, scale, offX, offY;
};
inline TurnFit turnFit(PosterTurn t, int W, int H) {
    float panelW = float(t.ur ? W : H), panelH = float(t.ud ? W : H);
    float s = std::min(panelW / W, panelH / H);
    return {panelW, panelH, s, (panelW - W * s) / 2, (panelH - H * s) / 2};
}

// A frame-region pixel (fx along u, fy along v) -> the UI pixel painted there. False
// outside the fitted picture.
inline bool turnToUi(PosterTurn t, int W, int H, float fx, float fy, float& ux, float& uy) {
    if (!turned(t)) {
        ux = fx;
        uy = fy;
        return fx >= 0 && fy >= 0 && fx < W && fy < H;
    }
    float r = t.ur > 0 ? fx : t.ur < 0 ? (W - 1 - fx) : t.vr > 0 ? fy : (H - 1 - fy);
    float d = t.ud > 0 ? fx : t.ud < 0 ? (W - 1 - fx) : t.vd > 0 ? fy : (H - 1 - fy);
    TurnFit f = turnFit(t, W, H);
    ux = (r - f.offX) / f.scale;
    uy = (d - f.offY) / f.scale;
    return ux >= 0 && uy >= 0 && ux < W && uy < H;
}

// A UI pixel -> the frame-region pixel it is painted at.
inline void uiToTurn(PosterTurn t, int W, int H, float ux, float uy, float& fx, float& fy) {
    if (!turned(t)) {
        fx = ux;
        fy = uy;
        return;
    }
    TurnFit f = turnFit(t, W, H);
    float r = f.offX + ux * f.scale, d = f.offY + uy * f.scale;
    fx = t.ur > 0 ? r : t.ur < 0 ? (W - 1 - r) : t.ud > 0 ? d : (W - 1 - d);
    fy = t.vr > 0 ? r : t.vr < 0 ? (H - 1 - r) : t.vd > 0 ? d : (H - 1 - d);
}

// The frame-region rectangle the fitted picture covers (x along u, y along v), inclusive-exclusive.
inline void turnBox(PosterTurn t, int W, int H, int& x0, int& y0, int& x1, int& y1) {
    float ax, ay, bx, by;
    uiToTurn(t, W, H, 0, 0, ax, ay);
    uiToTurn(t, W, H, float(W - 1), float(H - 1), bx, by);
    x0 = int(std::min(ax, bx));
    y0 = int(std::min(ay, by));
    x1 = int(std::max(ax, bx)) + 1;
    y1 = int(std::max(ay, by)) + 1;
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(W, x1); y1 = std::min(H, y1);
}

// Paints the W x H BGRA frame `src` (row pitch `pitch` bytes) into the W x H region `dst`
// (same pitch) turned to `t`: every region pixel inside the fitted picture takes its UI
// pixel (nearest), the rest keeps what is there.
inline void paintTurned(PosterTurn t, int W, int H, const unsigned char* src, unsigned char* dst, int pitch) {
    int x0, y0, x1, y1;
    turnBox(t, W, H, x0, y0, x1, y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float ux, uy;
            if (!turnToUi(t, W, H, float(x), float(y), ux, uy)) continue;
            int sx = std::min(W - 1, std::max(0, int(ux))), sy = std::min(H - 1, std::max(0, int(uy)));
            const unsigned char* s = src + size_t(sy) * pitch + size_t(sx) * 4;
            unsigned char* d = dst + size_t(y) * pitch + size_t(x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
}
