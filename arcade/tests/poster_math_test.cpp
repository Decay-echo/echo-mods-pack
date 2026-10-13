// Lobby-poster geometry checks (no game needed): for every poster in posters.h, a fingertip
// and a bullet ray at known points of the picture face must land on the right screen pixel,
// through the same code the runtime uses (posters.cpp is compiled into this test).
#include "../native/runtime/posters.cpp"
#include <cstdio>

namespace stream {
void setPosterActive(bool) {}
void setPosterHqActive(bool) {}
bool posterHqTextureReady() { return true; }
void setSidesActive(bool) {}
bool sidesTextureReady() { return true; }
void setTurnedActive(bool, PosterTurn) {}
bool posterTextureReady() { return true; }
}

using namespace posters;

static int failures = 0;
#define CHECK(x, ...) do { if (!(x)) { std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #x); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } } while (0)

// A point of the face at mesh UV (u, v): position and front normal, in the poster's space.
static bool facePoint(const Mesh& mesh, float u, float v, Vec& at, Vec& normal) {
    for (auto& t : mesh.tris) {
        float x0 = t.uv[0][0], y0 = t.uv[0][1], x1 = t.uv[1][0] - x0, y1 = t.uv[1][1] - y0, x2 = t.uv[2][0] - x0, y2 = t.uv[2][1] - y0;
        float det = x1 * y2 - x2 * y1;
        if (std::fabs(det) < 1e-12f) continue;
        float b = ((u - x0) * y2 - x2 * (v - y0)) / det, c = (x1 * (v - y0) - (u - x0) * y1) / det;
        if (b < -1e-4f || c < -1e-4f || b + c > 1 + 1e-4f) continue;
        at = t.a * (1 - b - c) + t.b * b + t.c * c;
        normal = t.n;
        return true;
    }
    return false;
}

// The turn maths (poster_turn.h): a UI pixel painted and read back lands where it was.
static void checkTurns() {
    const PosterTurn turns[] = {POSTER_UPRIGHT, {0, 1, -1, 0}, {0, -1, 1, 0}, {-1, 0, 0, -1}};
    const int W = int(POSTER_FRAME_W), H = int(POSTER_FRAME_H);
    for (auto& t : turns) {
        TurnFit f = turnFit(t, W, H);
        for (int px = 0; px < W; px += 97)
            for (int py = 0; py < H; py += 53) {
                float fx, fy, ux, uy;
                uiToTurn(t, W, H, px + .5f, py + .5f, fx, fy);
                bool in = turnToUi(t, W, H, fx, fy, ux, uy);
                float tol = 1 / f.scale + .01f;
                CHECK(in && std::fabs(ux - (px + .5f)) <= tol && std::fabs(uy - (py + .5f)) <= tol && fx >= 0 && fy >= 0 && fx < W && fy < H,
                      "turn (%d,%d,%d,%d): pixel %d,%d -> %.1f,%.1f -> %.1f,%.1f", t.ur, t.ud, t.vr, t.vd, px, py, fx, fy, ux, uy);
            }
    }
    // The central display's turn: u down, v left. The picture stands in the middle of the long
    // axis, full width across the short one.
    int x0, y0, x1, y1;
    turnBox({0, 1, -1, 0}, W, H, x0, y0, x1, y1);
    CHECK(x0 == 351 && x1 == 673 && y0 == 0 && y1 == H, "turned box %d,%d-%d,%d", x0, y0, x1, y1);
    float ux, uy;
    CHECK(!turnToUi({0, 1, -1, 0}, W, H, 100, 100, ux, uy), "outside the picture (letterbox) reads as no pixel");
    // Painting: a frame with a mark at its top-left corner lands at the panel's top-left: along
    // v's far end (left of the viewer = high v... the viewer's left is +v) and u's low end.
    // Two 5x5 marks near the UI's top-left (green) and bottom-right (red) land where uiToTurn
    // says (the picture is scaled by 0.56, so a mark covers a few region pixels).
    std::vector<unsigned char> src(size_t(W) * H * 4, 0), dst(size_t(W) * H * 4, 0);
    for (int dy = 0; dy < 5; dy++)
        for (int dx = 0; dx < 5; dx++) {
            src[((16 + dy) * W + 16 + dx) * 4 + 1] = 200;
            src[((H - 21 + dy) * W + W - 21 + dx) * 4 + 2] = 200;
        }
    paintTurned({0, 1, -1, 0}, W, H, src.data(), dst.data(), W * 4);
    float ax, ay, bx, by;
    uiToTurn({0, 1, -1, 0}, W, H, 18.5f, 18.5f, ax, ay);
    uiToTurn({0, 1, -1, 0}, W, H, W - 18.5f, H - 18.5f, bx, by);
    CHECK(dst[(size_t(int(ay)) * W + int(ax)) * 4 + 1] == 200, "UI top-left mark painted at %.0f,%.0f", ax, ay);
    CHECK(dst[(size_t(int(by)) * W + int(bx)) * 4 + 2] == 200, "UI bottom-right mark painted at %.0f,%.0f", bx, by);
    // Top-left of the UI goes to the low end of u (the panel's top) and the high end of v
    // (the viewer's left); bottom-right the other way round.
    CHECK(ax < 370 && ay > H - 30 && bx > 650 && by < 30, "turned marks at %.0f,%.0f and %.0f,%.0f", ax, ay, bx, by);
}

