#!/bin/sh
# Keeps the forks and this repository in step.
#
#   scripts/sync.sh setup      once per clone: submodules follow checkouts and
#                              pulls, and a push sends fork commits first
#   scripts/sync.sh status     each fork against its GitHub branch and upstream
#   scripts/sync.sh push       push the forks' dualize branches, then this repo
#   scripts/sync.sh upstream   fetch the forks' upstreams and list what's new
#
# The forks live on their dualize branch; this repository pins the commit of
# each. A change to a fork is a commit there, then a commit here that moves the
# pin (git add tools/DolRecomp).
set -eu
cd "$(dirname "$0")/.."

FORKS="tools/DolRecomp thirdparty/aurora"

upstream_of() {
    case "$1" in
    tools/DolRecomp) echo "https://github.com/ExpansionPak/DolRecomp.git main" ;;
    thirdparty/aurora) echo "https://github.com/encounter/aurora.git main" ;;
    esac
}

check_fork() {
    branch=$(git -C "$1" branch --show-current)
    if [ "$branch" != dualize ]; then
        echo "$1: on '${branch:-a detached HEAD}', not dualize" >&2
        return 1
    fi
    if [ -n "$(git -C "$1" status --porcelain)" ]; then
        echo "$1: uncommitted changes" >&2
        return 1
    fi
}

case "${1:-status}" in
setup)
    git config submodule.recurse true
    git config push.recurseSubmodules on-demand
    git config status.submoduleSummary true
    git config diff.submodule log
    git submodule update --init
    for fork in $FORKS; do
        git -C "$fork" checkout -q dualize 2>/dev/null ||
            git -C "$fork" checkout -q -b dualize --track origin/dualize
        set -- $(upstream_of "$fork")
        git -C "$fork" remote get-url upstream >/dev/null 2>&1 ||
            git -C "$fork" remote add upstream "$1"
    done
    echo "submodules follow checkouts; git push sends fork commits first"
    ;;
status)
    for fork in $FORKS; do
        git -C "$fork" fetch -q origin dualize
        pinned=$(git ls-tree HEAD "$fork" | awk '{print $3}')
        head=$(git -C "$fork" rev-parse HEAD)
        printf '%s: %s\n' "$fork" "$(git -C "$fork" status -sb | head -1)"
        [ "$pinned" = "$head" ] || echo "  pinned here at ${pinned%"${pinned#???????}"}: commit the new pin"
    done
    ;;
push)
    for fork in $FORKS; do
        check_fork "$fork"
        git -C "$fork" push origin dualize
    done
    git push
    ;;
upstream)
    for fork in $FORKS; do
        set -- $(upstream_of "$fork")
        git -C "$fork" remote get-url upstream >/dev/null 2>&1 ||
            git -C "$fork" remote add upstream "$1"
        git -C "$fork" fetch -q upstream "$2"
        echo "== $fork: upstream/$2 has $(git -C "$fork" rev-list --count "HEAD..upstream/$2") commits we don't"
        git -C "$fork" log --oneline -10 "HEAD..upstream/$2"
    done
    ;;
*)
    sed -n '2,13p' "$0"
    exit 2
    ;;
esac
