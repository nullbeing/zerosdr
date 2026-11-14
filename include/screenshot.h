#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <string>

// Build a unique screenshot path in the user's home directory:
//   $HOME/zerosdr_screenshot_YYYYMMDD_HHMMSS.png
// A numeric suffix is appended if two shots land in the same second.
std::string makeScreenshotPath();

#endif  // SCREENSHOT_H
