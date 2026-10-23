#!/usr/bin/env bash
# Mirror GitHub-CI-built release artifacts to the self-hosted Gitea repo.
#
# Gitea has no CI runner; GitHub Actions builds and publishes on v* tags
# (.github/workflows/build.yml). This script downloads those artifacts and
# attaches them to the matching Gitea release, creating the release if needed.
#
# Usage:
#   scripts/publish-gitea-release.sh <tag>        # e.g. v0.2.1
#
# Required env:
#   GH_TOKEN     GitHub PAT (read access to the public repo is enough)
# Optional env:
#   GITEA_TOKEN  overrides the token in ~/.config/gitea-ops/config.json
#   GITEA_HOST   overrides the host in the same config (e.g. http://192.168.31.205:5300)
#   GH_REPO      default LIGHT1213/dsviewcli
#   GITEA_REPO   default light/dsviewcli
#   https_proxy / http_proxy honored for GitHub access (Gitea is usually local)

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

tag=${1:?usage: scripts/publish-gitea-release.sh <tag>   (e.g. v0.2.1)}
[[ "$tag" =~ ^v[0-9] ]] || die "tag should look like v0.2.1, got '$tag'"

GH_REPO=${GH_REPO:-LIGHT1213/dsviewcli}
GITEA_REPO=${GITEA_REPO:-light/dsviewcli}
cfg="$HOME/.config/gitea-ops/config.json"

[[ -n "${GH_TOKEN:-}" ]] || die "GH_TOKEN not set (GitHub PAT for downloading CI artifacts)"

gtok=${GITEA_TOKEN:-}
ghost=${GITEA_HOST:-}
if [[ -z "$gtok" || -z "$ghost" ]] && [[ -f "$cfg" ]]; then
    # tr -d '\r': native Windows python writes CRLF, command substitution
    # strips only LF and a stray CR would end up inside the token/host.
    GITEA_HOST=${GITEA_HOST:-$(python -c 'import json,sys;print(json.load(open(sys.argv[1])).get("host",""))' "$cfg" | tr -d '\r')}
    gtok=${GITEA_TOKEN:-$(python -c 'import json,sys;print(json.load(open(sys.argv[1])).get("token",""))' "$cfg" | tr -d '\r')}
    ghost=$GITEA_HOST
fi
[[ -n "$ghost" ]] || die "Gitea host unknown: set GITEA_HOST or create $cfg"
[[ -n "$gtok" ]]  || die "Gitea token unknown: set GITEA_TOKEN or create $cfg"

# --- GitHub side: fetch the CI-published release for this tag -----------------
gh_rel=$(curl -sf -H "Authorization: token $GH_TOKEN" \
    "https://api.github.com/repos/$GH_REPO/releases/tags/$tag") \
    || die "GitHub release $tag not found (was the tag pushed and did the CI release job finish?)"

mapfile -t assets < <(python -c 'import json,sys
for a in json.load(sys.stdin)["assets"]:
    print(a["name"], a["browser_download_url"])' <<<"$gh_rel" | tr -d '\r')
[[ ${#assets[@]} -gt 0 ]] || die "release $tag has no assets on GitHub (CI release job may have skipped)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

declare -A sums
for line in "${assets[@]}"; do
    name=${line%% *} url=${line#* }
    echo "  download $name"
    curl -sfL -H "Authorization: token $GH_TOKEN" -o "$tmp/$name" "$url" \
        || die "download failed: $name"
    sums[$name]=$(sha256sum "$tmp/$name" | cut -d' ' -f1)
done

# --- Gitea side: find or create the release -----------------------------------
# Notes stay single-line ASCII: earlier attempts showed the Gitea API rejects
# some multi-line bodies through this path.
rel_id=$(curl -s -H "Authorization: token $gtok" \
    "$ghost/api/v1/repos/$GITEA_REPO/releases/tags/$tag" \
    | python -c 'import json,sys
try: print(json.load(sys.stdin)["id"])
except Exception: pass' | tr -d '\r')
if [[ -z "${rel_id:-}" ]]; then
    notes="$tag binaries mirrored from GitHub Actions CI. sha256:"
    for name in "${!sums[@]}"; do notes+=" $name=${sums[$name]}"; done
    body=$(python -c 'import json,sys
print(json.dumps({"tag_name": sys.argv[1],
                  "name": sys.argv[1] + " (Windows x64)",
                  "body": sys.argv[2]}))' "$tag" "$notes" | tr -d '\r')
    rel_id=$(curl -sf -X POST -H "Authorization: token $gtok" \
        -H "Content-Type: application/json" -d "$body" \
        "$ghost/api/v1/repos/$GITEA_REPO/releases" \
        | python -c 'import json,sys;print(json.load(sys.stdin)["id"])' | tr -d '\r') \
        || die "failed to create Gitea release for $tag"
    echo "  created Gitea release id=$rel_id"
else
    echo "  Gitea release exists (id=$rel_id), attaching missing assets"
fi

# --- attach assets (skip names already present) -------------------------------
existing=$(curl -sf -H "Authorization: token $gtok" \
    "$ghost/api/v1/repos/$GITEA_REPO/releases/$rel_id/assets")
for line in "${assets[@]}"; do
    name=${line%% *}
    if grep -q "\"name\":\"$name\"" <<<"$existing"; then
        echo "  skip $name (already attached)"
        continue
    fi
    curl -sf -X POST -H "Authorization: token $gtok" \
        -F "attachment=@$tmp/$name" \
        "$ghost/api/v1/repos/$GITEA_REPO/releases/$rel_id/assets?name=$name" >/dev/null \
        || die "upload failed: $name"
    echo "  uploaded $name (${sums[$name]})"
done

echo "done: $ghost/$GITEA_REPO/releases/tag/$tag"
