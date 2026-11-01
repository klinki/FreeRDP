#!/usr/bin/env python3
"""Stage and verify relocatable macOS FreeRDP runtimes; collect matching sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile


SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")
MACHO_MAGICS = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}
OPENSSL_CONFIG = """# Use only providers shipped inside this app.
openssl_conf = openssl_init
[openssl_init]
providers = provider_sect
[provider_sect]
default = default_sect
legacy = legacy_sect
[default_sect]
activate = 1
[legacy_sect]
activate = 1
"""


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], text=True, **kwargs).strip()


def system_path(path):
    return os.path.normpath(path).startswith(SYSTEM_PREFIXES)


def version(value):
    parts = tuple(int(part) for part in value.split("."))
    return parts + (0,) * (3 - len(parts))


def macho(path):
    with path.open("rb") as stream:
        return stream.read(4) in MACHO_MAGICS


def inspect(path, target, allow_universal=False):
    architectures = run("lipo", "-archs", path).split()
    universal = allow_universal and "arm64" in architectures and set(architectures) <= {"arm64", "x86_64"}
    if architectures != ["arm64"] and not universal:
        raise ValueError(f"{path}: expected arm64 only, found {architectures}")
    load = run("otool", "-arch", "arm64", "-l", path)
    platforms = re.findall(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (\S+)", load)
    if any(platform not in ("1", "MACOS", "macos") for platform in platforms):
        raise ValueError(f"{path}: expected macOS platform, found {platforms}")
    minimums = re.findall(r"^\s*minos ([0-9.]+)", load, re.M)
    minimums += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", load)
    if not minimums or any(version(item) > version(target) for item in minimums):
        raise ValueError(f"{path}: incompatible minimum macOS {minimums}, target {target}")
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", load)
    identifiers = re.findall(r"cmd LC_ID_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset", load)
    dependencies = []
    for line in run("otool", "-arch", "arm64", "-L", path).splitlines()[1:]:
        name = line.strip().split(" (compatibility version", 1)[0]
        if name and name not in identifiers:
            dependencies.append(name)
    return dict(architectures=architectures, minimum_macos=minimums[0],
                rpaths=rpaths, dependencies=dependencies)


def expand(path, loader, executable):
    return path.replace("@loader_path", str(loader.parent)).replace("@executable_path", str(executable.parent))


def resolve(name, loader, executable, rpaths, preferred=None):
    # Build-tree FreeRDP/WinPR must win over preinstalled copies with the same ABI name.
    if preferred and Path(name).name in preferred:
        return preferred[Path(name).name]
    if system_path(name):
        return None
    if name.startswith("@rpath/"):
        candidates = [Path(expand(item, loader, executable)) / name[7:] for item in rpaths]
    else:
        candidates = [Path(expand(name, loader, executable))]
    for candidate in candidates:
        if system_path(str(candidate)):
            return None  # System libraries may exist only in the dyld shared cache.
        if candidate.is_absolute() and candidate.is_file():
            return candidate.resolve()
    raise ValueError(f"{loader}: unresolved dependency {name}")


def build_libraries(build):
    result = {}
    for path in build.rglob("*.dylib"):
        if not path.name.startswith(("libfreerdp", "libwinpr")):
            continue
        real = path.resolve()
        for name in (path.name, real.name):
            if name in result and result[name] != real:
                raise ValueError(f"Ambiguous build-tree library {name}")
            result[name] = real
    return result


def sign(contents):
    for path in sorted(contents.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        if path.is_file() and not path.is_symlink() and macho(path):
            subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True)


def stage(binary, build, source, output, target):
    output.mkdir(parents=True, exist_ok=False)
    contents = output / "Contents"
    helpers, frameworks, resources = (contents / name for name in ("Helpers", "Frameworks", "Resources"))
    for folder in (helpers, frameworks, resources):
        folder.mkdir(parents=True)
    preferred = build_libraries(build)
    destinations, names, edges, metadata = {}, {}, {}, {}

    def register(path, destination):
        path = path.resolve()
        if destination in names and names[destination] != path:
            raise ValueError(f"Library filename collision: {destination}: {names[destination]} and {path}")
        if path not in destinations:
            destinations[path] = destination
            names[destination] = path
        return path

    root = register(binary, helpers / binary.name)
    pending = [(root, [])]
    # OpenSSL's legacy provider is dlopen'd, so seed it explicitly.
    providers_added = False
    while pending:
        path, inherited = pending.pop()
        if path in metadata:
            continue
        info = inspect(path, target)
        metadata[path] = info
        rpaths = [expand(item, path, binary) for item in info["rpaths"]] + inherited
        edges[path] = {}
        for dependency in info["dependencies"]:
            library = resolve(dependency, path, binary, rpaths, preferred)
            if library is not None:
                register(library, frameworks / library.name)
                edges[path][dependency] = library
                pending.append((library, rpaths))
                if library.name.startswith("libcrypto.3") and not providers_added:
                    provider = library.parent / "ossl-modules/legacy.dylib"
                    if not provider.is_file():
                        raise ValueError(f"Required OpenSSL legacy provider is missing: {provider}")
                    register(provider, frameworks / "ossl-modules/legacy.dylib")
                    pending.append((provider.resolve(), rpaths))
                    providers_added = True

    records = []
    for original, destination in destinations.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination)
        destination.chmod(destination.stat().st_mode | 0o200)
        changes = []
        if destination != destinations[root]:
            changes += ["-id", "@rpath/" + str(destination.relative_to(frameworks))]
        for dependency, library in edges[original].items():
            relative = os.path.relpath(destinations[library], destination.parent)
            changes += ["-change", dependency, "@loader_path/" + relative]
        for rpath in dict.fromkeys(metadata[original]["rpaths"]):
            changes += ["-delete_rpath", rpath]
        if changes:
            subprocess.run(["install_name_tool", *changes, str(destination)], check=True)
        records.append(dict(path=str(destination.relative_to(contents)),
                            source=str(original), source_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
                            architectures=metadata[original]["architectures"],
                            minimum_macos=metadata[original]["minimum_macos"]))
    (resources / "openssl.cnf").write_text(OPENSSL_CONFIG)
    if binary.name == "sfreerdp-server":
        # The sample server also searches its working directory for this fixture image.
        shutil.copy2(source / "server/Sample/test_icon.bmp", resources / "test_icon.bmp")
    licenses = resources / "Licenses/FreeRDP"
    licenses.mkdir(parents=True)
    for name in ("LICENSE", "NOTICE"):
        if (source / name).is_file():
            shutil.copy2(source / name, licenses / name)
    manifest = dict(engine_repository=run("git", "-C", source, "config", "--get", "remote.origin.url"),
                    engine_revision=run("git", "-C", source, "rev-parse", "HEAD"),
                    minimum_macos=target, architecture="arm64", binaries=records)
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        manifest["engine_build_options"] = {
            match.group(1): match.group(2)
            for line in cache.read_text().splitlines()
            if (match := re.match(r"((?:WITH_|CHANNEL_|BUILD_|CMAKE_OSX_|WINPR_HAVE_PIPE2)[A-Z0-9_]*):[^=]+=(.*)", line))
        }
    (resources / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    sign(contents)
    verify(output, target)


def verify(bundle, target):
    contents = (bundle / "Contents").resolve()
    if not contents.is_dir():
        raise ValueError(f"Missing Contents directory: {bundle}")
    binaries = []
    for path in contents.rglob("*"):
        if path.is_symlink() and not path.resolve().is_relative_to(contents):
            raise ValueError(f"Symlink escapes bundle: {path}")
        if path.is_file() and not path.is_symlink() and macho(path):
            binaries.append(path)
    if not binaries:
        raise ValueError("Bundle contains no Mach-O binaries")
    info_plist = contents / "Info.plist"
    bundle_info = None
    if info_plist.exists():
        import plistlib
        bundle_info = plistlib.loads(info_plist.read_bytes())
        executable_name = bundle_info.get("CFBundleExecutable")
        if not isinstance(executable_name, str) or Path(executable_name).name != executable_name:
            raise ValueError("App must declare a valid CFBundleExecutable")
        executable = contents / "MacOS" / executable_name
        if not executable.is_file():
            raise ValueError(f"Missing app executable: {executable}")
    else:
        executable = next(iter(sorted((contents / "Helpers").iterdir())))
    root_info = inspect(executable, target)
    root_rpaths = [expand(item, executable, executable) for item in root_info["rpaths"]]
    for path in binaries:
        # Preserve Sparkle's signed universal helpers. The app and FreeRDP remain arm64-only.
        sparkle = contents / "Frameworks/Sparkle.framework"
        info = inspect(path, target, allow_universal=path.is_relative_to(sparkle))
        for rpath in info["rpaths"]:
            if not rpath.startswith(("@loader_path", "@executable_path")) and not system_path(rpath):
                raise ValueError(f"{path}: nonrelocatable rpath {rpath}")
        rpaths = [expand(item, path, executable) for item in info["rpaths"]] + root_rpaths
        for dependency in info["dependencies"]:
            if system_path(dependency):
                continue
            if not dependency.startswith(("@loader_path/", "@executable_path/", "@rpath/")):
                raise ValueError(f"{path}: external dependency {dependency}")
            library = resolve(dependency, path, executable, rpaths)
            if library is not None and not library.is_relative_to(contents):
                raise ValueError(f"{path}: dependency escapes bundle: {dependency}")
        subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True)
    if bundle_info is not None:
        if bundle_info.get("LSMinimumSystemVersion") != target:
            raise ValueError(f"App minimum macOS must be stamped as {target}")
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=True)
    print(f"Verified {len(binaries)} binaries supporting arm64 for macOS {target}: {bundle}")


def license_name(name):
    return bool(re.match(r"(?i)^(licen[cs]e|copying|copyright|notice|authors|ftl|gplv?\d*|lgplv?\d*)([._-].*)?$", Path(name).name))


def copy_license(name, data, destination):
    path = Path(name)
    if path.is_absolute() or ".." in path.parts or len(data) > 2_000_000:
        raise ValueError(f"Unsafe license entry {name}")
    output = destination / path
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)


def extract_licenses(archive, destination):
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as package:
            for entry in package.infolist():
                if not entry.is_dir() and license_name(entry.filename):
                    copy_license(entry.filename, package.read(entry), destination)
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as package:
            for entry in package:
                if entry.isfile() and license_name(entry.name):
                    if entry.size > 2_000_000:
                        raise ValueError(f"Oversized license entry {entry.name}")
                    copy_license(entry.name, package.extractfile(entry).read(), destination)


def retain_downloads(cache, destination, info):
    """Keep source downloads, including Git formulae, without API caches or Git history."""
    repositories = []
    for head in cache.rglob("HEAD"):
        if head.parent.name == ".git":
            repositories.append(head.parent.parent)
        elif (head.parent / "objects").is_dir():
            repositories.append(head.parent)
    for index, repository in enumerate(repositories):
        revision = info["urls"]["stable"].get("revision") if len(repositories) == 1 else None
        revision = revision or "HEAD"
        resolved = run("git", "-C", repository, "rev-parse", revision)
        subprocess.run(["git", "-C", str(repository), "archive", "--format=tar",
                        "-o", str(destination / f"git-source-{index}.tar"), resolved], check=True)
        (destination / f"git-source-{index}.revision").write_text(resolved + "\n")
    for path in cache.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(cache)
        if "api" in relative.parts or ".git" in relative.parts or path.name.endswith(".jws.json"):
            continue
        if any(path.is_relative_to(repository) for repository in repositories):
            continue
        target = destination / "downloads" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def collect_sources(runtime, source, output):
    """Keep formula recipes, patches/resources fetched by brew, receipts and exact sources."""
    manifest_path = runtime / "Contents/Resources/runtime-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if run("git", "-C", source, "rev-parse", "HEAD") != manifest["engine_revision"]:
        raise ValueError("Engine source revision does not match the bundled runtime")
    if run("git", "-C", source, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Engine source has uncommitted changes")
    cellar = Path(run("brew", "--cellar")).resolve()
    formula_versions = {}
    for binary in manifest["binaries"]:
        path = Path(binary["source"])
        if path.is_relative_to(cellar):
            formula, installed_version = path.relative_to(cellar).parts[:2]
            if formula in formula_versions and formula_versions[formula] != installed_version:
                raise ValueError(f"Multiple versions of {formula} in runtime")
            formula_versions[formula] = installed_version
        elif not path.is_relative_to(source) and not path.name.startswith(("libfreerdp", "libwinpr", "sdl-freerdp")):
            raise ValueError(f"Cannot identify corresponding sources for {path}")
    licenses = runtime / "Contents/Resources/Licenses"
    notices = ["Sheltie bundled runtime", "",
               f"FreeRDP source: {manifest['engine_repository']}", f"Revision: {manifest['engine_revision']}", ""]
    with tempfile.TemporaryDirectory(prefix="runtime-sources-", dir=output.parent) as temporary:
        sources = Path(temporary)
        subprocess.run(["git", "-C", str(source), "archive", "--format=tar",
                        "-o", str(sources / "FreeRDP.tar"), manifest["engine_revision"]], check=True)
        dependencies = []
        for formula, installed_version in sorted(formula_versions.items()):
            installed = cellar / formula / installed_version
            recipe = installed / ".brew" / f"{formula}.rb"
            info = json.loads(run("brew", "info", "--json=v2", "--formula", recipe))["formulae"][0]
            stable = info["versions"]["stable"]
            expected = stable + (f"_{info['revision']}" if info["revision"] else "")
            if installed_version != expected:
                raise ValueError(f"{formula}: installed {installed_version}, saved source recipe {expected}; rebuild with matching dependencies")
            folder = sources / "Homebrew" / formula
            folder.mkdir(parents=True)
            shutil.copy2(recipe, folder / f"{formula}.rb")
            shutil.copy2(installed / "INSTALL_RECEIPT.json", folder / "INSTALL_RECEIPT.json")
            (folder / "metadata.json").write_text(json.dumps(info, indent=2) + "\n")
            cache = output.parent.parent / "runtime-source-cache" / formula / installed_version
            environment = dict(os.environ, HOMEBREW_CACHE=str(cache), HOMEBREW_NO_AUTO_UPDATE="1")
            subprocess.run(["brew", "fetch", "--build-from-source", "--formula", str(recipe)],
                           env=environment, check=True)
            retain_downloads(cache, folder, info)
            archives = [path for path in folder.rglob("*") if path.is_file() and not path.is_symlink()]
            if not archives:
                raise ValueError(f"No corresponding source downloads for {formula}")
            for archive in archives:
                extract_licenses(archive, licenses / formula)
            # Also retain notices installed by the formula.
            for path in installed.iterdir():
                if path.is_file() and license_name(path.name):
                    (licenses / formula).mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, licenses / formula / path.name)
            if not (licenses / formula).exists():
                raise ValueError(f"No license texts found in sources of {formula}")
            dependencies.append(dict(formula=formula, version=installed_version, license=info["license"],
                                     source_url=info["urls"]["stable"]["url"]))
            notices += [f"{formula} {installed_version}", f"License: {info['license']}",
                        f"Source: {info['urls']['stable']['url']}", ""]
        manifest["dependencies"] = dependencies
        (sources / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        recipes = sources / "launcher-build-recipes"
        recipes.mkdir()
        for name in ("build-engine.sh", "macos_bundle.py"):
            shutil.copy2(Path(__file__).parent / name, recipes / name)
        (sources / "README.txt").write_text(
            "Matching FreeRDP and Homebrew runtime sources, formula recipes, patches/resources and build receipts.\n"
            "FreeRDP.tar is the exact resolved source revision. Homebrew/* contains source downloads and Git source snapshots.\n"
            "scripts/sheltie/build-engine.sh and scripts/sheltie/macos_bundle.py in FreeRDP.tar describe the build.\n")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(output, "w:gz") as archive:
            archive.add(sources, arcname="runtime-sources")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    (licenses / "THIRD-PARTY-NOTICES.txt").write_text("\n".join(notices) +
        "\nMatching source code and build recipes accompany this download as runtime-sources.tar.gz.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    staging = commands.add_parser("stage")
    for name in ("binary", "engine-build", "source", "output"):
        staging.add_argument("--" + name, type=Path, required=True)
    staging.add_argument("--target", default="26.0")
    verification = commands.add_parser("verify")
    verification.add_argument("bundle", type=Path)
    verification.add_argument("--target", default="26.0")
    sources = commands.add_parser("sources")
    for name in ("runtime", "source", "output"):
        sources.add_argument("--" + name, type=Path, required=True)
    options = parser.parse_args()
    try:
        if options.command == "stage":
            stage(options.binary.resolve(), options.engine_build.resolve(), options.source.resolve(),
                  options.output.resolve(), options.target)
        elif options.command == "verify":
            verify(options.bundle.resolve(), options.target)
        else:
            options.output.parent.mkdir(parents=True, exist_ok=True)
            collect_sources(options.runtime.resolve(), options.source.resolve(), options.output.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
