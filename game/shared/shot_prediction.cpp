#include "game/shared/shot_prediction.h"

#include <algorithm>

#include "game/shared/player_movement.h"

namespace game {

ShotLedger::Confirmation ShotLedger::confirm(std::uint32_t sequence) {
    Confirmation result;
    const auto exact = std::find_if(pending_.begin(), pending_.end(),
                                    [&](const PredictedShot& p) { return p.sequence == sequence; });
    if (exact != pending_.end()) {
        // Everything before an exact match is older than a shot the server
        // has now fired, and a later echo can only be later still.
        result.refused = static_cast<std::size_t>(exact - pending_.begin());
        result.shot = *exact;
        pending_.erase(pending_.begin(), exact + 1);
        return result;
    }
    // Written as `front + tolerance < sequence` rather than a subtraction,
    // which would wrap for a prediction newer than the echo.
    while (!pending_.empty() && pending_.front().sequence + kPairingTolerance < sequence) {
        pending_.pop_front();
        ++result.refused;
    }
    if (!pending_.empty() && pending_.front().sequence <= sequence + kPairingTolerance) {
        result.shot = pending_.front();
        pending_.pop_front();
    }
    return result;
}

std::size_t ShotLedger::expire(std::uint32_t now_tick, std::uint32_t max_age_ticks) {
    std::size_t expired = 0;
    // Guarded against a prediction stamped later than `now`, which the tick
    // counter cannot produce but a wrapped or reset counter could: unsigned
    // subtraction would then read as an enormous age and empty the ledger.
    while (!pending_.empty() && now_tick >= pending_.front().tick &&
           now_tick - pending_.front().tick > max_age_ticks) {
        pending_.pop_front();
        ++expired;
    }
    return expired;
}

WeaponStatusMsg describe_loadout(const Loadout& loadout, const Arsenal& arsenal,
                                 std::uint32_t sequence) {
    const WeaponState& raised = loadout.weapons[loadout.slot];
    WeaponStatusMsg status;
    status.ammo = static_cast<std::uint8_t>(raised.ammo);
    status.reloading = raised.reloading();
    status.slot = loadout.slot;
    status.magazine = static_cast<std::uint8_t>(arsenal.at(loadout.slot).magazine_size);
    status.switching = loadout.switch_remaining_seconds > 0.0f;
    status.input_sequence = sequence;
    status.switch_remaining_seconds = loadout.switch_remaining_seconds;
    for (std::size_t i = 0; i < kMaxWeapons; ++i) {
        const WeaponState& w = loadout.weapons[i];
        status.weapons[i] = {static_cast<std::uint8_t>(w.ammo), w.cooldown_seconds,
                             w.reload_remaining_seconds, w.trigger_was_held};
    }
    return status;
}

std::size_t reconcile_loadout(Loadout& loadout, const Arsenal& arsenal,
                              const WeaponStatusMsg& status, const WeaponInputHistory& history) {
    const std::uint8_t live_slot = loadout.slot;
    const float live_ads = loadout.ads_fraction;

    loadout.slot = arsenal.clamp_slot(status.slot);
    loadout.switch_remaining_seconds = status.switch_remaining_seconds;
    for (std::size_t i = 0; i < kMaxWeapons; ++i) {
        const WeaponSlotStatus& s = status.weapons[i];
        loadout.weapons[i] =
            WeaponState{s.ammo, s.cooldown_seconds, s.reload_remaining_seconds, s.trigger_was_held};
    }

    std::size_t replayed = 0;
    for (const WeaponInput& input : history.inputs()) {
        if (input.sequence <= status.input_sequence) {
            continue;  // already inside the status
        }
        update_loadout(loadout, arsenal, input.weapon_slot, input.fire, input.reload, kTickSeconds,
                       input.aim);
        ++replayed;
    }

    if (loadout.slot == live_slot) {
        loadout.ads_fraction = live_ads;
    }
    return replayed;
}

}  // namespace game
