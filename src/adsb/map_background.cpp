#include "adsb/map_background.h"

#include "app_config.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTileSize = 256;
constexpr const char* kUserAgent = "ZeroSDR/0.3.9 (radar map)";

std::string sanitize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            out.push_back((char)c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "map" : out;
}

bool outside_china(double lat, double lon) {
    return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

double transform_lat(double x, double y) {
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y +
                 0.1 * x * y + 0.2 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) +
            20.0 * std::sin(2.0 * x * kPi)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(y * kPi) +
            40.0 * std::sin(y / 3.0 * kPi)) * 2.0 / 3.0;
    ret += (160.0 * std::sin(y / 12.0 * kPi) +
            320.0 * std::sin(y * kPi / 30.0)) * 2.0 / 3.0;
    return ret;
}

double transform_lon(double x, double y) {
    double ret = 300.0 + x + 2.0 * y + 0.1 * x * x +
                 0.1 * x * y + 0.1 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) +
            20.0 * std::sin(2.0 * x * kPi)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(x * kPi) +
            40.0 * std::sin(x / 3.0 * kPi)) * 2.0 / 3.0;
    ret += (150.0 * std::sin(x / 12.0 * kPi) +
            300.0 * std::sin(x / 30.0 * kPi)) * 2.0 / 3.0;
    return ret;
}

void wgs84_to_gcj02(double* lat, double* lon) {
    if (lat == nullptr || lon == nullptr || outside_china(*lat, *lon)) return;
    const double a = 6378245.0;
    const double ee = 0.00669342162296594323;
    double dlat = transform_lat(*lon - 105.0, *lat - 35.0);
    double dlon = transform_lon(*lon - 105.0, *lat - 35.0);
    const double radlat = *lat / 180.0 * kPi;
    double magic = std::sin(radlat);
    magic = 1.0 - ee * magic * magic;
    const double sqrtmagic = std::sqrt(magic);
    dlat = (dlat * 180.0) /
           ((a * (1.0 - ee)) / (magic * sqrtmagic) * kPi);
    dlon = (dlon * 180.0) / (a / sqrtmagic * std::cos(radlat) * kPi);
    *lat += dlat;
    *lon += dlon;
}

