#!/bin/sh
# Clone the pinned checkout the build reads into refs/: the shared shader list (tools/shader_list.py), the NVN names
# (tools/gen_nvn_procs.py) and the translator copy tools/build_xlate.py compares tools/vendor/ryujinx with. refs/ is
# read-only and ignored by git.
set -eu

cd "$(dirname "$0")/.."
mkdir -p refs

# fetch <name> <url> <commit> [history]: a shallow checkout of the commit, or with "history" the commit and all its
# ancestors (tools/shader_list.py walks the log of the shader list). origin/HEAD names the commit either
# way, so tools can read the pinned revision as origin/HEAD.
fetch() {
    name=$1 url=$2 commit=$3 history=${4:-}
    dir=refs/$name
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ] &&
       [ "$(git -C "$dir" rev-parse -q --verify refs/remotes/origin/HEAD 2>/dev/null)" = "$commit" ] &&
       { [ -z "$history" ] || [ "$(git -C "$dir" rev-parse --is-shallow-repository)" = false ]; }; then
        echo "$name: up to date"
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    if [ -n "$history" ]; then
        git -C "$dir" fetch -q origin "$commit"
    else
        git -C "$dir" fetch -q --depth 1 origin "$commit"
    fi
    git -C "$dir" checkout -q FETCH_HEAD
    git -C "$dir" update-ref refs/remotes/origin/HEAD "$commit"
    echo "$name: $commit"
}

fetch MacNativeGames https://github.com/ly05010419/MacNativeGames.git d1509b4b62211265fad5449cf53cc9f13484ecc5 history
