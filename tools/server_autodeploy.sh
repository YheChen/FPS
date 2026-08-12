#!/usr/bin/env bash
# Redeploys the dedicated server when main moves, or when it stops serving.
#
# WHY PULL: the alternative is CI reaching in over SSH, which means storing a
# Tailscale auth key AND an SSH key into a machine inside a home LAN with a
# third party. This direction hands out nothing: the host asks GitHub whether
# anything changed, and nothing inbound is required or opened.
#
# WHY IT EXISTS: CI publishes the client on every push to main; nothing
# published the server. On 2026-08-09 the live host was found five milestones
# behind -- including a merged fix for a denial of service it was still
# vulnerable to. Nobody decided that; it is just what happens when shipping
# depends on someone remembering.
#
# WHAT IS DEPLOYED IS TRACKED, NOT INFERRED. The first version of this script
# asked "does HEAD equal origin/main?" and treated yes as "nothing to do".
# That is not the same question. HEAD is where the checkout points; it says
# nothing about what is RUNNING. Both outages on 2026-08-09 were that gap:
#
#   1. A `git pull` by hand moved HEAD to main without building anything.
#      Every run afterwards saw HEAD == origin/main and did nothing, while the
#      container kept serving the old build. Silently, indefinitely.
#   2. A `docker compose down` left the stack stopped. HEAD still equalled
#      origin/main, so the script still did nothing -- and the thing whose job
#      is to keep the server running watched it stay down.
#
# So the two questions are now asked separately, against evidence:
#
#   * WHAT IS DEPLOYED is a stamp file, written only after a deploy has come
#     up healthy. Nothing else writes it, so it cannot be moved by a stray
#     `git pull`.
#   * WHETHER IT IS SERVING is asked of docker, every run. An unhealthy or
#     absent container is redeployed regardless of any SHA.
#
# THE PROTOCOL GUARD is the part that is not obvious. Client and server ship
# together and there is no in-protocol compatibility, so deploying a server
# whose kProtocolVersion has moved past the PUBLISHED client turns every
# connection into ServerReject(VersionMismatch) -- the game down, from a green
# build. So:
#
#   * protocol unchanged  -> deploy. This is almost every commit.
#   * protocol changed    -> deploy ONLY if the PUBLISHED client speaks the new
#                            version. Otherwise refuse and say so.
#
# "What the published client speaks" is read from the `client-live` git tag,
# which deploy-web moves after it has verified the deploy. It was originally
# read from https://<client>/version.json over HTTP; a tag rides the git fetch
# this script already does, needs no credential, and nothing can interpose on
# it.
#
# Failing closed on that check is the whole point: a server that is a few
# commits stale is a nuisance, and one that no client can join is an outage.
#
# Runs as an ordinary user in the `docker` group. No sudo, anywhere.
#
# Usage:  server_autodeploy.sh [--repo DIR] [--dry-run] [--force]
# Exit:   0 nothing to do or deployed, 1 failed, 2 declined (protocol guard,
#         or backing off after repeated failed recoveries).
set -uo pipefail

REPO="${FPS_REPO:-$HOME/GitHub/FPS}"
LIVE_CLIENT_TAG="${FPS_CLIENT_TAG:-client-live}"
# Deploy state lives outside the clone on purpose: inside it, it would either
# be a tracked file this script commits to (no) or an untracked one that trips
# the dirty-worktree refusal below.
STATE_DIR="${FPS_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/fps}"
STAMP="$STATE_DIR/deployed-commit"
RECOVERY="$STATE_DIR/recovery-attempts"

# After this many consecutive failed recovery attempts, back off to one try an
# hour. A server that crashes on startup would otherwise be rebuilt every time
# the timer fires, forever, which burns the host and buries the real error in
# a wall of identical journal entries.
MAX_RECOVERY_BURST=5
RECOVERY_BACKOFF_SECONDS=3600

DRY_RUN=0
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        # Skips the protocol guard only. Never skips the health check.
        --force) FORCE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

say() { echo "[autodeploy] $*"; }
die() { echo "[autodeploy] ERROR: $*" >&2; exit 1; }

cd "$REPO" 2>/dev/null || die "no repo at $REPO"

compose() {
    docker compose -f deploy/compose.yaml --env-file deploy/.env "$@"
}

short() { git rev-parse --short "$1" 2>/dev/null || echo "$1"; }

protocol_version_at() {
    # Reads from a git object rather than the worktree, so this works for a
    # commit that is not checked out.
    git show "$1:game/shared/protocol.h" 2>/dev/null |
        grep -oE 'kProtocolVersion = [0-9]+' | grep -oE '[0-9]+'
}

# --- what is actually deployed ----------------------------------------------
# Empty when this host has never recorded a successful deploy. Deliberately
# NOT defaulted to HEAD: guessing is what this file exists to stop.
deployed_commit() {
    [ -r "$STAMP" ] || return 0
    head -n 1 "$STAMP" | tr -d '[:space:]'
}

