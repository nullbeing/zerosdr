#ifndef MAP_BACKGROUND_H
#define MAP_BACKGROUND_H

#include "framebuffer.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * MapBackground - optional raster map under the radar scope.
 *
 * One 512x512 or 1024x1024 patch is fetched as a grid of 256px map tiles,
 * cropped around the observer and cached in ~/.cache/zerosdr/radar_map/.
 * By default the 1024 patch fills the whole display; setting
 * map.full_screen=false uses the smaller 512 patch clipped to the scope.
 * The patch is scaled/rotated while drawing, so radar zoom and heading-up
 * need no image rotation pass. Downloads happen on a worker thread; the radar
 * keeps using the previous patch (or the plain dark background) until a new
 * one is ready.
 *
 * The default provider is Esri "World Dark Gray Base": a dark basemap with
 * no text labels, which keeps the green sweep and white aircraft readable.
 * Carto/OSM/AMap and a custom URL template are also selectable. Provider and
 * proxy are configurable in the "map" section of config.json.
 */
class MapBackground {
public:
    MapBackground();
    ~MapBackground();

    MapBackground(const MapBackground&) = delete;
    MapBackground& operator=(const MapBackground&) = delete;

    /** Read the "map" section from AppConfig. Call before the first request. */
    void loadConfig();

    /**
     * Ask for a patch centred on lat/lon covering at least range_km.
     * Safe to call every frame: duplicate requests are ignored and failures
     * back off for a minute.
     */
    void request(double lat, double lon, float range_km);

    bool hasImage() const;
    bool enabled() const { return enabled_; }
    const char* attribution() const { return attribution_.c_str(); }
    const char* providerLabel() const { return provider_label_.c_str(); }

    /** Re-read the map section and drop the current patch (provider switch). */
    void reload();

    /**
     * Draw the cached patch inside the radar circle.
     * @param max_range_km Range represented by the outer ring
     * @param rotation_deg Compass rotation currently used by the scope
     */
    void draw(Framebuffer& fb, int cx, int cy, int radius,
              float max_range_km, float rotation_deg) const;

    /**
     * Draw the cached patch into a rectangle (used by the APRS detail map).
     * The rectangle centre is the patch centre and max_range_km is the
     * half-range along the shorter side.
     */
    void drawRect(Framebuffer& fb, int x, int y, int w, int h,
                  float max_range_km, float rotation_deg,
                  double offset_east_m = 0.0,
                  double offset_north_m = 0.0) const;

private:
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint16_t> pixels;  // RGB565, already dimmed
        int zoom = 0;
        double lat = 0.0;
        double lon = 0.0;
        double m_per_px = 0.0;         // Web-Mercator metres per pixel
        std::string key;
    };

    struct Job {
        double lat = 0.0;
        double lon = 0.0;
        int zoom = 0;
        std::string key;
        unsigned int generation = 0;
    };

    void workerLoop();
    bool loadCache(const Job& job, Image& out, bool allow_stale) const;
    bool buildJob(const Job& job, Image& out);
    bool downloadTiles(const Job& job, const std::string& tmpdir,
                       std::vector<std::string>& paths) const;
    void saveCache(const Job& job, const Image& image) const;

    std::string cacheDir() const;
    std::string cachePath(const Job& job) const;
    std::string tileUrl(const Job& job, int x, int y) const;
    int zoomForRange(double lat, float range_km) const;

    mutable std::mutex image_mutex_;
    std::shared_ptr<const Image> image_;

    mutable std::mutex job_mutex_;
    std::condition_variable job_cv_;
    Job pending_job_;
    bool job_valid_ = false;
    bool stop_ = false;
    std::string failed_key_;
    std::chrono::steady_clock::time_point failed_until_{};
    std::thread worker_;

    // Configuration (read from AppConfig before the worker starts doing work)
    bool enabled_ = true;
    std::string provider_ = "esri_dark";
    std::string provider_key_ = "esri_dark";
    std::string url_template_;
    std::string proxy_;
    std::string attribution_ = "ESRI";
    std::string provider_label_ = "ESRI DARK";
    unsigned int generation_ = 0;    // guarded by job_mutex_
    bool gcj02_ = false;             // AMap tiles use GCJ-02
    bool dark_ = false;              // invert/desaturate a light map
    bool full_screen_ = true;        // map fills the whole display
    int patch_size_ = 1024;          // 512 when circle-only
    int grid_tiles_ = 5;             // 3 for 512, 5 for 1024
    float dim_ = 0.90f;
    int cache_hours_ = 168;
};

#endif  // MAP_BACKGROUND_H
