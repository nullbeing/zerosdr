#ifndef ACARS_LOG_H
#define ACARS_LOG_H

#include "acars/acars_controller.h"

#include <vector>

namespace acars {

/**
 * Appends one decoded message to the daily JSONL log.
 *
 * Files live in $XDG_DATA_HOME/zerosdr/acars (default
 * ~/.local/share/zerosdr/acars) and are named YYYY-MM-DD.jsonl, one file per
 * local day. Failures are silent: logging must never disturb reception.
 */
void appendMessage(const Message& message);

/**
 * Loads up to max_messages decoded messages from the newest daily logs,
 * oldest first. Malformed lines are skipped.
 */
std::vector<Message> loadRecentMessages(int max_messages);

}  // namespace acars

#endif  // ACARS_LOG_H
