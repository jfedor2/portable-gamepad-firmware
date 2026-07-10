#!/usr/bin/env python3

import json
import sys

with open(sys.argv[1], "r") as f:
    builds = json.load(f)

sdks = set()

for b in builds:
    sdks.add(b["sdk"])

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

for sdk in sdks:
    print(
        f"""mkdir -p "${{SDK_PATH_PREFIX}}"/{sdk}/manifest
cp west-{sdk}.yml "${{SDK_PATH_PREFIX}}"/{sdk}/manifest/west.yml
west init -l "${{SDK_PATH_PREFIX}}"/{sdk}/manifest
ZEPHYR_BASE=`realpath "${{SDK_PATH_PREFIX}}"/{sdk}/zephyr` west update -o=--depth=1 -n
"""
    )

for b in builds:
    prefix = "#" if b.get("disabled", False) else ""
    extra_params = " ".join(
        f'-D{param}="{" ".join(values)}"' for param, values in b["extra_params"].items()
    )
    print(
        f'{prefix}ZEPHYR_BASE=`realpath "${{SDK_PATH_PREFIX}}"/{b["sdk"]}/zephyr` west build -p -d "${{BUILD_PATH_PREFIX}}"/{b["name"]} -b {b["board"]} app -- -DBOARD_ROOT="${{PWD}}"/app {extra_params}'
    )
    print(
        f'{prefix}cp "${{BUILD_PATH_PREFIX}}"/{b["name"]}/{b["artifact_built_name"]} "${{ARTIFACTS_PATH}}"/pgf-{b["name"]}.{b["artifact_built_name"].split(".")[-1]}'
    )
    print()
