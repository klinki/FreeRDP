"""Test selection and the small, audited per-case configuration differences."""

from dataclasses import dataclass
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent
PIN = json.loads((ROOT / "suite.json").read_text())


@dataclass(frozen=True)
class CaseOptions:
    rdp_version: str = "10.6"
    redirect_drive: bool = False
    video: bool = False
    graphics: bool = False

    def __post_init__(self):
        if self.rdp_version not in ("10.6", "10.7", "10.8"):
            raise ValueError(f"Unsupported test RDP version: {self.rdp_version}")
        for name in ("redirect_drive", "video", "graphics"):
            if type(getattr(self, name)) is not bool:
                raise ValueError(f"Case option {name} must be a boolean")


def profile_cases(profile, manifest=PIN, ancestors=()):
    if profile in ancestors:
        raise ValueError(f"Profile include cycle: {' -> '.join((*ancestors, profile))}")
    if profile not in manifest["profiles"]:
        raise ValueError(f"Unknown test profile: {profile}")
    definition = manifest["profiles"][profile]
    cases = []
    for included in definition.get("include", []):
        cases.extend(profile_cases(included, manifest, (*ancestors, profile)))
    cases.extend(definition["cases"])
    if not cases or any(not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", case) for case in cases):
        raise ValueError(f"Profile {profile} must contain valid test method names")
    if len(cases) != len(set(cases)):
        raise ValueError(f"Profile {profile} contains duplicate test cases")
    return cases


def select_cases(profile, requested=None, manifest=PIN):
    available = profile_cases(profile, manifest)
    if requested is None:
        return available
    if not requested or len(requested) != len(set(requested)):
        raise ValueError("Select at least one test, without duplicate --case arguments")
    for case in requested:
        if case not in available:
            raise ValueError(f"Test {case} is not in profile {profile}; use --list to inspect the profile")
    return list(requested)


def options_for(case, manifest=PIN):
    try:
        return CaseOptions(**manifest.get("case_options", {}).get(case, {}))
    except TypeError as error:
        raise ValueError(f"Invalid options for {case}: {error}") from error


def graphics_compatibility_mask(buildconfig):
    # capList in channels/rdpgfx/client/rdpgfx_main.c has the legacy 10.6
    # errata alias (0x000A0601) at bit 9 in the audited build. AV1 inserts an
    # entry before it; Azure adds more capabilities the Microsoft suite cannot
    # validate. Fail explicitly for other layouts instead of guessing a mask.
    for name in ("WITH_GFX_AV1", "WITH_GFX_AZURE"):
        values = re.findall(rf"\b{name}=(ON|OFF)\b", buildconfig)
        if values != ["OFF"]:
            raise ValueError(
                f"Graphics compatibility tests require {name}=OFF in /buildconfig; "
                "select non-graphics cases or use the audited build configuration"
            )
    return 0x200