uint16_t blend_to_radar_bg(const uint8_t* rgba, float dim, bool dark) {
    // The Esri dark canvas is almost black; lift the midtones so coastlines,
    // rivers and city texture remain visible on the small panel.
    static const std::array<uint8_t, 256> gamma = [] {
        std::array<uint8_t, 256> table{};
        for (int i = 0; i < 256; ++i) {
            table[i] = (uint8_t)std::lround(
                255.0 * std::pow((double)i / 255.0, 0.78));
        }
        return table;
    }();

    const int br = ((COLOR_RADAR_BG >> 11) & 0x1F) << 3;
    const int bg = ((COLOR_RADAR_BG >> 5) & 0x3F) << 2;
    const int bb = (COLOR_RADAR_BG & 0x1F) << 3;
    const float light = 1.0f - dim;

    int r;
    int g;
    int b;
    if (dark) {
        // Light AMap/OSM tiles: invert luminance, desaturate, and tint the
        // result dark blue-grey. Roads and labels become light on a dark
        // canvas, which is what the radar wants.
        const int luma =
            (rgba[0] * 299 + rgba[1] * 587 + rgba[2] * 114) / 1000;
        const int v = 255 - luma;
        r = (int)(v * 0.50f);
        g = (int)(v * 0.60f);
        b = (int)(v * 0.78f);
    } else {
        r = gamma[rgba[0]];
        g = gamma[rgba[1]];
        b = gamma[rgba[2]];
    }
    r = (int)(r * dim + br * light);
    g = (int)(g * dim + bg * light);
    b = (int)(b * dim + bb * light);
    r = std::max(0, std::min(255, r));
    g = std::max(0, std::min(255, g));
    b = std::max(0, std::min(255, b));
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

bool run_curl(const std::vector<std::string>& args, const std::string& proxy) {
    if (args.empty()) return false;

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    if (!proxy.empty()) {
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
            const std::string entry = *e;
            if (entry.rfind("https_proxy=", 0) == 0 ||
                entry.rfind("http_proxy=", 0) == 0 ||
                entry.rfind("HTTPS_PROXY=", 0) == 0 ||
                entry.rfind("HTTP_PROXY=", 0) == 0) {
                continue;
            }
            env_storage.push_back(entry);
        }
        env_storage.push_back("https_proxy=" + proxy);
        env_storage.push_back("http_proxy=" + proxy);
        env_storage.push_back("HTTPS_PROXY=" + proxy);
        env_storage.push_back("HTTP_PROXY=" + proxy);
        for (auto& entry : env_storage) envp.push_back(entry.data());
        envp.push_back(nullptr);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(),
                                proxy.empty() ? environ : envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

double web_mercator_mpp(double lat, int zoom) {
    const double coslat = std::max(0.05, std::cos(lat * kPi / 180.0));
    return 156543.03392 * coslat / std::ldexp(1.0, zoom);
}

}  // namespace

MapBackground::MapBackground() {
    worker_ = std::thread(&MapBackground::workerLoop, this);
}

MapBackground::~MapBackground() {
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        stop_ = true;
    }
    job_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MapBackground::loadConfig() {
    auto& root = AppConfig::instance().data();
    nlohmann::json map = nlohmann::json::object();
    if (root.contains("map") && root["map"].is_object()) map = root["map"];

    enabled_ = map.value("enabled", true);
    provider_ = map.value("provider", "esri_dark");
    std::transform(provider_.begin(), provider_.end(), provider_.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    const std::string configured_url = map.value("url_template", "");
    const std::string configured_attr = map.value("attribution", "");
    proxy_ = map.value("proxy", "");
    full_screen_ = map.value("full_screen", true);
    patch_size_ = full_screen_ ? 1024 : 512;
    grid_tiles_ = patch_size_ / kTileSize + 1;
    dim_ = (float)map.value("dim", 0.90);
    cache_hours_ = map.value("cache_hours", 168);

    if (dim_ < 0.10f) dim_ = 0.10f;
    if (dim_ > 1.0f) dim_ = 1.0f;
    if (cache_hours_ < 0) cache_hours_ = 0;

    gcj02_ = false;
    attribution_ = "ESRI";
    provider_key_ = sanitize(provider_);
    provider_label_ = "MAP";

    if (provider_ == "esri_dark" || provider_ == "esri") {
        // No key required. Dark grey canvas with coastlines/rivers and no
        // text labels; labels are a separate Esri reference layer.
        url_template_ = "https://services.arcgisonline.com/ArcGIS/rest/"
                        "services/Canvas/World_Dark_Gray_Base/MapServer/"
                        "tile/{z}/{y}/{x}";
        attribution_ = "ESRI";
        provider_key_ = "esri_dark";
        provider_label_ = "ESRI DARK";
    } else if (provider_ == "esri_imagery") {
        url_template_ = "https://server.arcgisonline.com/ArcGIS/rest/"
                        "services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
        attribution_ = "ESRI";
        provider_key_ = "esri_imagery";
        provider_label_ = "ESRI SAT";
    } else if (provider_ == "carto_dark_nolabels" || provider_ == "carto" ||
               provider_ == "carto_dark") {
        // CARTO now watermarks tiles unless an api_key is supplied. Kept for
        // custom templates; for a keyless default use esri_dark instead.
        url_template_ = "https://a.basemaps.cartocdn.com/dark_nolabels/"
                        "{z}/{x}/{y}.png";
        attribution_ = "OSM/CARTO";
        provider_key_ = "carto_dark_nolabels";
        provider_label_ = "CARTO";
    } else if (provider_ == "carto_dark_all") {
        url_template_ = "https://a.basemaps.cartocdn.com/dark_all/"
                        "{z}/{x}/{y}.png";
        attribution_ = "OSM/CARTO";
        provider_key_ = "carto_dark_all";
        provider_label_ = "CARTO";
    } else if (provider_ == "osm") {
        url_template_ = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
        attribution_ = "OSM";
        provider_key_ = "osm";
        provider_label_ = "OSM";
    } else if (provider_ == "amap" || provider_ == "gaode") {
        // Unofficial web tile endpoint. GCJ-02 is handled below.
        url_template_ = "https://webrd01.is.autonavi.com/appmaptile"
                        "?lang=zh_cn&size=1&scale=1&style=8"
                        "&x={x}&y={y}&z={z}";
        attribution_ = "AMAP";
        provider_key_ = "amap";
        provider_label_ = "AMAP";
        gcj02_ = true;
    } else if (provider_ == "custom" || provider_ == "custom_url") {
        url_template_ = configured_url;
        attribution_ = configured_attr.empty() ? "MAP" : configured_attr;
        provider_key_ = "custom_" +
                        std::to_string(std::hash<std::string>{}(url_template_) &
                                       0xffffu);
        provider_label_ = "CUSTOM";
    } else {
        std::cerr << "MapBackground: unknown provider '" << provider_
                  << "', map disabled" << std::endl;
        enabled_ = false;
        url_template_.clear();
    }

    // Light tile sets (AMap and OSM) are inverted and tinted by default so
    // the radar keeps its dark scope look.
    const bool auto_dark = (provider_ == "amap" || provider_ == "osm");
    dark_ = map.value("dark", auto_dark);
    if (dark_ && provider_ == "amap") {
        provider_label_ = "AMAP DARK";
    } else if (dark_ && provider_ == "osm") {
        provider_label_ = "OSM DARK";
    }

    if (enabled_ && url_template_.empty()) {
        std::cerr << "MapBackground: empty URL template, map disabled"
                  << std::endl;
        enabled_ = false;
    }
    if (!configured_attr.empty() && provider_ != "custom" &&
        provider_ != "custom_url") {
        attribution_ = configured_attr;
    }

    std::cout << "MapBackground: " << (enabled_ ? "enabled" : "disabled")
              << ", provider=" << provider_
              << ", dim=" << dim_ << ", cache=" << cache_hours_ << "h"
              << (proxy_.empty() ? "" : ", proxy configured") << std::endl;
}

void MapBackground::reload() {
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        ++generation_;
        job_valid_ = false;
        failed_key_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        image_.reset();
    }
    loadConfig();
    job_cv_.notify_all();
}

int MapBackground::zoomForRange(double lat, float range_km) const {
    if (range_km < 1.0f) range_km = 1.0f;
    // Circle-only: the patch half-span covers the radar radius with 10%
    // margin. Full-screen: it must also cover the screen corners at the
    // normal 1.0x scope size (about 3.2x the radar radius), so fetch a
    // coarser patch that is still large enough at every zoom level.
    const double coverage_km = full_screen_ ? 3.5 * range_km : 1.1 * range_km;
    const double wanted_mpp =
        (2.0 * coverage_km * 1000.0) / (double)patch_size_;
    const double coslat = std::max(0.05, std::cos(lat * kPi / 180.0));
    int z = (int)std::floor(
        std::log2(156543.03392 * coslat / wanted_mpp));
    z = std::max(2, std::min(18, z));
    return z;
}

void MapBackground::request(double lat, double lon, float range_km) {
    if (!enabled_ || range_km <= 0.0f) return;
    if (lat < -85.0 || lat > 85.0 || lon < -180.0 || lon > 180.0) return;

    const int zoom = zoomForRange(lat, range_km);
    char key[256];
    snprintf(key, sizeof(key), "%s_%.3f_%.3f_z%d", provider_key_.c_str(),
             lat, lon, zoom);
    Job job{lat, lon, zoom, key};

    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        if (image_ != nullptr && image_->key == job.key) return;
    }

    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        if (job_valid_ && pending_job_.key == job.key) return;
        const auto now = std::chrono::steady_clock::now();
        if (job.key == failed_key_ && now < failed_until_) return;
        job.generation = generation_;
        pending_job_ = job;
        job_valid_ = true;
    }
    job_cv_.notify_one();
}

