// Viewer (SPEC.md 6.11). Three rules it keeps:
//   1. No scene logic. It reads the scene geometry contract and nothing else -- never
//      the cache, never the scene spec, never the builders.
//   2. It launches the CLI as subprocesses, so a GUI run and a CLI run are identical.
//   3. --screenshot <path> renders one frame headless, through EGL, and exits.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <glm/glm.hpp>

#include <polyscope/point_cloud.h>
#include <polyscope/polyscope.h>
#include <polyscope/surface_mesh.h>

#include "contract_read.h"
#include "las_read.h"

namespace {

const char* const USAGE =
    "usage:\n"
    "  onion_viewer [--contract <dir>] [--screenshot <path.png>]\n"
    "               [--view top|oblique|closeup|profile]\n";

// Crown voxels run to hundreds of millions. Display-side decimation only: it is never
// written back, and the factor is shown on screen so a screenshot can state it
// (SPEC.md 9, V2).
constexpr size_t MAX_CROWN_POINTS = 2000000;
constexpr size_t MAX_LAS_POINTS = 3000000;

// ASPRS classes the scene uses (FORMAT.md section 2).
std::array<double, 3> class_colour(uint8_t cls) {
    switch (cls) {
        case 2: return {0.85, 0.55, 0.18};   // ground
        case 5: return {0.13, 0.80, 0.22};   // high vegetation
        case 6: return {0.95, 0.20, 0.25};   // building
        default: return {0.60, 0.60, 0.60};
    }
}

double g_scene_mid_z = 0.0;
double g_centre_z = 0.0;   // ground height at the tile centre, for the close-up camera

struct Vertices {
    std::vector<std::array<double, 3>> v;
    std::vector<std::array<uint32_t, 3>> f;
};

// Split the contract mesh by class. Faces carry the class, so each layer is the subset
// of faces with that code, with vertices renumbered to just those used.
Vertices submesh(const ContractMesh& m, uint8_t cls) {
    Vertices out;
    std::vector<uint32_t> remap(m.xyz.size() / 3, UINT32_MAX);
    for (size_t i = 0; i < m.cls.size(); ++i) {
        if (m.cls[i] != cls) continue;
        std::array<uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            const uint32_t src = m.indices[3 * i + k];
            if (remap[src] == UINT32_MAX) {
                remap[src] = static_cast<uint32_t>(out.v.size());
                out.v.push_back({m.xyz[3 * src], m.xyz[3 * src + 1], m.xyz[3 * src + 2]});
            }
            tri[k] = remap[src];
        }
        out.f.push_back(tri);
    }
    return out;
}

// --- Run button state -------------------------------------------------------------
struct RunState {
    std::mutex mu;
    std::string log;
    std::atomic<bool> busy{false};
    float lat = 38.20f, lon = -86.40f, extent = 1000.0f;
    int seed = 1;
    char cache[512] = "/tmp/onion_cache";
    char out[512] = "/tmp/onion_out";
};
RunState g_run;
std::string g_cli;
std::string g_contract_dir;
std::atomic<bool> g_reload{false};

std::string cli_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "onion";
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : p.substr(0, slash)) + "/onion";
}

void append(const std::string& s) {
    const std::lock_guard<std::mutex> lk(g_run.mu);
    g_run.log += s;
}

// Runs one subprocess and streams its output into the log. Errors are shown verbatim.
int run_cli(const std::string& cmd) {
    append("$ " + cmd + "\n");
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> p(popen(cmd.c_str(), "r"), pclose);
    if (!p) { append("failed to start\n"); return -1; }
    char line[1024];
    while (std::fgets(line, sizeof line, p.get())) append(line);
    return 0;
}

std::string quote(const std::string& s) { return "'" + s + "'"; }

void do_run() {
    g_run.busy = true;
    char args[1024];
    std::snprintf(args, sizeof args, "--lat %.6f --lon %.6f --extent %.1f --seed %d",
                  g_run.lat, g_run.lon, g_run.extent, g_run.seed);
    const std::string cache = g_run.cache;
    const std::string out = g_run.out;

    // fetch, then generate -- exactly what the CLI does, as subprocesses (rule 2).
    run_cli(quote(g_cli) + " fetch " + args + " --cache " + quote(cache) + " 2>&1");
    run_cli(quote(g_cli) + " generate " + args + " --cache " + quote(cache) + " --out " +
            quote(out) + " 2>&1");
    g_contract_dir = out;
    g_reload = true;
    g_run.busy = false;
}

// --- Scene loading ----------------------------------------------------------------
ContractMeta g_meta;

