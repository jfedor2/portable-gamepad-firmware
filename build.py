#!/usr/bin/env python3

import argparse
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("builds_file")
    parser.add_argument("--sdk", help="only set up and build for this SDK")
    parser.add_argument("--name", help="only set up and build this build")
    parser.add_argument(
        "--no-setup", action="store_true", help="skip SDK setup commands"
    )
    parser.add_argument(
        "--setup-only",
        action="store_true",
        help="only emit SDK setup commands, skip build commands",
    )
    args = parser.parse_args()

    if args.no_setup and args.setup_only:
        parser.error("--no-setup and --setup-only are mutually exclusive")

    with open(args.builds_file, "r") as f:
        builds = json.load(f)

    if args.name:
        builds = [b for b in builds if b["name"] == args.name]
        if not builds:
            parser.error(f"no build named {args.name!r}")

    if args.sdk:
        builds = [b for b in builds if b["sdk"] == args.sdk]
        if not builds:
            parser.error(f"no builds using SDK {args.sdk!r}")

    sdks = set()
    extra_setup_by_sdk = {}

    for b in builds:
        sdks.add(b["sdk"])
        for extra_setup in b.get("extra_setup", []):
            if extra_setup not in extra_setup_by_sdk.setdefault(b["sdk"], []):
                extra_setup_by_sdk[b["sdk"]].append(extra_setup)

    print(
        """#!/bin/bash

set -x -e -u -o pipefail

if [ -z "${SDK_PATH_PREFIX:-}" ]; then
    SDK_PATH_PREFIX=./sdk
fi

if [ -z "${BUILD_PATH_PREFIX:-}" ]; then
    BUILD_PATH_PREFIX=./build
fi

if [ -z "${ARTIFACTS_PATH:-}" ]; then
    ARTIFACTS_PATH=./artifacts
fi

mkdir -p "${BUILD_PATH_PREFIX}"

mkdir -p "${ARTIFACTS_PATH}"
"""
    )

    if not args.no_setup:
        for sdk in sdks:
            print(
                f"""mkdir -p "${{SDK_PATH_PREFIX}}"/{sdk}/manifest
cp west-{sdk}.yml "${{SDK_PATH_PREFIX}}"/{sdk}/manifest/west.yml
west init -l "${{SDK_PATH_PREFIX}}"/{sdk}/manifest
ZEPHYR_BASE=`realpath "${{SDK_PATH_PREFIX}}"/{sdk}/zephyr` west update -o=--depth=1 -n
"""
            )
            for extra_setup in extra_setup_by_sdk.get(sdk, []):
                print(
                    f'ZEPHYR_BASE=`realpath "${{SDK_PATH_PREFIX}}"/{sdk}/zephyr` {extra_setup}'
                )
            if extra_setup_by_sdk.get(sdk):
                print()

    if not args.setup_only:
        for b in builds:
            prefix = "#" if b.get("disabled", False) and not args.name else ""
            extra_params = " ".join(
                f'-D{param}="{" ".join(values)}"'
                for param, values in b["extra_params"].items()
            )
            print(
                f'{prefix}ZEPHYR_BASE=`realpath "${{SDK_PATH_PREFIX}}"/{b["sdk"]}/zephyr` west build -p -d "${{BUILD_PATH_PREFIX}}"/{b["name"]} -b {b["board"]} app -- -DBOARD_ROOT="${{PWD}}"/app {extra_params}'
            )
            print(
                f'{prefix}cp "${{BUILD_PATH_PREFIX}}"/{b["name"]}/{b["artifact_built_name"]} "${{ARTIFACTS_PATH}}"/pgf-{b["name"]}.{b["artifact_built_name"].split(".")[-1]}'
            )
            print()


if __name__ == "__main__":
    main()