bool MapBackground::hasImage() const {
    std::lock_guard<std::mutex> lock(image_mutex_);
    return image_ != nullptr && !image_->pixels.empty();
}

void MapBackground::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(job_mutex_);
            job_cv_.wait(lock, [this] { return stop_ || job_valid_; });
            if (stop_) break;
            job = pending_job_;
            job_valid_ = false;
        }

        std::cout << "MapBackground: loading " << job.key << std::endl;
        Image image;
        bool ok = loadCache(job, image, false);
        if (!ok) {
            ok = buildJob(job, image);
            if (!ok) {
                // Network or provider failure: an expired cache is still
                // better than a blank radar.
                ok = loadCache(job, image, true);
            }
        }

        bool current = false;
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            current = (job.generation == generation_);
        }
        if (!current) {
            std::cout << "MapBackground: discarding stale " << job.key
                      << std::endl;
            continue;
        }

        if (ok) {
            auto shared = std::make_shared<Image>(std::move(image));
            {
                std::lock_guard<std::mutex> lock(image_mutex_);
                image_ = shared;
            }
            {
                std::lock_guard<std::mutex> lock(job_mutex_);
                failed_key_.clear();
            }
            std::cout << "MapBackground: ready " << job.key << " ("
                      << shared->width << "x" << shared->height << ")"
                      << std::endl;
        } else {
            {
                std::lock_guard<std::mutex> lock(job_mutex_);
                failed_key_ = job.key;
                failed_until_ =
                    std::chrono::steady_clock::now() + std::chrono::seconds(60);
            }
            std::cerr << "MapBackground: failed " << job.key
                      << " (retry in 60s)" << std::endl;
        }
    }
}

