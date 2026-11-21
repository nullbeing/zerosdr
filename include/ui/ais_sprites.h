#ifndef AIS_SPRITES_H
#define AIS_SPRITES_H

#include "ais/ais_store.h"
#include "framebuffer.h"

#include <cstdint>

namespace ais_ui {

enum class PlotTargetKind {
    Cargo,
    Tanker,
    Passenger,
    Generic,
    ClassB,
    Aton,
    Base,
};

/**
 * Draw a small top-down 8-bit pixel ship at a plot position. `heading` is
 * degrees true, 0 = north/up; the sprite is rotated with nearest-neighbour
 * sampling, so every destination pixel stays filled.
 */
void drawPlotTarget(Framebuffer& fb, int cx, int cy, float heading,
                    PlotTargetKind kind, uint16_t bright, uint16_t dim,
                    uint16_t highlight);

/**
 * Draw a chunky 8-bit style side view of a vessel. The sprite is built on a
 * 48x36 palette canvas and scaled 2x, giving 2x2 display pixels inside the
 * 96x72 dossier panel.
 */
void drawDossierShip(Framebuffer& fb, int x, int y, int w, int h,
                     const ais::VesselState& vessel,
                     uint16_t bright, uint16_t dim, uint16_t highlight);

}  // namespace ais_ui

#endif  // AIS_SPRITES_H
