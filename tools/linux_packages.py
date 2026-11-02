#!/usr/bin/env python3
"""Install Linux packages for the build - the one part of the setup that runs as root. Standard library only, and
standalone on purpose (it imports nothing of the project): what runs as root stays short enough to read.

    sudo python3 tools/linux_packages.py install [--llvm-repo CODENAME:VERSION] PACKAGE...

tools/build.py --install-deps runs it through pkexec (the desktop's password prompt), and prints the same command for a
terminal; the launcher's "Install packages" button is that. The package manager is the distribution's (apt, dnf,
pacman).

--llvm-repo: Debian / Ubuntu releases whose own clang is too old for the game (Ubuntu 22.04, Debian 12 - the game
needs clang 19+) get LLVM's official repository first, as https://apt.llvm.org documents: its signing key in
/etc/apt/keyrings (checked against the fingerprint below when gpg is there) and a sources list entry for
llvm-toolchain-<CODENAME>-<VERSION>, then apt-get update.
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.request

LLVM_KEY_URL = "https://apt.llvm.org/llvm-snapshot.gpg.key"
LLVM_KEY_FINGERPRINT = "6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"  # "Sylvestre Ledru - Debian LLVM packages"
LLVM_KEYRING = "/etc/apt/keyrings/apt.llvm.org.asc"
LLVM_SOURCES = "/etc/apt/sources.list.d/apt.llvm.org.list"


def family():
    """0 Debian / Ubuntu (apt), 1 Fedora (dnf), 2 Arch (pacman) - as tools/toolchain.py's _linux_family."""
    try:
        with open("/etc/os-release", encoding="utf-8") as f:
            text = f.read().lower()
    except OSError:
        return 0
    like = " ".join(re.findall(r"^(?:id|id_like)=\"?([^\"\n]*)", text, re.M))
    if any(w in like for w in ("fedora", "rhel", "centos")):
        return 1
    if any(w in like for w in ("arch", "manjaro", "endeavouros")):
        return 2
    return 0


def run(cmd, env=None):
    print("[packages] " + " ".join(cmd), flush=True)
    code = subprocess.call(cmd, env=env)
    if code != 0:
        print("[packages] stopped: %s exited with %d" % (cmd[0], code), flush=True)
        sys.exit(code)


def add_llvm_repo(codename, version):
    if not re.fullmatch(r"[a-z]+", codename) or not re.fullmatch(r"\d+", version):
        sys.exit("[packages] bad --llvm-repo: %s:%s" % (codename, version))
    print("[packages] LLVM's repository (apt.llvm.org) for %s, LLVM %s" % (codename, version), flush=True)
    with urllib.request.urlopen(LLVM_KEY_URL, timeout=60) as r:
        key = r.read()
    if b"BEGIN PGP PUBLIC KEY BLOCK" not in key:
        sys.exit("[packages] the download of LLVM's signing key isn't a key: nothing changed")
    try:
        out = subprocess.run(["gpg", "--show-keys", "--with-colons"], input=key, capture_output=True, timeout=60).stdout
        fingerprints = re.findall(rb"^fpr:+([0-9A-F]+):", out, re.M)
        if fingerprints and LLVM_KEY_FINGERPRINT.encode() not in fingerprints:
            sys.exit("[packages] LLVM's signing key has an unexpected fingerprint: nothing changed")
    except (OSError, subprocess.SubprocessError):
        print("[packages] (gpg isn't installed: the key's fingerprint not checked; apt checks the packages with it)")
    os.makedirs(os.path.dirname(LLVM_KEYRING), mode=0o755, exist_ok=True)
    with open(LLVM_KEYRING, "wb") as f:
        f.write(key)
    os.chmod(LLVM_KEYRING, 0o644)
    line = "deb [signed-by=%s] https://apt.llvm.org/%s/ llvm-toolchain-%s-%s main\n" % (LLVM_KEYRING, codename, codename,
                                                                                       version)
    with open(LLVM_SOURCES, "w", encoding="utf-8") as f:
        f.write("# added by GTA: Vice City Stories (PC port), tools/linux_packages.py: the game needs clang %s+\n%s"
                % (version, line))
    os.chmod(LLVM_SOURCES, 0o644)
    run(["apt-get", "update"])


def install(packages, llvm_repo):
    if os.geteuid() != 0:
        sys.exit("[packages] run as root (sudo / pkexec)")
    for p in packages:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+_:-]*", p):
            sys.exit("[packages] not a package name: %r" % p)
    fam = family()
    if llvm_repo:
        if fam != 0:
            sys.exit("[packages] --llvm-repo is for Debian / Ubuntu")
        codename, _, version = llvm_repo.partition(":")
        add_llvm_repo(codename, version)
    if fam == 0:
        env = dict(os.environ, DEBIAN_FRONTEND="noninteractive")
        run(["apt-get", "install", "-y", "--no-install-recommends"] + packages, env)
    elif fam == 1:
        run(["dnf", "install", "-y"] + packages)
    else:
        run(["pacman", "-S", "--needed", "--noconfirm"] + packages)
    print("[packages] installed: " + " ".join(packages), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("install")
    p.add_argument("--llvm-repo", metavar="CODENAME:VERSION")
    p.add_argument("packages", nargs="+")
    a = ap.parse_args()
    install(a.packages, a.llvm_repo)


if __name__ == "__main__":
    main()