void load_scene(const std::string& dir) {
    polyscope::removeAllStructures();
    const ContractMeta meta = read_scene_geometry(dir + "/scene_geometry.json");
    g_meta = meta;
    const ContractMesh mesh = read_opaque_ply(dir + "/opaque.ply");
    const CrownPoints crowns =
        read_crown_points(dir + "/crowns.bin", meta.voxel_size_m, MAX_CROWN_POINTS);

    double zlo = 1e30, zhi = -1e30;
    for (size_t i = 2; i < mesh.xyz.size(); i += 3) {
        zlo = std::min<double>(zlo, mesh.xyz[i]);
        zhi = std::max<double>(zhi, mesh.xyz[i]);
    }
    g_scene_mid_z = 0.5 * (zlo + zhi);

    // Ground height at the tile centre. The mid of the whole z range is not it: on a
    // tile with relief the camera ends up under the terrain.
    const double ccx = 0.5 * (meta.bbox[0] + meta.bbox[2]);
    const double ccy = 0.5 * (meta.bbox[1] + meta.bbox[3]);
    double best = 1e30;
    g_centre_z = g_scene_mid_z;
    for (size_t i = 0; i + 2 < mesh.xyz.size(); i += 3) {
        const double dx = mesh.xyz[i] - ccx, dy = mesh.xyz[i + 1] - ccy;
        const double d = dx * dx + dy * dy;
        if (d < best) { best = d; g_centre_z = mesh.xyz[i + 2]; }
    }

    const Vertices ground = submesh(mesh, 2);
    const Vertices bld = submesh(mesh, 6);
    if (!ground.f.empty()) {
        auto* s = polyscope::registerSurfaceMesh("terrain", ground.v, ground.f);
        // Muted, so the class-coloured point cloud reads against it.
        s->setSurfaceColor({0.72, 0.70, 0.66});
    }
    if (!bld.f.empty()) {
        auto* s = polyscope::registerSurfaceMesh("buildings", bld.v, bld.f);
        s->setSurfaceColor({0.58, 0.56, 0.60});
    }
    if (!crowns.xyz.empty()) {
        std::vector<std::array<double, 3>> pts(crowns.pad.size());
        for (size_t i = 0; i < pts.size(); ++i)
            pts[i] = {crowns.xyz[3 * i], crowns.xyz[3 * i + 1], crowns.xyz[3 * i + 2]};
        auto* c = polyscope::registerPointCloud("crowns", pts);
        // Quad impostors rather than spheres: Polyscope warns that spheres do not scale
        // to point counts this large.
        c->setPointRenderMode(polyscope::PointRenderMode::Quad);
        // Half a voxel, so decimated voxel centres still read as a canopy volume.
        c->setPointRadius(0.5 * meta.voxel_size_m, false);
        c->addScalarQuantity("PAD", crowns.pad)->setEnabled(true);
    }
    // The point cloud, if a scan has been written. The viewer reads the LAS and the
    // contract and nothing else (SPEC.md 6.11 rule 1).
    size_t las_shown = 0, las_total = 0;
    int las_stride = 1;
    const std::string las_path = dir + "/points.las";
    if (std::FILE* probe = std::fopen(las_path.c_str(), "rb")) {
        std::fclose(probe);
        const LasPoints lp =
            read_las_points(las_path, meta.origin_x, meta.origin_y, MAX_LAS_POINTS);
        las_total = lp.total;
        las_stride = lp.stride;
        las_shown = lp.cls.size();
        std::vector<std::array<double, 3>> pts(lp.cls.size());
        std::vector<std::array<double, 3>> col(lp.cls.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            pts[i] = {lp.xyz[3 * i], lp.xyz[3 * i + 1], lp.xyz[3 * i + 2]};
            col[i] = class_colour(lp.cls[i]);
        }
        auto* c = polyscope::registerPointCloud("point cloud", pts);
        c->setPointRenderMode(polyscope::PointRenderMode::Quad);
        c->setPointRadius(0.07, false);  // half the 0.14 m sample spacing
        c->addColorQuantity("class", col)->setEnabled(true);
        // The scanned cloud is the subject in V2, so the crowns it was cast through
        // start hidden rather than obscuring it.
        if (polyscope::hasPointCloud("crowns"))
            polyscope::getPointCloud("crowns")->setEnabled(false);
    }

    char msg[768];
    std::snprintf(msg, sizeof msg,
                  "loaded %s\n  EPSG %d  tile %.0f x %.0f m\n"
                  "  %zu triangles, %zu objects\n"
                  "  crowns %zu, %zu of %zu non-empty voxels shown (1 in %d)\n"
                  "  points %zu of %zu shown (1 in %d), display decimation only\n",
                  dir.c_str(), meta.epsg, meta.bbox[2], meta.bbox[3], mesh.cls.size(),
                  meta.n_objects, crowns.crown_count, crowns.pad.size(),
                  crowns.total_nonempty, crowns.stride, las_shown, las_total, las_stride);
    append(msg);
    std::printf("%s", msg);
    // The window then blocks in show(); without this the summary sits in the
    // buffer and is lost if the process is killed or its output is piped.
    std::fflush(stdout);
}

