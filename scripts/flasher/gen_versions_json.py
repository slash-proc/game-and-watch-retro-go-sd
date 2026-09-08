#!/usr/bin/env python3
"""
Regenerate dist/versions.json from the releases actually mirrored under dist/.

The index is rebuilt from scratch on every deploy rather than edited in place —
editing is how an index drifts from the releases it describes. Each entry's
compatibility fields are read out of that release's own manifest.json, so the
two can never disagree.

Inputs:
  --dist-dir    the Pages tree being assembled; each mirrored release is a
                <tag>/ directory containing manifest.json
  --releases    JSON array from `gh release list --json tagName,publishedAt,isPrerelease`
  --retained    how many versions to keep (newest first)

A release listed by `gh` but not mirrored under --dist-dir is skipped: it was
either published before this format existed or its mirror step failed, and
advertising a manifest that is not there would break every installer that
followed the index.
"""
import argparse
import json
import os
import sys

SCHEMA_VERSION = 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dist-dir", required=True)
    ap.add_argument("--releases", required=True, help="gh release list JSON")
    ap.add_argument("--retained", type=int, default=5)
    ap.add_argument("--repo", default="slash-proc/game-and-watch-retro-go-sd")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.releases, encoding="utf-8") as f:
        releases = json.load(f)

    # `gh release list` is already newest-first, but sort explicitly so the
    # invariant does not depend on the CLI's default ordering.
    releases.sort(key=lambda r: r.get("publishedAt", ""), reverse=True)

    versions = []
    skipped = []
    for rel in releases:
        tag = rel["tagName"]
        manifest_path = os.path.join(args.dist_dir, tag, "manifest.json")
        if not os.path.isfile(manifest_path):
            skipped.append(tag)
            continue
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        fw = manifest["firmware"]
        versions.append(
            {
                "tag": tag,
                "manifest": f"{tag}/manifest.json",
                "publishedAt": rel["publishedAt"],
                "prerelease": bool(rel.get("isPrerelease", False)),
                "gitTag": fw["gitTag"],
                "providesAbi": dict(fw["providesAbi"]),
                "coreMetaVersion": fw["coreMetaVersion"],
            }
        )
        if len(versions) >= args.retained:
            break

    if not versions:
        print(
            f"error: no mirrored releases found under {args.dist_dir}", file=sys.stderr
        )
        return 1

    doc = {
        "schemaVersion": SCHEMA_VERSION,
        "project": "retro-go-sd",
        "title": "Retro-Go SD",
        "repo": args.repo,
        "releasesUrl": f"https://github.com/{args.repo}/releases",
        "retained": args.retained,
        "versions": versions,
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print(f"{args.out}: {len(versions)} versions ({', '.join(v['tag'] for v in versions)})")
    if skipped:
        print(f"  not mirrored, skipped: {', '.join(skipped)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
