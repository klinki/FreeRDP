#!/usr/bin/env python3
"""Complete the Sheltie runtime manifest and check release tags against the fork version."""
import argparse
import json
from pathlib import Path
import re
import subprocess

MANIFEST = Path("Contents/Resources/runtime-manifest.json")
CLIENT = Path("Contents/Helpers/sdl-freerdp")
RELEASE_TAG = re.compile(r"v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?-sheltie\.[1-9]\d*")


def query_capabilities(client):
    output = subprocess.check_output([str(client), "/launcher-capabilities"], text=True, timeout=60)
    value = json.loads(output)
    capabilities = value.get("capabilities")
    if (not isinstance(capabilities, list) or not capabilities
            or not all(isinstance(item, str) and item for item in capabilities)):
        raise ValueError("The client reported no launcher capabilities")
    for key in ("schemaVersion", "bridgeProtocolVersion"):
        if type(value.get(key)) is not int:
            raise ValueError(f"The client reported an invalid {key}")
    return dict(schema_version=value["schemaVersion"], protocol_version=value["bridgeProtocolVersion"],
                capabilities=sorted(set(capabilities)))


def check_release(version, tag):
    """Require a pushed tag to be the exact fork release the version script derives."""
    if not RELEASE_TAG.fullmatch(tag):
        raise ValueError(f"{tag} is not a fork release tag such as v3.32.2-sheltie.1")
    if version.get("release_tag") != tag:
        raise ValueError(f"{tag} is not the fork release at this commit; "
                         f"sheltie-version.py derives {version.get('release_tag') or version.get('version')}")
    if version.get("dirty"):
        raise ValueError("Release builds require a clean checkout")
    if not tag.startswith(f"v{version['upstream_version']}-sheltie."):
        raise ValueError(f"{tag} does not match upstream base {version['upstream_version']}")


def complete(runtime, version, tag, query=query_capabilities):
    path = runtime / MANIFEST
    manifest = json.loads(path.read_text())
    if manifest.get("engine_revision") != version["revision"]:
        raise ValueError("The runtime was not built from the versioned revision")
    if tag:
        check_release(version, tag)
    manifest.update(engine_version=version["version"], release_tag=tag or None,
                    upstream_version=version["upstream_version"], upstream_base=version["upstream_base"],
                    bridge=query(runtime / CLIENT))
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    completion = commands.add_parser("complete", help="add version and bridge metadata to a staged runtime")
    completion.add_argument("--runtime", type=Path, required=True)
    completion.add_argument("--version-json", type=Path, required=True,
                            help="output of scripts/sheltie-version.py --json")
    completion.add_argument("--tag", default="", help="release tag being published, if any")
    completion.add_argument("--output", type=Path, required=True, help="copy of the completed manifest")
    release = commands.add_parser("check-release", help="validate a fork release tag")
    release.add_argument("--version-json", type=Path, required=True)
    release.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        version = json.loads(args.version_json.read_text())
        if args.command == "check-release":
            check_release(version, args.tag)
            print(f"{args.tag}: release check passed")
        else:
            manifest = complete(args.runtime, version, args.tag)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(manifest, indent=2) + "\n")
            print(f"{manifest['engine_version']}: {len(manifest['bridge']['capabilities'])} bridge capabilities")
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Release manifest error: {error}\n")


if __name__ == "__main__":
    main()
