import shlex
import subprocess
import sys
from pathlib import Path

import click

LIGRA = Path(__file__).resolve().parents[2] / "external" / "ligra"
BUILD = Path(__file__).resolve().parent / "build" / "ligra"
CXXFLAGS = [
    "-std=c++14",
    "-fopenmp",
    "-march=native",
    "-O3",
    "-DOPENMP",
    "-DBYTERLE",
    "-include",
    "cstdint",
]


def execute(cmd: list[str]) -> int:
    click.echo(shlex.join(cmd), err=True)
    return subprocess.run(cmd).returncode


@click.group()
def main() -> None:
    pass


@main.command()
@click.argument("app")
def build(app: str) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    source = LIGRA / "apps" / f"{app}.C"
    cmd = ["g++", *CXXFLAGS, "-I", str(LIGRA / "ligra"), str(source), "-o", str(BUILD / app)]
    sys.exit(execute(cmd))


@main.command()
@click.argument("app")
@click.argument("graph", type=click.Path(exists=True, dir_okay=False))
def run(app: str, graph: str) -> None:
    binary = BUILD / app
    if not binary.exists():
        raise click.ClickException(f"{binary} not found; run `build {app}` first")
    sys.exit(execute([str(binary), "-s", graph]))


if __name__ == "__main__":
    main()
