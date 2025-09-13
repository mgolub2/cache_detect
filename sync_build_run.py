#!/usr/bin/env python3
"""
Sync Makefile and cache_detect.c to remote hosts over SSH, build, run, and
collect results into text files named after the remote CPU (via lscpu).

Usage:
  python3 sync_build_run.py hosts.txt \
    --remote-dir ~/cache_detect \
    --jobs 6 \
    --output-dir . \
    --patterns all|random,seq,reverse,stride,interleave,gray,bitrev

hosts.txt format:
  - One host per line (e.g., "user@host", "host", or SSH config alias)
  - Blank lines and lines starting with '#' are ignored

Requirements:
  - Passwordless SSH to each host (keys or agent). The script uses BatchMode.
  - Remote host has make and a C compiler installed.
  - On Linux, lscpu is preferred for CPU name; macOS fallback via sysctl.

Notes:
  - Only Makefile and cache_detect.c are synced (as requested).
  - Results are written locally to files named "<CPU Name> (<pattern>).txt".
  - Use --append to collate multiple runs into the same file(s).
  - By default, runs all patterns up to 1 GiB working set size.
"""

from __future__ import annotations

import argparse
import concurrent.futures
from pathlib import Path
import shlex
import subprocess
from typing import Dict, List, Optional, Tuple


