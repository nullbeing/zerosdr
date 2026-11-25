#ifndef ACARS_LABELS_H
#define ACARS_LABELS_H

#include <string>

namespace acars {

/**
 * Best-effort description for an ARINC 618/620 ACARS message label.
 *
 * A number of labels are airline-defined (notably 10 and the 3x/4x ranges),
 * so the returned text is a hint about the usual content rather than an
 * authoritative decode. Returns an empty string for labels outside the table.
 */
std::string labelDescription(const std::string& label);

}  // namespace acars

#endif  // ACARS_LABELS_H
