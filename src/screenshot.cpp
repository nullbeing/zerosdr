#include "screenshot.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <sys/time.h>

std::string makeScreenshotPath() {
    const char* home = std::getenv("HOME");
    const std::string dir = (home != nullptr && *home != '\0') ? home : "/tmp";

    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    struct tm local {};
    localtime_r(&tv.tv_sec, &local);

    char base[256];
    std::snprintf(base, sizeof(base),
                  "%s/zerosdr_screenshot_%04d%02d%02d_%02d%02d%02d",
                  dir.c_str(), local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);

    std::string path = std::string(base) + ".png";
    std::error_code ec;
    int suffix = 1;
    while (std::filesystem::exists(path, ec)) {
        path = std::string(base) + "_" + std::to_string(suffix++) + ".png";
    }
    return path;
}
