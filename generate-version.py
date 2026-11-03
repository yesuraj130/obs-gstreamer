#!/usr/bin/env python3
"""
Generate version and build metadata for obs-gstreamer.
Captures Git commit hash (+ dirty file count for local builds),
build timestamp (UTC), repository URL, and build type (CI vs Local).
"""
import sys
import os
import subprocess
import datetime

def get_git_info():
    commit = None
    dirty_count = 0
    repo_url = None
    build_type = "Local Build"

    is_ci = os.environ.get("GITHUB_ACTIONS") == "true"

    if is_ci:
        sha = os.environ.get("GITHUB_SHA", "")
        if sha:
            commit = sha[:7]
        repo = os.environ.get("GITHUB_REPOSITORY", "")
        server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
        if repo:
            repo_url = f"{server}/{repo}"
        run_num = os.environ.get("GITHUB_RUN_NUMBER", "")
        build_type = f"CI (GitHub Actions #{run_num})" if run_num else "CI (GitHub Actions)"
        return commit, 0, repo_url, build_type

    # Local environment: query git
    try:
        commit = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL
        ).decode("utf-8", errors="ignore").strip()
    except Exception:
        commit = None

    try:
        status = subprocess.check_output(
            ["git", "status", "--porcelain"],
            stderr=subprocess.DEVNULL
        ).decode("utf-8", errors="ignore").strip()
        if status:
            lines = [l for l in status.splitlines() if l.strip()]
            dirty_count = len(lines)
    except Exception:
        dirty_count = 0

    try:
        url = subprocess.check_output(
            ["git", "config", "--get", "remote.origin.url"],
            stderr=subprocess.DEVNULL
        ).decode("utf-8", errors="ignore").strip()
        if url:
            if url.startswith("git@github.com:"):
                url = "https://github.com/" + url[len("git@github.com:"):]
            elif url.startswith("git@gitlab.com:"):
                url = "https://gitlab.com/" + url[len("git@gitlab.com:"):]
            if url.endswith(".git"):
                url = url[:-4]
            repo_url = url
    except Exception:
        repo_url = None

    if not commit:
        build_type = "Source Archive Build"

    return commit, dirty_count, repo_url, build_type

def main():
    commit, dirty_count, repo_url, build_type = get_git_info()

    if not commit:
        commit_str = "custom"
    elif os.environ.get("GITHUB_ACTIONS") == "true":
        commit_str = commit
    elif dirty_count > 0:
        commit_str = f"{commit} (+{dirty_count} uncommitted changes)"
    else:
        commit_str = f"{commit} (clean)"

    build_date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    repo_url_str = repo_url if repo_url else ""

    if len(sys.argv) < 2:
        print(commit_str)
        return

    output_path = sys.argv[1]

    lines = [
        "/*",
        " * Auto-generated version and build metadata",
        " */",
        '#include "version.h"',
        '#include <obs-module.h>',
        "",
        f'const char *obs_gstreamer_version = "{commit_str}";',
        f'const char *obs_gstreamer_commit = "{commit_str}";',
        f'const char *obs_gstreamer_build_date = "{build_date}";',
        f'const char *obs_gstreamer_repo_url = "{repo_url_str}";',
        f'const char *obs_gstreamer_build_type = "{build_type}";',
        "",
        "void obs_gstreamer_log_build_info(void)",
        "{",
        '\tblog(LOG_INFO, "[obs-gstreamer] Loading plugin (version: %s)", obs_gstreamer_version);',
        '\tblog(LOG_INFO, "[obs-gstreamer]   Build Date: %s (%s)", obs_gstreamer_build_date, obs_gstreamer_build_type);',
        '\tif (obs_gstreamer_repo_url && obs_gstreamer_repo_url[0])',
        '\t\tblog(LOG_INFO, "[obs-gstreamer]   Repository: %s", obs_gstreamer_repo_url);',
        "}",
        ""
    ]
    new_content = "\n".join(lines)

    if os.path.exists(output_path):
        try:
            with open(output_path, "r", encoding="utf-8") as f:
                old_content = f.read()
            if old_content == new_content:
                return
        except Exception:
            pass

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(new_content)

if __name__ == "__main__":
    main()
