#!/bin/sh
# A save store over rclone, so any remote rclone reaches (S3, R2, Google Cloud Storage, Drive, SFTP, ...) holds the
# saves. Point tlk-play at it with {"store": "cmd:/path/to/rclone-store.sh"} in ~/.config/tlk-recomp/sync.json and
# set TLK_RCLONE_REMOTE, such as r2:my-bucket/tlk-saves. It keeps NAME.zip and its sidecar NAME.json side by side,
# so list reads only the small files. Needs rclone, unzip, and jq.
#
#   rclone-store.sh list | get NAME | put NAME < zip
# Exit 2: no such save. Exit 3: the stored copy is as new or newer.
set -eu
remote=${TLK_RCLONE_REMOTE:?set TLK_RCLONE_REMOTE, such as r2:bucket/tlk-saves}
case ${1:-} in
list)
    printf '{'
    sep=
    for f in $(rclone lsf --include '*.json' "$remote" 2>/dev/null); do
        printf '%s"%s": ' "$sep" "${f%.json}"
        rclone cat "$remote/$f"
        sep=', '
    done
    printf '}\n'
    ;;
get)
    rclone lsf "$remote/$2.zip" >/dev/null 2>&1 || exit 2
    rclone cat "$remote/$2.zip"
    ;;
put)
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    cat > "$tmp/$2.zip"
    unzip -p "$tmp/$2.zip" save.json > "$tmp/$2.json"
    sha=$(unzip -p "$tmp/$2.zip" save.state | sha256sum | cut -d' ' -f1)
    [ "$sha" = "$(jq -r .state_sha256 "$tmp/$2.json")" ] || { echo "save.state doesn't match its sidecar" >&2; exit 1; }
    new=$(jq -r .saved_unix "$tmp/$2.json")
    stored=$(rclone cat "$remote/$2.json" 2>/dev/null || true)
    [ "$(printf '%s' "$stored" | jq -r .state_sha256 2>/dev/null)" = "$sha" ] && exit 0
    old=$(printf '%s' "$stored" | jq -r .saved_unix 2>/dev/null || true)
    if [ -n "$old" ] && [ "$old" != null ] && [ "$old" -ge "$new" ]; then
        echo "stored at $old, not older than $new" >&2
        exit 3
    fi
    rclone copyto "$tmp/$2.zip" "$remote/$2.zip"
    rclone copyto "$tmp/$2.json" "$remote/$2.json"
    ;;
*)
    sed -n '2,8p' "$0" >&2
    exit 64
    ;;
esac
