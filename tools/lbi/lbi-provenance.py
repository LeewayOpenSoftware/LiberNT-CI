#!/usr/bin/env python3
# PROJECT:     LiberNT Source Provenance (LBI WS-C)
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Generate the LBI per-build provenance manifest from a git clone
# COPYRIGHT:   Copyright 2026 Leeway Open Software
"""
lbi-provenance.py - LBI WS-C prototype manifest generator (schema 1.0).

Implements shared/libernt/lbi/WS-C-PROVENANCE.md section 5, steps 1-6 and 8
(artifact hashing, step 7, is left to a build-system wrapper). Every field is
produced by a recorded method; fields without evidence are emitted as null
with an explicit confidence - this tool never fabricates ancestry.

Usage:
  lbi-provenance.py --repo PATH [--source-ref REF] [--upstream-ref REF]
      [--source-remote NAME] [--upstream-url URL] [--ledger DIR]
      [--cmake-cache FILE] [--arch ARCH] [--build-type TYPE]
      [--patch-id-sample N] [--output FILE]

Defaults follow the WS-C example: source refs/heads/leeway/main, upstream
up/main-nt10, ledger shared/libernt/upstream-review (relative to CWD).
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone


def fail(msg):
    sys.stderr.write("lbi-provenance: error: %s\n" % msg)
    sys.exit(2)


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo] + list(args),
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        fail("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r


def git_out(repo, *args, **kw):
    return git(repo, *args, **kw).stdout.strip()


def tree_blobs(repo, ref):
    """path -> blob sha for every file in ref's tree (shallow-safe)."""
    out = git_out(repo, "ls-tree", "-r", ref)
    blobs = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        mode, typ, sha = meta.split()
        if typ == "blob":
            blobs[path] = sha
    return blobs


def merge_base(repo, a, b):
    r = git(repo, "merge-base", a, b, check=False)
    return r.stdout.strip() if r.returncode == 0 else None


def patch_id(repo, sha):
    show = git(repo, "show", sha, check=False)
    if show.returncode != 0:
        return None
    pid = subprocess.run(["git", "patch-id", "--stable"],
                         input=show.stdout, capture_output=True, text=True)
    if pid.returncode != 0 or not pid.stdout.strip():
        return None
    return pid.stdout.split()[0]