std::string MapBackground::cacheDir() const {
    const char* home = getenv("HOME");
    return std::string(home != nullptr ? home : "/home/pi") +
           "/.cache/zerosdr/radar_map";
}

std::string MapBackground::cachePath(const Job& job) const {
    char name[512];
    snprintf(name, sizeof(name), "%s/map_%s_%.3f_%.3f_z%d_p%d.png",
             cacheDir().c_str(), provider_key_.c_str(), job.lat, job.lon,
             job.zoom, patch_size_);
    return name;
}

bool MapBackground::loadCache(const Job& job, Image& out,
                              bool allow_stale) const {
    const std::string path = cachePath(job);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!allow_stale && cache_hours_ > 0) {
        const time_t now = time(nullptr);
        if (st.st_mtime + (time_t)cache_hours_ * 3600 < now) return false;
    }

    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (data == nullptr) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    if (w != patch_size_ || h != patch_size_) {
        stbi_image_free(data);
        return false;
    }

    Image image;
    image.width = w;
    image.height = h;
    image.zoom = job.zoom;
    image.lat = job.lat;
    image.lon = job.lon;
    image.m_per_px = web_mercator_mpp(job.lat, job.zoom);
    image.key = job.key;
    image.pixels.resize((size_t)w * h);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        image.pixels[i] = blend_to_radar_bg(&data[i * 4], dim_, dark_);
    }
    stbi_image_free(data);
    out = std::move(image);
    return true;
}

