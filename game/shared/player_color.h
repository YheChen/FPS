#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "game/shared/protocol.h"

// One identity colour per player slot, so eight identical grey figures become
// eight people you can tell apart at a glance.
//
// Keyed by player id, never by join order or by a position in whatever list of
// connected players a client happens to hold: the id is already in every
// snapshot, so all clients derive the same colour for the same person with no
// protocol field to add, and someone who drops and reclaims their slot comes
// back the colour they left as.
//
// These are albedos, handed to the lit shader's u_tint. They are not display
// colours: what a player sees is albedo * (ambient + sun) through the ACES
// tonemap, which is close to linear below ~0.5 and compresses hard above it.
// That is why the palette leans dark. Four bright colours would all resolve to
// "pale" once the curve is done with them.
//
// How they were chosen, because "eight colours" is where this gets subtle:
//
//  - Separation from THIS arena. The floor is grey concrete, the walls tan,
//    the pillars steel blue, the centre platform green, the sky blue. Under
//    the sunlit shading path no entry lands closer than dE(CIE76) 18 to any of
//    them, and the closest pair of players is 49 apart. Saturation does most
//    of that work: nothing in the arena is vivid.
//
//  - The practice targets count as scenery too. They are stretched cubes
//    tinted by remaining health, sweeping red at nearly-dead to gold at full,
//    and they are drawn online as well as offline. That band is why slot 2 is
//    a dusty apricot and not the gold it wants to be -- gold sat dE 13 from a
//    full-health target, and a screenshot of eight bots made the pair genuinely
//    hard to call. Slot 0's red still comes within dE 11 of a target that is
//    about to die, which is accepted: targets are static boxes and players are
//    moving figures with limbs, so the silhouettes settle it.
//
//  - Deuteranopia, ~8% of men, is handled by never letting a pair rely on the
//    red-green axis alone. Run the palette through the Vienot 1999 simulation
//    and the closest pair is still dE 25 in sunlight, because every pair whose
//    hues collapse together is separated in lightness or in chroma instead:
//    red is dark and vivid where apricot is light and dusty, forest is dark
//    where lime is bright. Protanopia fares worse (closest pair dE 14) and
//    tritanopia worse again. Eight simultaneously distinguishable colours is
//    past what any palette can promise for every kind of colour vision, which
//    is why the HUD puts a colour chip beside the name instead of asking the
//    colour to carry the identity by itself.
//
//  - Deep shadow flattens all of this. Ambient alone is 0.2, so every one of
//    these lands under luminance 0.3 once tonemapped, and there the closest
//    pair is 24 apart with normal vision but only 11 with deuteranopia, and
//    the arena margin drops to 9. That is the shading, not the palette: no
//    choice of albedo survives being multiplied by 0.2.
//
//  - Nobody is a black silhouette or a white blur: rendered luminance stays
//    inside a band. Being hard to SEE is a fairness problem, not merely an
//    identity one.
namespace game {

inline constexpr std::array<glm::vec3, kMaxPlayers> kPlayerColors{{
    {0.85f, 0.06f, 0.00f},  // 0 red
    {0.00f, 1.00f, 1.00f},  // 1 cyan
    {0.90f, 0.60f, 0.40f},  // 2 apricot
    {0.15f, 0.15f, 1.00f},  // 3 indigo
    {0.40f, 1.00f, 0.00f},  // 4 lime
    {0.70f, 0.11f, 0.46f},  // 5 plum
    {0.00f, 0.35f, 0.16f},  // 6 forest
    {0.00f, 0.33f, 0.70f},  // 7 cobalt
}};

// Environment deaths carry kNoPlayer (255) as the killer, and the kill feed
// calls that "world". Grey rather than kPlayerColors[255 % kMaxPlayers]:
// wrapping would hand the world a living player's colour, which is worse than
// having no colour at all.
inline constexpr glm::vec3 kNoPlayerColor{0.55f, 0.56f, 0.58f};

constexpr glm::vec3 player_color(std::uint8_t id) {
    return id < kMaxPlayers ? kPlayerColors[id] : kNoPlayerColor;
}

}  // namespace game
