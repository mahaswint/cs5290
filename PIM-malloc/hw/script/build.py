import argparse
import os
import shutil
import subprocess
import sys


def build(root_dirpath: str, build_dirpath: str, upmem_home: str):
    cmake_cmd = [
        "cmake",
        "-S", root_dirpath,
        "-B", build_dirpath,
        "-G", "Ninja",
        f"-DUPMEM_HOME={upmem_home}",
    ]
    subprocess.run(cmake_cmd, check=True)
    subprocess.run(["ninja", "-C", build_dirpath], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Build uMalloc for UPMEM DPUs")
    parser.add_argument(
        "--upmem-home",
        default=os.environ.get("UPMEM_HOME"),
        help="Path to the UPMEM SDK installation (default: $UPMEM_HOME env var)",
    )
    args = parser.parse_args()

    if not args.upmem_home:
        print(
            "Error: UPMEM SDK path not specified.\n"
            "\n"
            "Please provide the path to your UPMEM SDK installation using one of:\n"
            "  1. Command-line argument: python3 build.py --upmem-home /path/to/upmem-sdk\n"
            "  2. Environment variable:  export UPMEM_HOME=/path/to/upmem-sdk\n",
            file=sys.stderr,
        )
        sys.exit(1)

    upmem_home = os.path.abspath(args.upmem_home)

    if not os.path.isdir(upmem_home):
        print(f"Error: UPMEM_HOME directory does not exist: {upmem_home}", file=sys.stderr)
        sys.exit(1)

    script_dirpath = os.path.dirname(__file__)

    root_dirpath = os.path.join(script_dirpath, "..")
    build_dirpath = os.path.join(root_dirpath, "build")

    if os.path.exists(build_dirpath):
        shutil.rmtree(build_dirpath)
    os.makedirs(build_dirpath)

    build(root_dirpath, build_dirpath, upmem_home)