def run_subprocess(args: List[str], timeout: Optional[int] = None, input_text: Optional[str] = None) -> Tuple[int, str, str]:
    """Run a subprocess and return (returncode, stdout, stderr)."""
    try:
        completed = subprocess.run(
            args,
            input=input_text,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        return completed.returncode, completed.stdout, completed.stderr
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or f"Command timed out after {timeout}s"
        return 124, stdout, stderr


def build_ssh_base_args(
    host: str,
    port: int,
    identity_file: Optional[str],
    connect_timeout: int,
    extra_args: Optional[List[str]] = None,
) -> List[str]:
    args: List[str] = ["ssh", "-o", "BatchMode=yes", "-o", f"ConnectTimeout={connect_timeout}"]
    if port:
        args += ["-p", str(port)]
    if identity_file:
        args += ["-i", identity_file]
    if extra_args:
        args += extra_args
    args.append(host)
    return args


def build_scp_args(
    port: int,
    identity_file: Optional[str],
    extra_args: Optional[List[str]] = None,
) -> List[str]:
    args: List[str] = ["scp", "-q"]
    if port:
        args += ["-P", str(port)]
    if identity_file:
        args += ["-i", identity_file]
    if extra_args:
        args += extra_args
    return args


def normalize_remote_dir_arg(remote_dir: str) -> str:
    """Ensure '~' is preserved for remote expansion if path equals local home prefix.

    If user passed a path like '/Users/alice/cache_detect' that is actually their
    local HOME + '/cache_detect', rewrite it to '~/cache_detect' so the remote
    shell expands it to the remote user's HOME instead of trying to create
    '/Users/...'.
    """
    try:
        local_home = str(Path.home())
        # Accept both exact home and home-prefixed paths
        if remote_dir == local_home:
            return "~"
        if remote_dir.startswith(local_home + "/"):
            suffix = remote_dir[len(local_home) + 1 :]
            return f"~/{suffix}"
    except Exception:
        pass
    return remote_dir


def quote_remote_dir_for_shell(remote_dir: str) -> str:
    """Quote a remote directory path for use inside bash -lc while allowing ~ expansion.

    We cannot single-quote ~ if we want it to expand. Strategy:
      - If path starts with '~', leave ~ unquoted and quote the remainder safely.
      - Else, single-quote the whole path via shlex.quote.
    """
    if remote_dir.startswith("~"):
        # Split '~' and the remainder (handle '~' or '~/...')
        if remote_dir == "~":
            return "~"
        rest = remote_dir[1:]
        # Ensure leading slash remains if present
        return "~" + shlex.quote(rest)
    return shlex.quote(remote_dir)


def detect_remote_cpu_name(
    host: str,
    port: int,
    identity_file: Optional[str],
    connect_timeout: int,
    extra_ssh_args: Optional[List[str]] = None,
) -> str:
    """Detect CPU model name on remote host using lscpu with fallbacks."""
    cpu_cmd = r'''
set -e
name=""
if command -v lscpu >/dev/null 2>&1; then
    name=$(lscpu | awk -F: 'tolower($1) ~ /model name/ {print $2; exit}')
    name="$(echo "$name" | sed -e "s/^[[:space:]]*//" -e "s/[[:space:]]*$//")"
fi
if [ -z "$name" ] && [ -r /proc/cpuinfo ]; then
    name=$(grep -m1 -i "model name" /proc/cpuinfo | sed -e "s/.*:[[:space:]]*//")
fi
if [ -z "$name" ] && command -v sysctl >/dev/null 2>&1; then
    name=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)
fi
if [ -z "$name" ]; then
    name=$(uname -m)
fi
printf "%s\n" "$name"
'''
    remote = f"bash -lc {shlex.quote(cpu_cmd)}"
    args = build_ssh_base_args(host, port, identity_file, connect_timeout, extra_args=extra_ssh_args)
    args.append(remote)
    rc, out, err = run_subprocess(args, timeout=connect_timeout + 10)
    name = (out or "").strip() or host
    return name


def sanitize_cpu_name(name: str) -> str:
    """Sanitize CPU name for use as a filename, lightly normalizing."""
    replacements = ["(R)", "(TM)", "CPU", "Processor", "altivec supported"]
    for token in replacements:
        name = name.replace(token, "")
    name = name.replace("®", "").replace("™", "").replace("℠", "")

    import re

    # Drop trailing frequency like '@ 3.20GHz'
    name = re.sub(r"\s*@\s*[\d\.\-]+\s*[GMk]?[Hh][Zz].*$", "", name)
    # Collapse spaces
    name = re.sub(r"\s+", " ", name).strip()

    # Keep safe characters
    safe_chars: List[str] = []
    for ch in name:
        if ch.isalnum() or ch in (" ", "-", "_", ".", "(", ")"):
            safe_chars.append(ch)
    safe_name = "".join(safe_chars).strip()
    return safe_name or "unknown_cpu"


def ensure_remote_dir(
    host: str,
    port: int,
    identity_file: Optional[str],
    connect_timeout: int,
    remote_dir: str,
    extra_ssh_args: Optional[List[str]] = None,
) -> None:
    path_for_shell = quote_remote_dir_for_shell(remote_dir)
    cmd = f"mkdir -p {path_for_shell}"
    remote = f"bash -lc {shlex.quote(cmd)}"
    args = build_ssh_base_args(host, port, identity_file, connect_timeout, extra_args=extra_ssh_args)
    args.append(remote)
    rc, _, err = run_subprocess(args, timeout=connect_timeout + 10)
    if rc != 0:
        raise RuntimeError(f"Failed to create remote dir on {host}: {err.strip()}")


def copy_sources_to_remote(
    host: str,
    port: int,
    identity_file: Optional[str],
    remote_dir: str,
    sources: List[Path],
    extra_scp_args: Optional[List[str]] = None,
) -> None:
    scp_args = build_scp_args(port, identity_file, extra_args=extra_scp_args)
    for src in sources:
        dest = f"{host}:{remote_dir}/"
        args = scp_args + [str(src), dest]
        rc, _, err = run_subprocess(args, timeout=60)
        if rc != 0:
            raise RuntimeError(f"Failed to copy {src.name} to {host}: {err.strip()}")


def remote_build(
    host: str,
    port: int,
    identity_file: Optional[str],
    connect_timeout: int,
    remote_dir: str,
    extra_ssh_args: Optional[List[str]] = None,
    build_timeout: int = 600,
) -> None:
    """Build remotely using make."""
    path_for_shell = quote_remote_dir_for_shell(remote_dir)
    build_cmd = f"cd {path_for_shell} && make clean all >/dev/null 2>&1"
    remote = f"bash -lc {shlex.quote('set -e; ' + build_cmd)}"
    args = build_ssh_base_args(host, port, identity_file, connect_timeout, extra_args=extra_ssh_args)
    args.append(remote)
    rc, out, err = run_subprocess(args, timeout=build_timeout)
    if rc != 0:
        msg = err.strip() or out.strip() or "unknown error"
        raise RuntimeError(f"Build failed on {host}: {msg}")


def remote_run_pattern(
    host: str,
    port: int,
    identity_file: Optional[str],
    connect_timeout: int,
    remote_dir: str,
    pattern: str,
    extra_ssh_args: Optional[List[str]] = None,
    run_timeout: int = 600,
) -> str:
    """Run the benchmark remotely for a single pattern and return stdout."""
    path_for_shell = quote_remote_dir_for_shell(remote_dir)
    # Up to 1 GiB, min 1 KiB
    run_cmd = (
        f"cd {path_for_shell} && ./cache_detect --min-bytes 1024 --max-bytes 1073741824 "
        f"--pattern {shlex.quote(pattern)}"
    )
    remote = f"bash -lc {shlex.quote('set -e; ' + run_cmd)}"
    args = build_ssh_base_args(host, port, identity_file, connect_timeout, extra_args=extra_ssh_args)
    args.append(remote)
    rc, out, err = run_subprocess(args, timeout=run_timeout)
    if rc != 0:
        msg = err.strip() or out.strip() or "unknown error"
        raise RuntimeError(f"Run failed on {host} (pattern={pattern}): {msg}")
    return out


def process_host(host: str, args) -> Tuple[str, List[Path], Optional[str]]:
    """Process a single host: sync, build, run all patterns, and write output files."""
    try:
        ensure_remote_dir(host, args.port, args.identity, args.ssh_timeout, args.remote_dir, extra_ssh_args=args.ssh_option)

        sources = [Path(args.repo_dir) / "Makefile", Path(args.repo_dir) / "cache_detect.c"]
        for src in sources:
            if not src.exists():
                raise FileNotFoundError(f"Local source missing: {src}")
        copy_sources_to_remote(host, args.port, args.identity, args.remote_dir, sources, extra_scp_args=args.scp_option)

        cpu_name = detect_remote_cpu_name(host, args.port, args.identity, args.ssh_timeout, extra_ssh_args=args.ssh_option)
        safe_cpu = sanitize_cpu_name(cpu_name)

        # Build once
        remote_build(host, args.port, args.identity, args.ssh_timeout, args.remote_dir, extra_ssh_args=args.ssh_option, build_timeout=args.build_timeout)

        # Run each selected pattern
        output_paths: List[Path] = []
        out_dir = Path(args.output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        for pat in args.patterns:
            out_text = remote_run_pattern(
                host,
                args.port,
                args.identity,
                args.ssh_timeout,
                args.remote_dir,
                pat,
                extra_ssh_args=args.ssh_option,
                run_timeout=args.run_timeout,
            )
            output_path = out_dir / f"{safe_cpu} ({pat}).txt"
            mode = "a" if args.append else "w"
            with output_path.open(mode, encoding="utf-8") as f:
                if args.append and f.tell() > 0:
                    f.write("\n\n")
                f.write(out_text.rstrip() + "\n")
            output_paths.append(output_path)

        return host, output_paths, None
    except Exception as exc:  # noqa: BLE001 - top-level boundary
        return host, [], str(exc)


def parse_hosts_file(path: Path) -> List[str]:
    hosts: List[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        hosts.append(line)
    return hosts


def parse_patterns_arg(p: str) -> List[str]:
    ALL = ["random", "seq", "reverse", "stride", "interleave", "gray", "bitrev"]
    p = (p or "all").strip()
    if not p or p.lower() == "all":
        return ALL
    parts = [t.strip() for t in p.split(",")]
    # Keep order, filter empties and dedupe preserving first occurrence
    seen: Dict[str, bool] = {}
    result: List[str] = []
    for t in parts:
        if not t:
            continue
        if t not in seen:
            result.append(t)
            seen[t] = True
    return result or ALL


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Sync Makefile and cache_detect.c to hosts, build, run, and collect results, "
            "naming files by CPU (via lscpu)."
        )
    )
    parser.add_argument("hosts_file", help="Path to file containing hostnames or ssh aliases, one per line.")
    parser.add_argument(
        "--remote-dir",
        default="~/cache_detect",
        help="Remote working directory to place sources and build. Default: %(default)s",
    )
    parser.add_argument(
        "--output-dir",
        default=str(Path(__file__).resolve().parent),
        help="Local directory to write results. Default: script directory.",
    )
    parser.add_argument(
        "--repo-dir",
        default=str(Path(__file__).resolve().parent),
        help="Local repo directory containing Makefile and cache_detect.c. Default: script directory.",
    )
    parser.add_argument("-p", "--port", type=int, default=22, help="SSH port. Default: 22")
    parser.add_argument("-i", "--identity", help="SSH identity file (private key).")
    parser.add_argument("--ssh-timeout", type=int, default=15, help="SSH connect timeout seconds. Default: %(default)s")
    parser.add_argument("--build-timeout", type=int, default=300, help="Build timeout seconds. Default: %(default)s")
    parser.add_argument("--run-timeout", type=int, default=6000, help="Run timeout seconds. Default: %(default)s")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="Max concurrent hosts. Default: %(default)s")
    parser.add_argument("--append", action="store_true", help="Append to output file(s) if they exist instead of overwriting.")
    parser.add_argument(
        "--ssh-option",
        action="append",
        default=[],
        help=(
            "Extra -o options for ssh, e.g., --ssh-option StrictHostKeyChecking=no. "
            "Can be repeated."
        ),
    )
    parser.add_argument(
        "--scp-option",
        action="append",
        default=[],
        help=(
            "Extra options for scp, e.g., --scp-option -oStrictHostKeyChecking=no. "
            "Can be repeated."
        ),
    )
    parser.add_argument(
        "--patterns",
        default="all",
        help=(
            "Comma-separated pattern names to run, or 'all'. "
            "Patterns: random, seq, reverse, stride, interleave, gray, bitrev."
        ),
    )
    args = parser.parse_args()

    # Normalize ssh/scp options
    def normalize_ssh_opts(opts: List[str]) -> List[str]:
        norm: List[str] = []
        for o in opts:
            if o.startswith("-o"):
                if o == "-o":
                    # ignore lonely -o; expect the next token to be provided
                    continue
                norm.append(o)
            else:
                norm += ["-o", o]
        return norm

    def normalize_scp_opts(opts: List[str]) -> List[str]:
        norm: List[str] = []
        i = 0
        while i < len(opts):
            o = opts[i]
            if o.startswith("-o") and o != "-o":
                # scp prefers '-o', 'Key=Value' split
                norm += ["-o", o[2:]]
            else:
                norm.append(o)
            i += 1
        return norm

    args.ssh_option = normalize_ssh_opts(args.ssh_option)
    args.scp_option = normalize_scp_opts(args.scp_option)

    # Normalize remote-dir: if user shell expanded '~' to local home, map back to '~'
    args.remote_dir = normalize_remote_dir_arg(args.remote_dir)

    hosts = parse_hosts_file(Path(args.hosts_file))
    if not hosts:
        print("No hosts found in hosts file.", flush=True)
        return

    # Parse patterns
    args.patterns = parse_patterns_arg(args.patterns)

    print(f"Processing {len(hosts)} host(s) with up to {args.jobs} concurrent jobs...", flush=True)

    results: List[Tuple[str, List[Path], Optional[str]]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_host = {executor.submit(process_host, host, args): host for host in hosts}
        for future in concurrent.futures.as_completed(future_to_host):
            host = future_to_host[future]
            try:
                host, out_paths, err = future.result()
                if err:
                    print(f"[{host}] ERROR: {err}", flush=True)
                else:
                    if out_paths:
                        names = ", ".join(p.name for p in out_paths)
                        print(f"[{host}] OK -> {names}", flush=True)
                    else:
                        print(f"[{host}] OK (no outputs)", flush=True)
                results.append((host, out_paths, err))
            except Exception as exc:  # noqa: BLE001 - boundary
                print(f"[{host}] Exception: {exc}", flush=True)
                results.append((host, [], str(exc)))

    ok = sum(1 for _, paths, e in results if paths and not e)
    print(f"Done. Successful: {ok}/{len(results)}")


if __name__ == "__main__":
    main()