def picked_table_stats(repo, ledger):
    """Per-batch stats from PICKED-<id>.txt files, with a patch-id sample."""
    import glob, os
    batches = []
    for path in sorted(glob.glob(os.path.join(ledger, "PICKED-*.txt"))):
        batch_id = re.search(r"PICKED-(\w+)\.txt$", path).group(1)
        pairs = []
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r"([0-9a-f]+)\s*<-\s*([0-9a-f]+)\s*::", line.strip())
                if m:
                    pairs.append((m.group(1), m.group(2)))
        batches.append((batch_id, path, pairs))
    return batches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--source-ref", default="refs/heads/leeway/main")
    ap.add_argument("--upstream-ref", default="up/main-nt10")
    ap.add_argument("--source-remote", default="origin")
    ap.add_argument("--upstream-url",
                    default="https://github.com/LiberNT/LiberNT.git")
    ap.add_argument("--ledger", default="shared/libernt/upstream-review")
    ap.add_argument("--cmake-cache")
    ap.add_argument("--arch")
    ap.add_argument("--build-type")
    ap.add_argument("--patch-id-sample", type=int, default=5)
    ap.add_argument("--output")
    args = ap.parse_args()

    src = args.source_ref
    up = args.upstream_ref
    head = git_out(args.repo, "rev-parse", "--verify", src)
    tree = git_out(args.repo, "rev-parse", head + "^{tree}")
    cdate = git_out(args.repo, "log", "-1", "--format=%cI", head)
    origin_url = git(args.repo, "config", "remote.%s.url" % args.source_remote,
                     check=False).stdout.strip() or None

    mb = merge_base(args.repo, up, head)
    up_tip = git(args.repo, "rev-parse", "--verify", up,
                 check=False).stdout.strip() or None

    # content equality + local delta (skip gracefully if upstream absent)
    shared = identical = 0
    differing = src_only = up_only = 0
    diff_by_area = {}
    if up_tip:
        a = tree_blobs(args.repo, up)
        b = tree_blobs(args.repo, head)
        common = set(a) & set(b)
        shared = len(common)
        identical = sum(1 for p in common if a[p] == b[p])
        differing = shared - identical
        src_only = len(set(b) - set(a))
        up_only = len(set(a) - set(b))
        from collections import Counter
        diff_by_area = dict(Counter(p.split("/")[0]
                                    for p in common if a[p] != b[p]).most_common(12))

    # sync batches: table presence + patch-id sample verification
    batches = []
    for batch_id, path, pairs in picked_table_stats(args.repo, args.ledger):
        checked = matched = missing = 0
        step = max(1, len(pairs) // max(1, args.patch_id_sample))
        for new, old in pairs[::step][:args.patch_id_sample]:
            pn, po = patch_id(args.repo, new), patch_id(args.repo, old)
            if pn is None or po is None:
                missing += 1
                continue
            checked += 1
            matched += (pn == po)
        batches.append({
            "id": batch_id,
            "picked_table": path,
            "picked_count": len(pairs),
            "patch_id_sample": {"checked": checked, "matched": matched,
                                "missing_objects": missing},
            "confidence": "patch-equal" if checked and matched == checked
                          else "recorded",
        })

    # reactos-era lineage probe: candidate upstream refs, deepest wins; a
    # small count means the local ref is shallow-clipped and we say so
    lineage = {"in_repo_evidence": None, "confidence": "unverified",
               "exact_fork_revision": {"value": None,
                                       "confidence": "unverified",
                                       "external_check":
                                           "compare LiberNT/LiberNT against "
                                           "reactos/reactos via GitHub fork "
                                           "network or tree match"}}
    if up_tip:
        best = None
        for cand in [up, "up/main"]:
            count = git_out(args.repo, "rev-list", "--count", cand,
                            check=False)
            if not count:
                continue
            count = int(count)
            if best is None or count > best[1]:
                best = (cand, count)
        if best:
            cand, count = best
            oldest = git_out(args.repo, "log", "--format=%H %cI", cand,
                             check=False)
            first = oldest.splitlines()[-1].split() if oldest else []
            lineage["in_repo_evidence"] = {
                "ref_used": cand,
                "oldest_commit": first[0] if first else None,
                "oldest_commit_date": first[1] if len(first) > 1 else None,
                "commit_count": count,
                "shallow_clipped": count < 10000,
                "method": "git log/rev-list on " + cand,
            }
            lineage["confidence"] = "proven-ancestry (inside upstream clone)"

    toolchain = {"compiler": None, "generator": None, "build_type": None,
                 "arch": args.arch, "cmake_defines_digest": None}
    if args.cmake_cache:
        kv = {}
        with open(args.cmake_cache, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r"(\w+):(\w+)=(.*)", line.strip())
                if m:
                    kv[m.group(1)] = m.group(3)
        toolchain["compiler"] = {
            "id": kv.get("CMAKE_C_COMPILER_ID"),
            "version": kv.get("CMAKE_C_COMPILER_ID") and
                       kv.get("CMAKE_C_COMPILER_VERSION"),
        }
        toolchain["generator"] = kv.get("CMAKE_GENERATOR")
        toolchain["build_type"] = kv.get("CMAKE_BUILD_TYPE") or args.build_type
        defines = sorted(k for k in kv if k.startswith("ENABLE_"))
        toolchain["cmake_defines_digest"] = hashlib.sha256(
            "\n".join(defines).encode()).hexdigest()

    manifest = {
        "manifest": {
            "schema_id": "lbi-provenance-manifest/1.0",
            "schema_version": "1.0",
            "manifest_id": None,
            "generated_at": datetime.now(timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"),
            "generator": {"name": "lbi-provenance", "version": "0.1.0",
                          "schema_ref": "WS-C-PROVENANCE.md#3"},
        },
        "build": {
            "lbi": None,
            "lbi_attempt_id": None,
            "manifest_ref": None,
            "source": {
                "remote": origin_url,
                "ref": src,
                "commit": head,
                "tree": tree,
                "committer_date": cdate,
            },
        },
        "upstream": {
            "repository": args.upstream_url,
            "branch": up,
            "tip_at_manifest_time": up_tip,
            "ancestry": {
                "merge_base_with_source": mb,
                "merge_base_method": "git merge-base %s %s" % (up, head),
                "confidence": "proven-ancestry" if mb else "none-recorded",
            },
            "sync": {"batches": batches,
                     "ledger": args.ledger},
            "content_equality": {
                "vs_upstream_commit": up_tip,
                "shared_paths": shared, "identical_blobs": identical,
                "differing_blobs": differing,
                "source_only_paths": src_only,
                "upstream_only_paths": up_only,
                "method": "git ls-tree -r blob-sha compare",
                "confidence": "content-equal" if up_tip else "none-recorded",
            },
        },
        "reactos_lineage": lineage,
        "local_delta": {"differing_blobs_by_area": diff_by_area,
                        "note": "informational; vs upstream ref at generation"},
        "toolchain": toolchain,
        "artifacts": [],
        "notes": "artifact hashes (step 7) require a build-system wrapper; "
                 "lbi/manifest_ref fields await WS-B allocation mechanism",
    }

    canonical = json.dumps(manifest, sort_keys=True, ensure_ascii=False,
                           separators=(",", ":")).encode()
    manifest["manifest"]["manifest_id"] = hashlib.sha256(canonical).hexdigest()
    out = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(out)
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
