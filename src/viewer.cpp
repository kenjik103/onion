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

namespace {

const char* const USAGE =
    "usage:\n"
    "  onion_viewer [--contract <dir>] [--screenshot <path.png>] [--view top|oblique]\n";

// Crown voxels run to hundreds of millions. Display-side decimation only: it is never
// written back, and the factor is shown on screen so a screenshot can state it
// (SPEC.md 9, V2).
constexpr size_t MAX_CROWN_POINTS = 2000000;

double g_scene_mid_z = 0.0;

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

    const Vertices ground = submesh(mesh, 2);
    const Vertices bld = submesh(mesh, 6);
    if (!ground.f.empty()) {
        auto* s = polyscope::registerSurfaceMesh("terrain", ground.v, ground.f);
        s->setSurfaceColor({0.45, 0.40, 0.32});
    }
    if (!bld.f.empty()) {
        auto* s = polyscope::registerSurfaceMesh("buildings", bld.v, bld.f);
        s->setSurfaceColor({0.80, 0.35, 0.30});
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
    char msg[512];
    std::snprintf(msg, sizeof msg,
                  "loaded %s\n  EPSG %d  tile %.0f x %.0f m\n"
                  "  %zu triangles, %zu objects\n"
                  "  crowns %zu, %zu of %zu non-empty voxels shown (1 in %d)\n",
                  dir.c_str(), meta.epsg, meta.bbox[2], meta.bbox[3], mesh.cls.size(),
                  meta.n_objects, crowns.crown_count, crowns.pad.size(),
                  crowns.total_nonempty, crowns.stride);
    append(msg);
    std::printf("%s", msg);
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
