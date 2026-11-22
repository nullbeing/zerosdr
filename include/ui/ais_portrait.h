#ifndef AIS_PORTRAIT_H
#define AIS_PORTRAIT_H

#include "ais/ais_store.h"
#include "framebuffer.h"

#include <cstdint>

namespace ais_ui {

/**
 * Large, instrument-style AIS ship portrait for the dossier view.
 *
 * 200x170 left pane: a circular porthole with a sea/sky gradient, a compass
 * bezel and a detailed side-view ship (cargo, tanker, passenger, fishing,
 * sailing, tug, Class B, AtoN or base station). This is the AIS counterpart
 * of the ADSB attitude ball, not a pixel sprite.
 */
void drawDossierPortrait(Framebuffer& fb, const ais::VesselState* vessel,
                         uint16_t accent, uint16_t dim,
                         uint16_t highlight, uint16_t amber);

}  // namespace ais_ui

#endif  // AIS_PORTRAIT_H