// A camera derived from the tile itself, so screenshots are reproducible and do not
// depend on Polyscope's automatic home view. "top" is near-nadir, for comparing a
// screenshot against the G1 overlays, which are top-down rasters.
void set_camera(const ContractMeta& m, const std::string& mode) {
    const float cx = static_cast<float>(0.5 * (m.bbox[0] + m.bbox[2]));
    const float cy = static_cast<float>(0.5 * (m.bbox[1] + m.bbox[3]));
    const float size = static_cast<float>(std::max(m.bbox[2] - m.bbox[0], m.bbox[3] - m.bbox[1]));
    const float cz = static_cast<float>(g_scene_mid_z);
    const glm::vec3 target(cx, cy, cz);
    // Straight down is ambiguous for the up vector, so "top" is nudged a little south.
    if (mode == "profile") {
        // Near-horizontal, so the canopy reads as a layer above the ground rather than
        // as green dust seen through from above. This is the view that shows vertical
        // structure in a forested tile.
        const float gz = static_cast<float>(g_centre_z);
        polyscope::view::lookAt(glm::vec3(cx - 165.0f, cy - 18.0f, gz + 26.0f),
                                glm::vec3(cx + 60.0f, cy, gz + 14.0f));
        return;
    }
    if (mode == "closeup") {
        // A few tens of metres across, so 7 cm points are actually resolvable and the
        // cloud can be seen sitting on the surfaces rather than dusting them. Anchored
        // on the ground at the tile centre, not the mid of the whole z range.
        const float gz = static_cast<float>(g_centre_z);
        polyscope::view::lookAt(glm::vec3(cx - 70.0f, cy - 70.0f, gz + 58.0f),
                                glm::vec3(cx, cy, gz + 8.0f));
        return;
    }
    const glm::vec3 eye = (mode == "top")
                              ? glm::vec3(cx, cy - 0.02f * size, cz + 1.35f * size)
                              : glm::vec3(cx - 0.75f * size, cy - 0.75f * size, cz + 0.55f * size);
    polyscope::view::lookAt(eye, target);
}

void ui() {
    ImGui::PushItemWidth(140);
    ImGui::InputFloat("latitude", &g_run.lat, 0.0f, 0.0f, "%.5f");
    ImGui::InputFloat("longitude", &g_run.lon, 0.0f, 0.0f, "%.5f");
    ImGui::InputFloat("extent (m)", &g_run.extent, 0.0f, 0.0f, "%.0f");
    ImGui::InputInt("seed", &g_run.seed);
    ImGui::PopItemWidth();
    ImGui::InputText("cache", g_run.cache, sizeof g_run.cache);
    ImGui::InputText("out", g_run.out, sizeof g_run.out);

    if (g_run.busy) {
        ImGui::TextUnformatted("running...");
    } else if (ImGui::Button("Run")) {
        // Detached, so a slow fetch never blocks the window (SPEC.md 6.11 rule 2).
        std::thread(do_run).detach();
    }
    ImGui::Separator();
    const std::lock_guard<std::mutex> lk(g_run.mu);
    ImGui::TextUnformatted(g_run.log.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string shot_path;
        std::string view_mode = "oblique";
        for (int i = 1; i < argc; i += 2) {
            const std::string k = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("flag has no value: " + k + "\n" + USAGE);
            const std::string v = argv[i + 1];
            if (k == "--contract") g_contract_dir = v;
            else if (k == "--screenshot") shot_path = v;
            else if (k == "--view") view_mode = v;
            else throw std::runtime_error("unknown flag: " + k + "\n" + USAGE);
        }
        g_cli = cli_path();

        // EGL, so headless works with no display. Linux only (SPEC.md 5, 6.11 rule 3).
        if (!shot_path.empty()) polyscope::options::allowHeadlessBackends = true;
        polyscope::options::programName = "onion viewer";
        polyscope::options::buildGui = true;
        polyscope::init();

        // The contract stores z as elevation (FORMAT.md section 1). Polyscope defaults
        // to Y-up, which stands the terrain on its edge, so say so explicitly.
        polyscope::view::setUpDir(polyscope::UpDir::ZUp);
        polyscope::view::setFrontDir(polyscope::FrontDir::NegYFront);

        if (!g_contract_dir.empty()) load_scene(g_contract_dir);

        if (!shot_path.empty()) {
            set_camera(g_meta, view_mode);
            // c_str(), because screenshot(std::string) is ambiguous between the
            // ScreenshotOptions and bool overloads; Polyscope ships this one to settle it.
            polyscope::screenshot(shot_path.c_str());
            std::printf("wrote %s\n", shot_path.c_str());
            return 0;
        }

        polyscope::state::userCallback = ui;
        polyscope::show();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