bool MapBackground::downloadTiles(const Job& job,
                                  const std::string& tmpdir,
                                  std::vector<std::string>& paths) const {
    double lat = job.lat;
    double lon = job.lon;
    if (gcj02_) wgs84_to_gcj02(&lat, &lon);

    const double n = std::ldexp(1.0, job.zoom);
    const double tx = (lon + 180.0) / 360.0 * n;
    const double lat_rad = lat * kPi / 180.0;
    const double ty =
        (1.0 - std::asinh(std::tan(lat_rad)) / kPi) / 2.0 * n;
    const int center_x = (int)std::floor(tx);
    const int center_y = (int)std::floor(ty);
    const int margin = (grid_tiles_ - 1) / 2;
    const int grid_x = center_x - margin;
    const int grid_y = center_y - margin;

    auto wrap_x = [n](int x) {
        int size = (int)n;
        int r = x % size;
        if (r < 0) r += size;
        return r;
    };
    auto clamp_y = [n](int y) {
        return std::max(0, std::min((int)n - 1, y));
    };

    std::vector<std::string> urls;
    paths.clear();
    for (int j = 0; j < grid_tiles_; ++j) {
        for (int i = 0; i < grid_tiles_; ++i) {
            const int x = wrap_x(grid_x + i);
            const int y = clamp_y(grid_y + j);
            char file[512];
            snprintf(file, sizeof(file), "%s/tile_%d_%d.png", tmpdir.c_str(),
                     i, j);
            paths.emplace_back(file);
            urls.emplace_back(tileUrl(job, x, y));
        }
    }

    auto build_args = [&](bool parallel, const std::vector<size_t>& indices) {
        std::vector<std::string> args = {
            "curl", "-fsS", "--connect-timeout", "8", "--max-time", "20",
            "-A", kUserAgent};
        if (parallel) {
            args.push_back("-Z");
            args.push_back("--parallel-max");
            args.push_back("2");
        }
        for (size_t index : indices) {
            args.push_back("-o");
            args.push_back(paths[index]);
            args.push_back(urls[index]);
        }
        return args;
    };

    auto remove_tiles = [&]() {
        std::error_code ec;
        for (const auto& path : paths) std::filesystem::remove(path, ec);
    };

    auto missing_tiles = [&]() {
        std::vector<size_t> missing;
        for (size_t i = 0; i < paths.size(); ++i) {
            struct stat st;
            if (stat(paths[i].c_str(), &st) != 0 || st.st_size == 0) {
                missing.push_back(i);
            }
        }
        return missing;
    };

    remove_tiles();
    std::vector<size_t> all(paths.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;

    // The proxy/carrier may limit concurrent connections, so use only two.
    run_curl(build_args(true, all), proxy_);

    // Retry whatever did not arrive sequentially. This also covers curl
    // builds without -Z and transient proxy connection timeouts.
    const std::vector<size_t> missing = missing_tiles();
    if (!missing.empty()) {
        run_curl(build_args(false, missing), proxy_);
    }

    return missing_tiles().empty();
}

std::string MapBackground::tileUrl(const Job& job, int x, int y) const {
    std::string url = url_template_;
    auto replace_all = [](std::string& text, const std::string& from,
                          const std::string& to) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(url, "{z}", std::to_string(job.zoom));
    replace_all(url, "{x}", std::to_string(x));
    replace_all(url, "{y}", std::to_string(y));
    return url;
}

bool MapBackground::buildJob(const Job& job, Image& out) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir(), ec);
    if (ec) {
        std::cerr << "MapBackground: cannot create " << cacheDir() << ": "
                  << ec.message() << std::endl;
        return false;
    }

    char tmp_name[256];
    snprintf(tmp_name, sizeof(tmp_name), "%s/tmp_%ld_%d", cacheDir().c_str(),
             (long)getpid(), job.zoom);
    const std::string tmpdir = tmp_name;
    std::filesystem::remove_all(tmpdir, ec);
    std::filesystem::create_directories(tmpdir, ec);
    if (ec) return false;

    std::vector<std::string> paths;
    if (!downloadTiles(job, tmpdir, paths)) {
        std::filesystem::remove_all(tmpdir, ec);
        return false;
    }

    const int grid_size = grid_tiles_ * kTileSize;
    std::vector<unsigned char> grid((size_t)grid_size * grid_size * 4, 0);
    bool tiles_ok = true;
    for (int j = 0; j < grid_tiles_ && tiles_ok; ++j) {
        for (int i = 0; i < grid_tiles_ && tiles_ok; ++i) {
            int w = 0, h = 0, comp = 0;
            unsigned char* tile =
                stbi_load(paths[j * grid_tiles_ + i].c_str(), &w, &h, &comp, 4);
            if (tile == nullptr || w != kTileSize || h != kTileSize) {
                if (tile != nullptr) stbi_image_free(tile);
                tiles_ok = false;
                break;
            }
            const int dst_x = i * kTileSize;
            const int dst_y = j * kTileSize;
            for (int y = 0; y < kTileSize; ++y) {
                memcpy(&grid[(((size_t)dst_y + y) * grid_size + dst_x) * 4],
                       &tile[(size_t)y * w * 4], (size_t)w * 4);
            }
            stbi_image_free(tile);
        }
    }
    std::filesystem::remove_all(tmpdir, ec);
    if (!tiles_ok) return false;

    // Crop a patch centred on the observer's pixel position.
    double lat = job.lat;
    double lon = job.lon;
    if (gcj02_) wgs84_to_gcj02(&lat, &lon);
    const double n = std::ldexp(1.0, job.zoom);
    const double tx = (lon + 180.0) / 360.0 * n;
    const double ty = (1.0 - std::asinh(std::tan(lat * kPi / 180.0)) / kPi) /
                      2.0 * n;
    const int center_x = (int)std::floor(tx);
    const int center_y = (int)std::floor(ty);
    const int margin = (grid_tiles_ - 1) / 2;
    const double px = tx * kTileSize - (double)(center_x - margin) * kTileSize;
    const double py = ty * kTileSize - (double)(center_y - margin) * kTileSize;
    int crop_x = (int)std::lround(px) - patch_size_ / 2;
    int crop_y = (int)std::lround(py) - patch_size_ / 2;
    crop_x = std::max(0, std::min(grid_size - patch_size_, crop_x));
    crop_y = std::max(0, std::min(grid_size - patch_size_, crop_y));

    std::vector<unsigned char> patch((size_t)patch_size_ * patch_size_ * 4);
    for (int y = 0; y < patch_size_; ++y) {
        memcpy(&patch[(size_t)y * patch_size_ * 4],
               &grid[(((size_t)crop_y + y) * grid_size + crop_x) * 4],
               (size_t)patch_size_ * 4);
    }

    // Write the undimmed crop so the cache can be re-tinted later.
    const std::string final_path = cachePath(job);
    const std::string tmp_path = final_path + ".tmp";
    if (stbi_write_png(tmp_path.c_str(), patch_size_, patch_size_, 4,
                       patch.data(), patch_size_ * 4)) {
        std::filesystem::rename(tmp_path, final_path, ec);
        if (ec) std::filesystem::remove(tmp_path, ec);
    }

    out.width = patch_size_;
    out.height = patch_size_;
    out.zoom = job.zoom;
    out.lat = job.lat;
    out.lon = job.lon;
    out.m_per_px = web_mercator_mpp(job.lat, job.zoom);
    out.key = job.key;
    out.pixels.resize((size_t)patch_size_ * patch_size_);
    for (size_t i = 0; i < out.pixels.size(); ++i) {
        out.pixels[i] = blend_to_radar_bg(&patch[i * 4], dim_, dark_);
    }
    return true;
}

