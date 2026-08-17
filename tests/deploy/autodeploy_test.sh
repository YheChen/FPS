#!/usr/bin/env bash
# Decision-table tests for tools/server_autodeploy.sh.
#
# This script is the one piece of the project that has taken the live game
# down, twice in one day, and both times because it INFERRED what was deployed
# instead of knowing. Cases 3 and 4 below are those two outages; they fail
# against the version of the script that asked "does HEAD equal origin/main?".
#
# No docker and no network: `docker` is a stub on PATH that answers from
# environment variables and logs what it was asked to do, and "GitHub" is a
# bare repository in a temp directory. So this runs anywhere, in about a
# second, which is the difference between a test that runs in CI and a
# deployment check nobody performs.
#
# Usage: tests/deploy/autodeploy_test.sh
set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AUTODEPLOY="$SCRIPT_DIR/../../tools/server_autodeploy.sh"
[ -x "$AUTODEPLOY" ] || AUTODEPLOY="bash $AUTODEPLOY"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PASSED=0
FAILED=0

fail() { echo "  FAIL: $*"; FAILED=$((FAILED + 1)); }
pass() { PASSED=$((PASSED + 1)); }

check() {  # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then pass; else fail "$1: expected '$2', got '$3'"; fi
}

check_contains() {  # check_contains <description> <needle> <haystack>
    case "$3" in
        *"$2"*) pass ;;
        *) fail "$1: expected output to mention '$2'"; echo "--- output ---"; echo "$3" ;;
    esac
}

check_not_contains() {
    case "$3" in
        *"$2"*) fail "$1: output should NOT mention '$2'"; echo "--- output ---"; echo "$3" ;;
        *) pass ;;
    esac
}

# --- the docker stub ---------------------------------------------------------
# Answers `compose ps -q`, `compose up`, `compose logs` and `inspect` from the
# environment, and appends every invocation to $FAKE_DOCKER_LOG so a test can
# assert that a deploy did or did not happen.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/docker" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${FAKE_DOCKER_LOG:-/dev/null}"

container_state() {  # prints "<status> <health>"; empty when absent
    case "$1" in
        server) echo "${FAKE_SERVER_STATE-running healthy}" ;;
        caddy)  echo "${FAKE_CADDY_STATE-running none}" ;;
    esac
}

if [ "${1:-}" = "compose" ]; then
    shift
    while [ $# -gt 0 ]; do
        case "$1" in
            -f|--file|--env-file|-p|--project-name) shift 2 ;;
            *) break ;;
        esac
    done
    case "${1:-}" in
        ps)
            service="${!#}"
            state=$(container_state "$service")
            [ -n "$state" ] && echo "cid-$service"
            exit 0 ;;
        up)
            # FAKE_UP_EXITS is a space-separated list consumed one per call,
            # so a test can make the first `up` fail and the rollback succeed.
            n=0
            [ -r "${FAKE_UP_COUNT:-/nonexistent}" ] && n=$(cat "$FAKE_UP_COUNT")
            [ -n "${FAKE_UP_COUNT:-}" ] && echo $((n + 1)) > "$FAKE_UP_COUNT"
            read -r -a exits <<< "${FAKE_UP_EXITS:-0}"
            exit "${exits[$n]:-${exits[${#exits[@]} - 1]}}" ;;
        logs) exit 0 ;;
        *) exit 0 ;;
    esac
fi

if [ "${1:-}" = "inspect" ]; then
    template="$3"
    id="$4"
    service="${id#cid-}"
    read -r status health <<< "$(container_state "$service")"
    case "$template" in
        *Health*) echo "$health" ;;
        *) echo "$status" ;;
    esac
    exit 0
fi
exit 0
STUB
chmod +x "$WORK/bin/docker"
export PATH="$WORK/bin:$PATH"

# --- a repository that looks like the deployment ----------------------------
# "origin" is a bare repo; "host" is the clone the T14 would have. Commits
# carry a real protocol.h, because the guard reads kProtocolVersion out of git
# objects rather than the worktree.
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com

ORIGIN="$WORK/origin.git"
SRC="$WORK/src"
git init --quiet --bare -b main "$ORIGIN"
git init --quiet -b main "$SRC"
git -C "$SRC" remote add origin "$ORIGIN"

commit_with_protocol() {  # commit_with_protocol <version> <message>
    mkdir -p "$SRC/game/shared" "$SRC/deploy"
    printf 'inline constexpr std::uint16_t kProtocolVersion = %s;\n' "$1" \
        > "$SRC/game/shared/protocol.h"
    : > "$SRC/deploy/compose.yaml"
    : > "$SRC/deploy/.env"
    git -C "$SRC" add -A
    # --allow-empty: consecutive commits at the same protocol version change
    # no file, and a commit that silently does not happen makes every SHA in
    # the test alias the previous one.
    git -C "$SRC" commit --quiet --allow-empty -m "$2"
    git -C "$SRC" push --quiet origin main
    git -C "$SRC" rev-parse HEAD
}

BASE=$(commit_with_protocol 7 "base")

