"""Build the shader translator: build/xlate/nx2navi_xlate.exe (vendored translator 1.3.2 + patches, NativeAOT, SPIR-V for Vulkan).

Copies tools/vendor/ryujinx to build/xlate/ryujinx, applies tools/xlate/patches/*.patch there (tools/vendor stays
byte-identical), then runs `dotnet publish`. Needs the .NET 10 SDK, the MSVC build
tools and vswhere.exe (docs/building.md). Run with .venv. On Linux it builds build/xlate/nx2navi_xlate for linux-x64, which
NativeAOT links with the system's clang (no MSVC); NativeAOT builds for the OS it runs on only.

Next to the exe it writes nx2navi_xlate.id: the translator identity, a sha256 over everything the build reads (this script,
Program.cs, the csproj, the patches, the vendor tree). runtime/gfx_shader.c puts it into every shader cache key, so a
translator change re-keys the cache without a hand-made KEY_VERSION bump. --id-only writes it for the exe already built
(the caller vouches that the exe matches the sources).

  python tools/build_xlate.py [--id-only] [--out DIR]

--out builds into DIR instead of build/xlate (a task build dir: nx2navi.exe takes the translator next to it).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "tools" / "vendor" / "ryujinx"
MNG_VENDOR = ROOT / "refs" / "MacNativeGames" / "vendor" / "ryujinx"
XLATE = ROOT / "tools" / "xlate"
BUILD = ROOT / "build" / "xlate"
WIN = os.name == "nt"
EXE = "nx2navi_xlate.exe" if WIN else "nx2navi_xlate"
VSWHERE_DIR = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"


def tree_hashes(base: Path) -> dict[str, str]:
    """Hash every source file; src/*/obj and src/*/bin are build output (an IDE restore writes them, the vendor
    .gitignore ignores them and prepare_sources() drops them), so they are left out."""
    def build_output(rel: Path) -> bool:
        return len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")

    return {
        p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(base.rglob("*"))
        if p.is_file() and not build_output(p.relative_to(base))
    }


def mng_pin() -> str:
    """The MacNativeGames commit tools/fetch_refs.sh pins."""
    for line in (ROOT / "tools" / "fetch_refs.sh").read_text(encoding="utf-8").splitlines():
        f = line.split()
        if len(f) >= 4 and f[:2] == ["fetch", "MacNativeGames"]:   # a fifth word asks for the history
            return f[3]
    sys.exit("tools/fetch_refs.sh pins no MacNativeGames commit")


def pinned_blobs() -> dict[str, str]:
    """Git blob ids of vendor/ryujinx in the refs/ checkout at the pinned commit. The checkout itself may be newer than the pin or
    carry CRLF line ends (core.autocrlf), so its files are not compared."""
    out = subprocess.run(["git", "-C", str(MNG_VENDOR.parent.parent), "ls-tree", "-r", mng_pin(), "vendor/ryujinx"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"git ls-tree of the MacNativeGames pin failed: {out.stderr.strip()}")
    blobs = {}
    for line in out.stdout.splitlines():
        meta, path = line.split("\t", 1)
        blobs[path.removeprefix("vendor/ryujinx/")] = meta.split()[2]
    return blobs


def blob_ids(base: Path) -> dict[str, str]:
    """Git blob ids of the source files tree_hashes() hashes."""
    ids = {}
    for rel in tree_hashes(base):
        data = (base / rel).read_bytes()
        ids[rel] = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
    return ids


def verify_vendor() -> None:
    """tools/vendor/ryujinx must be byte-identical to the refs/ checkout's copy at the pinned commit (when present)."""
    if not MNG_VENDOR.is_dir():
        print("refs/MacNativeGames missing: vendor comparison skipped")
        return
    ours, theirs = blob_ids(VENDOR), pinned_blobs()
    if ours != theirs:
        diff = sorted(k for k in ours.keys() | theirs.keys() if ours.get(k) != theirs.get(k))
        sys.exit(f"tools/vendor/ryujinx differs from MNG in {len(diff)} files, first: {diff[:5]}")
    print(f"vendor identical to MNG: {len(ours)} files")


def prepare_sources() -> Path:
    src = BUILD / "ryujinx"
    if BUILD.exists():
        shutil.rmtree(BUILD / "ryujinx", ignore_errors=True)
    shutil.copytree(VENDOR, src)
    for junk in list(src.glob("src/*/obj")) + list(src.glob("src/*/bin")):
        shutil.rmtree(junk)
    patches = sorted((XLATE / "patches").glob("*.patch"))
    if len(patches) != 40:
        sys.exit(f"expected 40 patches, found {len(patches)}")
    # build/xlate is inside this checkout, and git inside a repository reads a "diff --git" patch's paths from the
    # repository's top: those hunks fall outside the directory handed to --directory, git skips every file of the patch
    # and still exits 0. GIT_CEILING_DIRECTORIES stops git from walking up to the checkout, so it reads each patch as it
    # would outside a repository, and a "Skipped patch" line fails the build (including 0021's PRMT implementation).
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(BUILD.parent), LC_ALL="C")
    for patch in patches:
        for extra in (["--check"], []):
            r = subprocess.run(
                ["git", "apply", *extra, "--verbose", "--whitespace=nowarn",
                 "--directory", str(src.relative_to(BUILD)), str(patch)],
                cwd=BUILD, capture_output=True, text=True, env=env,
            )
            skipped = "Skipped patch" in r.stdout or "Skipped patch" in r.stderr
            if r.returncode != 0 or skipped:
                sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        print(f"applied {patch.name}")
    return src