void MapBackground::draw(Framebuffer& fb, int cx, int cy, int radius,
                         float max_range_km, float rotation_deg) const {
    if (radius <= 0 || max_range_km <= 0.0f) return;

    std::shared_ptr<const Image> image;
    {
        std::lock_guard<std::mutex> lock(image_mutex_);
        image = image_;
    }
    if (image == nullptr || image->pixels.empty() || image->m_per_px <= 0.0) {
        return;
    }

    const double screen_m_per_px = (double)max_range_km * 1000.0 / radius;
    const int half_w = image->width / 2;
    const int half_h = image->height / 2;

    // The patch must cover the whole circle, or the whole screen when
    // full-screen mode is on.
    const double cover_km = (image->width / 2.0) * image->m_per_px / 1000.0;
    double needed_km = max_range_km;
    if (full_screen_) {
        const int max_dx = std::max(cx, fb.getWidth() - 1 - cx);
        const int max_dy = std::max(cy, fb.getHeight() - 1 - cy);
        const double corner_px =
            std::sqrt((double)max_dx * max_dx + (double)max_dy * max_dy);
        needed_km = corner_px * screen_m_per_px / 1000.0;
    }
    if (cover_km + 0.001 < needed_km) return;

    const double factor = screen_m_per_px / image->m_per_px;
    const double rad = rotation_deg * kPi / 180.0;
    const float cosr = (float)std::cos(rad);
    const float sinr = (float)std::sin(rad);

    const int y0 = full_screen_ ? 0 : std::max(0, cy - radius);
    const int y1 = full_screen_ ? fb.getHeight() - 1
                                : std::min(fb.getHeight() - 1, cy + radius);
    const int x0 = full_screen_ ? 0 : std::max(0, cx - radius);
    const int x1 = full_screen_ ? fb.getWidth() - 1
                                : std::min(fb.getWidth() - 1, cx + radius);
    const int r2 = radius * radius;

    for (int y = y0; y <= y1; ++y) {
        const int dy = y - cy;
        const int dy2 = dy * dy;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - cx;
            if (!full_screen_ && dx * dx + dy2 > r2) continue;

            // Screen -> world: heading-up tilts the map clockwise by the
            // compass heading before sampling the (north-up) patch.
            const float rx = dx * cosr - dy * sinr;
            const float ry = dx * sinr + dy * cosr;
            const int mx = half_w + (int)std::lround(rx * factor);
            const int my = half_h + (int)std::lround(ry * factor);
            if (mx < 0 || mx >= image->width || my < 0 ||
                my >= image->height) {
                continue;
            }
            fb.drawPixel(x, y, image->pixels[(size_t)my * image->width + mx]);
        }
    }
}
