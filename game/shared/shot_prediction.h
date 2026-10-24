#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "game/shared/protocol.h"
#include "game/shared/weapon.h"

// Client-side shot prediction (M57). Lives in game/shared for the reason
// prediction.h does: the client is one main.cpp with no test target, and the
// rules here are the kind that are wrong in ways a screenshot cannot show.
//
// THE PROBLEM. Online, the client never ran the weapon tick: it sent the Fire
// bit and waited for the server's FireEvent echo before drawing a tracer,
// playing the bang, kicking the viewmodel or flashing the muzzle. At 80 ms RTT
// every trigger pull showed nothing for 80 ms, then everything at once. The
// offline practice range did all of it on the same tick as the trigger, from
// the same shared update_loadout -- so the fix is to run that tick online too
// and reconcile against the server, exactly as movement already is.
//
// WHAT IS PREDICTED: the cosmetics of your own shot -- tracer, wall sparks,
// bang, kick, flash -- and the magazine, reload and raise timers the HUD and
// viewmodel draw from. WHAT IS NOT: whether it HIT. Blood, the hitmarker, the
// hit sound and damage numbers stay server-confirmed. A predicted hit that the
// server then denies is worse feedback than a one-round-trip delay on the
// confirm, because the first teaches you the game lies.
//
// HOW IT STAYS HONEST. Both directions carry the input sequence (protocol 13):
// a FireEvent says which command the server fired on, and a WeaponStatus says
// which command its loadout is current to. The first revision of this paired
// by order and reconciled by heuristic, and a design review found the shape
// of the failure: every echo after the first shot the two sides disagreed
// about was paired with the wrong prediction, so one disagreement became a
// silent shot followed by a double one. With sequences on both, pairing is a
// lookup and reconciliation is the same snap-and-replay movement uses.
namespace game {

struct PredictedShot {
    std::uint32_t sequence = 0;  // input sequence of the command that fired it
    std::uint32_t tick = 0;      // client tick it was predicted on, for expiry
    std::uint8_t slot = 0;
};

// Pairs shots the client predicted with the server's echoes of them.
class ShotLedger {
public:
    // How many sequences apart a prediction and an echo may be and still be
    // the same trigger pull. Not zero, because the two sides can legitimately
    // fire one command apart: a server starved of input re-runs the last
    // command, which advances ITS cooldown by a tick the client never ran,
    // and an automatic weapon then fires one command early. Without the
    // slack, that one-tick drift draws every shot of the burst twice. Small,
    // because a tolerance as wide as the shot interval would pair a shot
    // with its neighbour.
    static constexpr std::uint32_t kPairingTolerance = 3;

    // Predictions are recorded in the order they are made, which is
    // ascending sequence order: the client sends one command per tick and
    // fires at most once per command.
    void predict(const PredictedShot& shot) { pending_.push_back(shot); }

    struct Confirmation {
        // The prediction this echo answers, or nullopt if we never predicted
        // it -- the caller then renders the echo in full, so a shot is never
        // silently lost to a misprediction in that direction.
        std::optional<PredictedShot> shot;
        // Predictions this echo proved the server will never answer: it has
        // moved past them. The count is the symptom a misprediction bug
        // would show, so the caller logs it.
        std::size_t refused = 0;
    };

    // The server fired one of our shots on command `sequence`.
    //
    // An exact match wins. Otherwise the oldest prediction within
    // kPairingTolerance does. Anything older than the tolerance is refused:
    // the server consumes commands in order and the echoes arrive in order
    // (reliable, ordered channel), so no later echo can carry a sequence
    // that would reach back to it.
    Confirmation confirm(std::uint32_t sequence);

    // Drops predictions older than `max_age_ticks` that no echo has answered
    // or refused, which is only the LAST shots before the player stops
    // firing: any earlier one is resolved by the echo after it. Generous --
    // seconds -- because an entry that outlives its echo is harmless (it
    // can only pair within the tolerance), while one discarded ahead of a
    // slow echo gets that shot drawn twice.
    std::size_t expire(std::uint32_t now_tick, std::uint32_t max_age_ticks);

    std::size_t pending() const { return pending_.size(); }
    void clear() { pending_.clear(); }

private:
    std::deque<PredictedShot> pending_;
};

// What the weapon tick reads from one command, kept so the predicted loadout
// can be rebuilt from a server status by replaying it.
struct WeaponInput {
    std::uint32_t sequence = 0;
    std::uint8_t weapon_slot = 0;
    bool fire = false;
    bool reload = false;
    bool aim = false;  // wants_ads() as evaluated on that tick, sprint included
};

// The weapon inputs sent and not yet known to be included in a status --
// every command sent in game, including the ones the client did not tick
// itself because it believed it was dead. The server is the one that decides
// whether a command reaches the weapon, and a status's sequence is its answer
// for everything up to it; the client's own belief is a round trip stale.
class WeaponInputHistory {
public:
    // Eight and a half seconds at 60 Hz: far past any round trip, so the
    // inputs a status needs replayed are always still here. Capped because a
    // status is only sent when something happens (a shot, a reload, a
    // switch, a grenade) and a player who does none of those for a minute
    // would otherwise grow this without bound.
    static constexpr std::size_t kCapacity = 512;

    void record(const WeaponInput& input) {
        inputs_.push_back(input);
        if (inputs_.size() > kCapacity) {
            inputs_.pop_front();
        }
    }
    // Forgets every input the server has confirmed running. Safe because
    // statuses arrive in the order they were sent and each one's sequence is
    // at least the previous one's, so nothing older will be needed again.
    void acknowledge(std::uint32_t sequence) {
        while (!inputs_.empty() && inputs_.front().sequence <= sequence) {
            inputs_.pop_front();
        }
    }
    const std::deque<WeaponInput>& inputs() const { return inputs_; }
    std::size_t size() const { return inputs_.size(); }
    void clear() { inputs_.clear(); }

private:
    std::deque<WeaponInput> inputs_;
};

// The server's half: a WeaponStatus describing `loadout` as of `sequence`,
// the newest input whose weapon tick it includes. The server sends exactly
// this, so reconcile_loadout's snap below is its inverse by construction.
// Grenades are left to the caller; they are not part of the loadout.
WeaponStatusMsg describe_loadout(const Loadout& loadout, const Arsenal& arsenal,
                                 std::uint32_t sequence);

// Rebuilds the predicted loadout from a server status: snap to the server's
// state machine as of status.input_sequence, then replay every recorded
// input after it through the same update_loadout. Exactly the movement
// reconcile, for the weapon. Returns how many inputs were replayed.
//
// Whatever the client predicted and the server did not -- a shot it refused,
// a reload it started that the client had not -- is corrected by the snap,
// and everything the client did since is kept by the replay, so the HUD's
// count is the server's count minus the shots still in flight.
//
// The sights are the one exception: ads_fraction is restored to the value it
// had before the snap. It drives the FOV, so taking the server's would pop
// the zoom whenever the two sides disagreed about sprinting for a tick, and
// it feeds nothing else in the tick, so keeping the client's changes no
// other outcome. A replay that ends on a different weapon than the live one
// keeps the replay's value instead, because a switch drops the sights.
std::size_t reconcile_loadout(Loadout& loadout, const Arsenal& arsenal,
                              const WeaponStatusMsg& status, const WeaponInputHistory& history);

}  // namespace game