int main() {
    checkTurns();
    buildGeometry();
    CHECK(!meshes.empty() && table.size() == sizeof(POSTERS) / sizeof(POSTERS[0]), "%zu meshes, %zu posters", meshes.size(), table.size());
    int checked = 0;
    // Frame pixels to probe, and the mesh UV they sit at (inverse of toPixel).
    const int probes[][2] = {{512, 287}, {40, 40}, {980, 530}, {100, 500}};
    for (auto& info : POSTERS) {
        if (!hasFace(info.mesh)) {  // dock only: never touched or hit
            Dock dock;
            dock.mesh = info.mesh;
            dock.faces = {Xform{}};
            docks = {dock};
            int x, y;
            float metres;
            CHECK(!contact(Vec{}, false, x, y) && !rayHit(Vec{}, Vec{0, 0, 1}, x, y, metres), "poster %016llx without a face was hit", info.actor);
            continue;
        }
        Dock dock;
        dock.mesh = info.mesh;
        dock.level = Xform{{info.rot[0], info.rot[1], info.rot[2], info.rot[3]}, {info.pos[0], info.pos[1], info.pos[2]}, info.scale[0]};
        dock.space = Xform{{0, 0, 0.3826834f, 0.9238795f}, {5, -2, 30}, 1.5f};  // any gamespace placement must work
        // Plus a same-model poster 100 m up (off every test ray): it takes touches and shots too,
        // but must not steal them.
        dock.faces = {dock.level, dock.level};
        dock.faces[1].t = dock.faces[1].t + Vec{0, 100, 0};
        // And a second dock (several posters stay docked at once), in another place 300 m away.
        Dock other = dock;
        other.space.t = other.space.t + Vec{300, 0, 0};
        docks = {dock, other};
        const int tolerance = turned(turnOf(info.mesh)) ? 3 : 1;  // a turned picture is scaled down: coarser pixels
        for (auto& px : probes) {
            float fx, fy;  // where that UI pixel is painted in the frame region, for this mesh's turn
            uiToTurn(turnOf(info.mesh), int(POSTER_FRAME_W), int(POSTER_FRAME_H), px[0] + .5f, px[1] + .5f, fx, fy);
            float u = (fx + POSTER_X0) / POSTER_TEX_W, v = (fy + POSTER_Y0) / (2.f * POSTER_TEX_H);
            Vec local, n;
            if (!facePoint(meshes[info.mesh], u, v, local, n)) { CHECK(false, "poster %016llx has no face at pixel %d,%d", info.actor, px[0], px[1]); continue; }
            Vec world = dock.space.apply(dock.level.apply(local));
            Vec worldNormal = dock.space.rotate(dock.level.rotate(n));
            int x = -1, y = -1;
            // Fingertip 5 mm in front: touching, at that pixel. 20 cm in front: not touching.
            bool on = contact(world + worldNormal * .005f, false, x, y);
            CHECK(on && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance, "poster %016llx touch %d,%d -> %d %d,%d", info.actor, px[0], px[1], int(on), x, y);
            CHECK(!contact(world + worldNormal * .2f, false, x, y), "poster %016llx touched from 20 cm", info.actor);
            // A shot from 6 m in front (slightly angled) lands on the same pixel.
            Vec from = world + worldNormal * 6.f + Vec{.3f, .2f, 0};
            Vec dir = world - from;
            dir = dir * (1 / length(dir));
            float metres = 0;
            bool hit = rayHit(from, dir, x, y, metres);
            CHECK(hit && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance && std::fabs(metres - length(world - from)) < .02f,
                  "poster %016llx shot %d,%d -> %d %d,%d (%.2f m)", info.actor, px[0], px[1], int(hit), x, y, metres);
            // Shooting away from the poster misses.
            CHECK(!rayHit(from, dir * -1.f, x, y, metres), "poster %016llx hit by a shot fired away from it", info.actor);
            // The same spot on the other poster lands on the same pixel.
            Vec world2 = dock.space.apply(dock.faces[1].apply(local));
            Vec normal2 = dock.space.rotate(dock.faces[1].rotate(n));
            on = contact(world2 + normal2 * .005f, false, x, y);
            CHECK(on && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance, "poster %016llx second face touch %d,%d -> %d %d,%d", info.actor, px[0], px[1], int(on), x, y);
            Vec from2 = world2 + normal2 * 6.f;
            Vec dir2 = world2 - from2;
            dir2 = dir2 * (1 / length(dir2));
            hit = rayHit(from2, dir2, x, y, metres);
            CHECK(hit && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance && std::fabs(metres - 6.f) < .02f,
                  "poster %016llx second face shot %d,%d -> %d %d,%d (%.2f m)", info.actor, px[0], px[1], int(hit), x, y, metres);
            // The same spot on the second dock's poster lands on the same pixel too.
            Vec world3 = other.space.apply(other.level.apply(local));
            Vec normal3 = other.space.rotate(other.level.rotate(n));
            on = contact(world3 + normal3 * .005f, false, x, y);
            CHECK(on && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance, "poster %016llx second dock touch %d,%d -> %d %d,%d", info.actor, px[0], px[1], int(on), x, y);
            Vec from3 = world3 + normal3 * 6.f;
            Vec dir3 = world3 - from3;
            dir3 = dir3 * (1 / length(dir3));
            hit = rayHit(from3, dir3, x, y, metres);
            CHECK(hit && std::abs(x - px[0]) <= tolerance && std::abs(y - px[1]) <= tolerance && std::fabs(metres - 6.f) < .02f,
                  "poster %016llx second dock shot %d,%d -> %d %d,%d (%.2f m)", info.actor, px[0], px[1], int(hit), x, y, metres);
            checked++;
        }
    }
    std::printf(failures ? "poster_math_test: %d failure(s)\n" : "poster_math_test: all checks passed (%d points on the posters)\n", failures ? failures : checked);
    return failures ? 1 : 0;
}