record_deployed() {
    mkdir -p "$STATE_DIR" || die "cannot create $STATE_DIR"
    printf '%s\n' "$1" > "$STAMP" || die "cannot write $STAMP"
}

# --- is it actually serving? -------------------------------------------------
# Both services, because `docker compose down` takes both and a running server
# behind a stopped proxy is unreachable from the internet -- which is the only
# place players are.
#
# A service with no healthcheck ("none") counts as healthy if it is running:
# caddy has none, and demanding one it does not define would fail forever.
services_healthy() {
    local service id status health
    for service in server caddy; do
        id=$(compose ps -q "$service" 2>/dev/null | head -n 1)
        if [ -z "$id" ]; then
            say "liveness: no container for '$service'"
            return 1
        fi
        status=$(docker inspect -f '{{.State.Status}}' "$id" 2>/dev/null)
        health=$(docker inspect \
            -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' \
            "$id" 2>/dev/null)
        if [ "$status" != "running" ]; then
            say "liveness: '$service' is ${status:-unknown}"
            return 1
        fi
        if [ "$health" != "healthy" ] && [ "$health" != "none" ]; then
            say "liveness: '$service' is running but $health"
            return 1
        fi
    done
    return 0
}

# --- recovery backoff --------------------------------------------------------
# "<attempts> <unix-time-of-last>". Parsed defensively: a truncated or
# hand-edited file must not crash the one script whose job is bringing the
# server back, so anything unreadable counts as "no attempts yet".
recovery_attempts() {
    local n
    n=$(cut -d' ' -f1 "$RECOVERY" 2>/dev/null)
    case "$n" in ''|*[!0-9]*) echo 0 ;; *) echo "$n" ;; esac
}
recovery_last() {
    local t
    t=$(cut -d' ' -f2 "$RECOVERY" 2>/dev/null)
    case "$t" in ''|*[!0-9]*) echo 0 ;; *) echo "$t" ;; esac
}

record_recovery_attempt() {
    mkdir -p "$STATE_DIR" || die "cannot create $STATE_DIR"
    printf '%s %s\n' "$1" "$(date +%s)" > "$RECOVERY"
}

clear_recovery() { rm -f "$RECOVERY"; }

# --- is there anything to do? ------------------------------------------------
# --tags --force so a moved `client-live` is picked up rather than kept at
# whatever it pointed to the first time.
git fetch origin main --tags --force --quiet || die "git fetch failed"
TARGET=$(git rev-parse origin/main)
DEPLOYED=$(deployed_commit)
RECOVERING=0

if [ -z "$DEPLOYED" ]; then
    # An upgrade from the version of this script that had no stamp, or a fresh
    # host. Deploy once to reach a state we can vouch for. Seeding the stamp
    # from HEAD instead would be exactly the assumption that caused outage 1;
    # docs/deploy.md says how to seed it by hand if you would rather not
    # rebuild right now.
    REASON="no deploy stamp at $STAMP; nothing here knows what is running"
elif [ "$DEPLOYED" != "$TARGET" ]; then
    REASON="deployed $(short "$DEPLOYED") -> main $(short "$TARGET")"
    if [ "$(git rev-parse HEAD)" = "$TARGET" ]; then
        # Worth saying out loud: this is the shape of outage 1, and the old
        # script would have reported "nothing to do" here.
        say "note: the checkout is already at main but was never deployed"
    fi
elif ! services_healthy; then
    REASON="deployed $(short "$DEPLOYED") is not serving"
    RECOVERING=1
else
    say "up to date at $(short "$DEPLOYED") and serving; nothing to do"
    clear_recovery
    exit 0
fi

say "$REASON"

# A dirty tree means someone was working here by hand. Rebuilding would either
# ship their edit or destroy it; neither is this script's call to make.
if ! git diff --quiet || ! git diff --cached --quiet; then
    die "working tree has uncommitted changes; refusing to touch it"
fi

if [ "$RECOVERING" = "1" ]; then
    ATTEMPTS=$(recovery_attempts)
    SINCE=$(( $(date +%s) - $(recovery_last) ))
    if [ "$ATTEMPTS" -ge "$MAX_RECOVERY_BURST" ] && [ "$SINCE" -lt "$RECOVERY_BACKOFF_SECONDS" ]; then
        say "DECLINING: $ATTEMPTS recovery attempts have already failed and the"
        say "           last was ${SINCE}s ago. Backing off to one an hour so the"
        say "           real error stays readable. Look at:"
        say "             docker compose -f deploy/compose.yaml --env-file deploy/.env logs --tail 100"
        say "           Clear $RECOVERY to retry immediately."
        exit 2
    fi
    # Not under --dry-run: it reports the decision and must not spend one of
    # the five attempts that decision is counted against.
    [ "$DRY_RUN" = "1" ] || record_recovery_attempt $((ATTEMPTS + 1))
