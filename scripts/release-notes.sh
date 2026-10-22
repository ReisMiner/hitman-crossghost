#!/usr/bin/env bash
# Builds release notes for a merge to main: every commit since the previous
# release plus the merge request numbers referenced by those commits.
#
# Usage: release-notes.sh <version> <output> [previous-tag] [repository-url]
set -euo pipefail

version="${1:?usage: release-notes.sh <version> <output> [previous-tag] [repository-url]}"
output="${2:?usage: release-notes.sh <version> <output> [previous-tag] [repository-url]}"
from="${3:-}"
repository_url="${4:-}"

if [[ -n "$from" ]]; then
    range="$from..HEAD"
else
    range="HEAD"
fi

declare -A seen=()
changes=()
merge_requests=()

while IFS=$'\t' read -r sha subject; do
    [[ -z "$subject" ]] && continue

    # Squash merges keep the MR number as a "(#123)" suffix; merge commits
    # name it in the subject.
    if [[ "$subject" =~ \(#([0-9]+)\)[[:space:]]*$ ]]; then
        merge_requests+=("${BASH_REMATCH[1]}")
    fi
    if [[ "$subject" =~ ^Merge\ (pull|merge)\ request\ #([0-9]+) ]]; then
        merge_requests+=("${BASH_REMATCH[2]}")
        continue
    fi
    [[ "$subject" =~ ^Merge\ branch\  ]] && continue

    if [[ "$subject" =~ \(#[0-9]+\)[[:space:]]*$ ]]; then
        entry="$subject"
    else
        entry="$subject ($sha)"
    fi
    if [[ -z "${seen[$entry]:-}" ]]; then
        seen[$entry]=1
        changes+=("- $entry")
    fi
done < <(git log "$range" --pretty=format:'%h%x09%s'; printf '\n')

{
    echo '## Changes'
    echo
    if (( ${#changes[@]} )); then
        printf '%s\n' "${changes[@]}"
    else
        echo '- No commits since the previous release.'
    fi
    echo
    echo '## Merge requests'
    echo
    if (( ${#merge_requests[@]} )); then
        printf '%s\n' "${merge_requests[@]}" | sort -n -u | sed 's/^/- #/'
    else
        echo '- None referenced in the commit titles.'
    fi
    if [[ -n "$repository_url" && -n "$from" ]]; then
        echo
        echo "**Full changelog:** [$from...$version]($repository_url/compare/$from...$version)"
    fi
} > "$output"

echo "Wrote release notes for $version to $output"