# What the identity covers besides the sources: the publish settings in main(). Change it with them.
BUILD_SETTINGS = "PublishAot=true InvariantGlobalization=true Release"


def strip_code(text: str, xml: bool = False) -> str:
    """C# (or MSBuild XML) without its comments, trailing spaces and blank lines, string and character literals kept:
    what the compiler sees, so that a comment-only edit keeps the identity (and so every shader cache key)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if xml:
            if text.startswith("<!--", i):
                j = text.find("-->", i + 4)
                i = n if j < 0 else j + 3
                continue
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        elif c in "\"'" or text.startswith('@"', i):
            verbatim = text.startswith('@"', i)
            q = '"' if verbatim else c
            j = i + (2 if verbatim else 1)
            while j < n:
                if verbatim and text.startswith('""', j):
                    j += 2
                elif not verbatim and text[j] == "\\":
                    j += 2
                elif text[j] == q:
                    j += 1
                    break
                else:
                    j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    lines = (line.rstrip() for line in "".join(out).splitlines())
    return "\n".join(line for line in lines if line)


def code_digest(path: Path) -> str:
    """sha256 of a file, comments left out of C# and MSBuild files (strip_code)."""
    data = path.read_bytes()
    if path.suffix in (".cs", ".csproj", ".props", ".targets"):
        data = strip_code(data.decode("utf-8-sig", errors="replace"), xml=path.suffix != ".cs").encode()
    return hashlib.sha256(data).hexdigest()


def identity() -> str:
    """sha256 over what the translator is built from, comments left out: the patched tree as the build compiles it
    (build/xlate/ryujinx from prepare_sources), tools/xlate's own sources and the publish settings. Without a prepared
    tree, the vendor tree and the patch files as they are."""
    h = hashlib.sha256()
    h.update(f"settings\0{BUILD_SETTINGS}\n".encode())
    for name in ("Program.cs", "AmpereLowering.cs", "MeshSpirv.cs", "nx2navi_xlate.csproj"):
        h.update(f"{name}\0{code_digest(XLATE / name)}\n".encode())
    tree = BUILD / "ryujinx"
    if (tree / "src").is_dir():
        for f in sorted(tree.rglob("*")):
            rel = f.relative_to(tree)
            if f.is_file() and not (len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")):
                h.update(f"tree/{rel.as_posix()}\0{code_digest(f)}\n".encode())
    else:
        for f in sorted((XLATE / "patches").glob("*.patch")):
            h.update(f"{f.name}\0{hashlib.sha256(f.read_bytes()).hexdigest()}\n".encode())
        for rel, digest in tree_hashes(VENDOR).items():
            h.update(f"vendor/{rel}\0{digest}\n".encode())
    return h.hexdigest()


def write_identity(exe: Path) -> None:
    ident = identity()
    exe.with_suffix(".id").write_text(ident + "\n", encoding="ascii")
    print(f"translator identity {ident[:16]} -> {exe.with_suffix('.id')}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--id-only", action="store_true", help="write nx2navi_xlate.id for the exe already built")
    parser.add_argument("--out", type=Path, help="build directory instead of build/xlate")
    parser.add_argument("--llvm", type=Path, help="Linux: an LLVM directory whose bin/clang and lld link the exe")
    parser.add_argument("--sysroot", type=Path, help="Linux: link against this sysroot's glibc instead of the host's "
                                                     "(tools/make_release_linux.py)")
    a = parser.parse_args()
    if a.out:
        global BUILD
        BUILD = a.out.resolve()
    if a.id_only:
        write_identity(BUILD / EXE)
        return
    started = time.time()
    dotnet = shutil.which("dotnet")
    if not dotnet:
        sys.exit("missing: dotnet (.NET 10 SDK)")
    env = dict(os.environ)
    if WIN:
        if not (VSWHERE_DIR / "vswhere.exe").is_file() and not shutil.which("vswhere"):
            sys.exit(f"missing: vswhere.exe (expected in {VSWHERE_DIR}); NativeAOT cannot link without it")
        env["PATH"] = str(VSWHERE_DIR) + os.pathsep + env["PATH"]

    verify_vendor()
    src = prepare_sources()
    work = BUILD / "xlate"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in ("Program.cs", "AmpereLowering.cs", "MeshSpirv.cs", "nx2navi_xlate.csproj"):
        shutil.copy2(XLATE / name, work / name)
    out = BUILD / "publish"
    shutil.rmtree(out, ignore_errors=True)
    cmd = [
        dotnet, "publish", str(work / "nx2navi_xlate.csproj"), "-c", "Release", "-r", "win-x64" if WIN else "linux-x64",
        "-p:PublishAot=true", "-p:InvariantGlobalization=true", f"-p:RyuSrc={src}", "-o", str(out),
    ]
    if a.llvm:
        env["PATH"] = str(a.llvm.resolve() / "bin") + os.pathsep + env["PATH"]
        cmd += ["-p:CppCompilerAndLinker=clang", "-p:LinkerFlavor=lld"]
    if a.sysroot:
        cmd.append(f"-p:SysRoot={a.sysroot.resolve()}")
    if subprocess.run(cmd, env=env).returncode != 0:
        sys.exit("dotnet publish failed")
    exe = BUILD / EXE
    shutil.copy2(out / EXE, exe)
    write_identity(exe)
    print(f"built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