# What deploy-web does after it has verified a client is live.
publish_client() {
    git -C "$SRC" tag -f client-live "$1" >/dev/null 2>&1
    git -C "$SRC" push --quiet -f origin refs/tags/client-live
}
publish_client "$BASE"

# Each case gets its own clone and its own state directory, so nothing leaks.
HOST_N=0
new_host() {  # new_host [commit-to-check-out] -> sets HOST and FPS_STATE_DIR
    HOST_N=$((HOST_N + 1))
    HOST="$WORK/host$HOST_N"
    # --no-hardlinks: cloning a LOCAL path makes git hardlink the object pack
    # into the new repo instead of copying it. That is a real speedup and a
    # real flake -- on 2026-08-17 a CI run died on "fatal: hardlink different
    # from source" partway through case 14, failing three assertions for a
    # reason that had nothing to do with what they test. Fifteen small clones
    # is not where this suite spends its time.
    git clone --quiet --no-hardlinks "${HOST_ORIGIN:-$ORIGIN}" "$HOST"
    [ $# -gt 0 ] && git -C "$HOST" reset --hard --quiet "$1"
    export FPS_STATE_DIR="$WORK/state$HOST_N"
    export FAKE_DOCKER_LOG="$WORK/docker$HOST_N.log"
    export FAKE_UP_COUNT="$WORK/upcount$HOST_N"
    rm -f "$FAKE_DOCKER_LOG" "$FAKE_UP_COUNT"
    unset FAKE_SERVER_STATE FAKE_CADDY_STATE FAKE_UP_EXITS
}

stamp() { mkdir -p "$FPS_STATE_DIR"; printf '%s\n' "$1" > "$FPS_STATE_DIR/deployed-commit"; }
read_stamp() { cat "$FPS_STATE_DIR/deployed-commit" 2>/dev/null; }
run() { $AUTODEPLOY --repo "$HOST" "$@" 2>&1; }
deployed_this_run() { grep -qE '(^| )up ' "$FAKE_DOCKER_LOG" 2>/dev/null; }

# =============================================================================
echo "1. a host with no stamp deploys once, because nothing knows what is running"
new_host
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "reason" "no deploy stamp" "$OUT"
check "stamp now names main" "$BASE" "$(read_stamp)"
if deployed_this_run; then pass; else fail "should have run compose up"; fi

# =============================================================================
echo "2. up to date and serving: does nothing at all"
new_host
stamp "$BASE"
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "says so" "nothing to do" "$OUT"
if deployed_this_run; then fail "must not rebuild a healthy up-to-date host"; else pass; fi

# =============================================================================
echo "3. OUTAGE 1: someone ran 'git pull' by hand, so HEAD is main but nothing was built"
# The checkout is at main and the stack is healthy -- the old script's two
# signals both say "fine" -- but the container is still serving BASE.
NEXT=$(commit_with_protocol 7 "a fix nobody deployed")
new_host           # clone is at NEXT, i.e. HEAD == origin/main
stamp "$BASE"      # ...but BASE is what is actually running
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "names the gap" "was never deployed" "$OUT"
check "stamp advances" "$NEXT" "$(read_stamp)"
if deployed_this_run; then pass; else fail "OUTAGE 1 REGRESSION: did not redeploy"; fi

# =============================================================================
echo "4. OUTAGE 2: 'docker compose down' left the stack stopped at the right commit"
new_host
stamp "$NEXT"
export FAKE_SERVER_STATE=""   # no container at all
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "notices" "no container for 'server'" "$OUT"
check_contains "acts" "is not serving" "$OUT"
if deployed_this_run; then pass; else fail "OUTAGE 2 REGRESSION: left the server down"; fi
unset FAKE_SERVER_STATE

# =============================================================================
echo "5. a container that is up but failing its healthcheck is also redeployed"
new_host
stamp "$NEXT"
export FAKE_SERVER_STATE="running unhealthy"
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "names the health" "running but unhealthy" "$OUT"
if deployed_this_run; then pass; else fail "did not redeploy an unhealthy container"; fi
unset FAKE_SERVER_STATE

# =============================================================================
echo "6. caddy down counts too: a healthy server behind a stopped proxy is unreachable"
new_host
stamp "$NEXT"
export FAKE_CADDY_STATE="exited none"
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "names caddy" "'caddy' is exited" "$OUT"
if deployed_this_run; then pass; else fail "did not redeploy with caddy down"; fi
unset FAKE_CADDY_STATE

# =============================================================================
echo "7. a protocol bump the published client cannot speak is refused"
BUMPED=$(commit_with_protocol 8 "protocol 8")
publish_client "$NEXT"   # the published client still speaks 7
new_host
stamp "$NEXT"
OUT=$(run); RC=$?
check "exit code" 2 "$RC"
check_contains "refuses" "REFUSING" "$OUT"
check "stamp unchanged" "$NEXT" "$(read_stamp)"
if deployed_this_run; then fail "must not deploy past the published client"; else pass; fi

# =============================================================================
echo "8. ...and is deployed once the published client speaks it"
publish_client "$BUMPED"
new_host
stamp "$NEXT"
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "matched" "matched" "$OUT"
check "stamp advances" "$BUMPED" "$(read_stamp)"

# =============================================================================
echo "9. recovery is NOT blocked by the protocol guard"
# The stack is down at a commit whose protocol the published client no longer
# matches. Refusing here would mean the guard keeps the server switched off --
# a redeploy of the same commit cannot move the protocol, so it must proceed.
publish_client "$NEXT"
new_host "$BUMPED"
git -C "$HOST" checkout --quiet -B main "$BUMPED"
stamp "$BUMPED"
export FAKE_SERVER_STATE=""
OUT=$(run); RC=$?
unset FAKE_SERVER_STATE
check "exit code" 0 "$RC"
check_not_contains "no refusal" "REFUSING" "$OUT"
if deployed_this_run; then pass; else fail "guard blocked a recovery"; fi
publish_client "$BUMPED"

# =============================================================================
echo "10. a failed deploy rolls back to the STAMPED commit, not to HEAD"
# HEAD was advanced by hand to a commit that was never deployed, so HEAD is
# not a safe place to roll back to. The stamp is.
new_host                      # clone is at BUMPED == origin/main
stamp "$BASE"                 # but BASE is what actually served
export FAKE_UP_EXITS="1 0"    # deploy fails, rollback succeeds
OUT=$(run); RC=$?
unset FAKE_UP_EXITS
check "exit code" 1 "$RC"
check_contains "rolls back to the stamp" "rolling back to $(git -C "$HOST" rev-parse --short "$BASE")" "$OUT"
check "stamp records the rollback" "$BASE" "$(read_stamp)"
check "checkout is back at the stamp" "$BASE" "$(git -C "$HOST" rev-parse HEAD)"

# =============================================================================
echo "11. repeated failed recoveries back off instead of rebuilding every ten minutes"
new_host
stamp "$BUMPED"
git -C "$HOST" checkout --quiet -B main "$BUMPED"
export FAKE_SERVER_STATE=""
mkdir -p "$FPS_STATE_DIR"
printf '5 %s\n' "$(date +%s)" > "$FPS_STATE_DIR/recovery-attempts"
OUT=$(run); RC=$?
check "exit code" 2 "$RC"
check_contains "declines" "DECLINING" "$OUT"
if deployed_this_run; then fail "should have backed off"; else pass; fi
# An old enough last attempt tries again.
printf '5 %s\n' "$(( $(date +%s) - 7200 ))" > "$FPS_STATE_DIR/recovery-attempts"
rm -f "$FAKE_DOCKER_LOG"
OUT=$(run); RC=$?
unset FAKE_SERVER_STATE
check "exit code after the backoff window" 0 "$RC"
if deployed_this_run; then pass; else fail "backoff never expired"; fi

# =============================================================================
echo "12. a dirty worktree is left alone"
new_host
stamp "$BASE"
echo "someone was working here" > "$HOST/game/shared/protocol.h"
OUT=$(run); RC=$?
check "exit code" 1 "$RC"
check_contains "says why" "uncommitted changes" "$OUT"
if deployed_this_run; then fail "touched a dirty worktree"; else pass; fi

# =============================================================================
echo "13. --dry-run decides but changes nothing"
new_host
stamp "$BASE"
OUT=$(run --dry-run); RC=$?
check "exit code" 0 "$RC"
check_contains "says what it would do" "dry run: would deploy" "$OUT"
check "stamp untouched" "$BASE" "$(read_stamp)"
if deployed_this_run; then fail "--dry-run deployed"; else pass; fi
# ...including the recovery counter: a dry run reports the decision, so it
# must not spend one of the five attempts that decision is counted against.
new_host
stamp "$BUMPED"
git -C "$HOST" checkout --quiet -B main "$BUMPED"
export FAKE_SERVER_STATE=""
OUT=$(run --dry-run); RC=$?
unset FAKE_SERVER_STATE
check "exit code" 0 "$RC"
check "no recovery attempt recorded" "" "$(cat "$FPS_STATE_DIR/recovery-attempts" 2>/dev/null)"

# =============================================================================
echo "14. a brand-new deployment, with no stamp and no client published yet"
# The guard exists to stop a server outrunning the LIVE client. When no client
# has ever been published there is none to outrun, and refusing would mean a
# fresh host could never bring its server up at all.
FRESH_ORIGIN="$WORK/fresh.git"
git init --quiet --bare -b main "$FRESH_ORIGIN"
git -C "$SRC" remote add fresh "$FRESH_ORIGIN"
git -C "$SRC" push --quiet fresh main          # commits, but no client-live tag
HOST_ORIGIN="$FRESH_ORIGIN" new_host        # a clone that has never seen the tag
OUT=$(run); RC=$?
check "exit code" 0 "$RC"
check_contains "explains itself" "nothing is published to break" "$OUT"
if deployed_this_run; then pass; else fail "a fresh deployment could not start its server"; fi

echo
echo "passed $PASSED, failed $FAILED"
[ "$FAILED" -eq 0 ]