else
    git log --oneline HEAD..origin/main | sed 's/^/[autodeploy]   /'
fi

# --- protocol guard ----------------------------------------------------------
# Only when the deployed protocol is actually about to change. A recovery
# redeploys the same commit, so it cannot move the protocol and does not need
# the guard -- and must not be blocked by it, or a stack that went down during
# a client lag could never be brought back up.
OLD_PROTOCOL=""
if [ -n "$DEPLOYED" ] && git cat-file -e "$DEPLOYED^{commit}" 2>/dev/null; then
    OLD_PROTOCOL=$(protocol_version_at "$DEPLOYED")
fi
NEW_PROTOCOL=$(protocol_version_at "$TARGET")
[ -n "$NEW_PROTOCOL" ] || die "could not read kProtocolVersion at $(short "$TARGET")"

if [ "$OLD_PROTOCOL" != "$NEW_PROTOCOL" ]; then
    say "protocol ${OLD_PROTOCOL:-unknown} -> $NEW_PROTOCOL; checking what the published client speaks"
    LIVE_PROTOCOL=$(protocol_version_at "$LIVE_CLIENT_TAG")

    if [ "$FORCE" = "1" ]; then
        say "WARNING: --force, skipping the protocol guard"
    elif [ -z "$LIVE_PROTOCOL" ] && [ -n "$OLD_PROTOCOL" ]; then
        # The protocol demonstrably moved and nothing vouches for a client.
        say "REFUSING: protocol would be $NEW_PROTOCOL and the '$LIVE_CLIENT_TAG' tag"
        say "          does not exist, so no client is known to be published."
        say "          It is created by the deploy-web CI job on a push to main."
        exit 2
    elif [ -z "$LIVE_PROTOCOL" ]; then
        # No stamp AND no published client: a host being set up for the first
        # time. There is nothing live to break, and refusing here would mean a
        # new deployment could never bring its server up at all -- the guard
        # would be protecting a client that does not exist yet.
        say "no '$LIVE_CLIENT_TAG' tag and no deploy stamp: nothing is published to break"
    elif [ "$LIVE_PROTOCOL" != "$NEW_PROTOCOL" ]; then
        say "REFUSING: server would be protocol $NEW_PROTOCOL but the published"
        say "          client ($LIVE_CLIENT_TAG, $(short "$LIVE_CLIENT_TAG"))"
        say "          speaks $LIVE_PROTOCOL. Every connection would be rejected."
        say "          Publish the matching client first."
        exit 2
    else
        say "published client speaks $LIVE_PROTOCOL; matched"
    fi
fi

if [ "$DRY_RUN" = "1" ]; then
    say "dry run: would deploy $(short "$TARGET")"
    exit 0
fi

# --- deploy ------------------------------------------------------------------
# Roll back to the last commit KNOWN to have served, not to HEAD. They differ
# in precisely the case worth handling: a checkout someone advanced by hand,
# where HEAD is the build that was never proven and the stamp is the one that
# was.
ROLLBACK_TO="$DEPLOYED"
if [ -z "$ROLLBACK_TO" ] || ! git cat-file -e "$ROLLBACK_TO^{commit}" 2>/dev/null; then
    ROLLBACK_TO=$(git rev-parse HEAD)
fi

# Still --ff-only, and still fatal when it fails. A checkout that cannot
# fast-forward has a local commit on it, which the dirty-tree check above
# cannot see -- and `reset --hard` would delete someone's work to save a
# deploy, which is the wrong trade in both directions.
say "pulling"
git merge --ff-only origin/main >/dev/null || die "fast-forward failed"

say "building and restarting (a few minutes)"
if compose up -d --build --wait; then
    :
else
    say "deploy failed; rolling back to $(short "$ROLLBACK_TO")"
    git reset --hard "$ROLLBACK_TO" >/dev/null
    # Best effort: if this also fails there is nothing left to try from here,
    # and leaving the host loudly broken beats leaving it quietly wrong.
    if compose up -d --build --wait; then
        record_deployed "$ROLLBACK_TO"
    else
        say "ROLLBACK ALSO FAILED -- server is down"
    fi
    exit 1
fi

# --- verify ------------------------------------------------------------------
# `--wait` already blocked on the container's healthcheck, which is ws_smoke
# against itself. This is the second opinion: the process is up AND says it is
# running what we just deployed.
record_deployed "$TARGET"
clear_recovery
say "deployed $(short "$TARGET")"
compose logs --tail 40 server 2>/dev/null | grep -iE "Map:|Bot skill|listening|Stats:" |
    sed 's/^/[autodeploy]   /'
say "done"
