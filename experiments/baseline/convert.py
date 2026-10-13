import shlex
import subprocess
import sys
from pathlib import Path

import click
import format_conversion.format as fmt
import numpy as np
import pyarrow.parquet as pq

BUILD_NODELIST = (
    Path(__file__).resolve().parents[2]
    / "external"
    / "format-conversion"
    / "scripts"
    / "build_nodelist.py"
)


def execute(cmd: list[str]) -> int:
    click.echo(shlex.join(cmd), err=True)
    return subprocess.run(cmd).returncode


def read_csr(prefix: str) -> tuple[np.ndarray, np.ndarray]:
    indptr = pq.read_table(f"{prefix}.indptr.parquet").column("indptr").to_numpy()
    indices = pq.read_table(f"{prefix}.indices.parquet").column("indices").to_numpy()
    return indptr.astype(np.uint64, copy=False), indices.astype(np.uint32, copy=False)


@click.group()
def main() -> None:
    """Convert a SNAP edge list into a CSR, and the CSR into each system's input format."""


@main.command()
@click.argument("snap", type=click.Path(exists=True, dir_okay=False))
@click.argument("out")
@click.option("-t", "--threads", type=int, default=1, help="Threads for the CSR build.")
def csr(snap: str, out: str, threads: int) -> None:
    """Convert SNAP, a gzipped SNAP edge list, into OUT.nodes.csv, OUT.indptr.parquet
    and OUT.indices.parquet: undirected, without self loops, with vertex i being line
    i of the node list.

    The edge list is decompressed to OUT.tsv while converting.
    """
    tsv, nodes = Path(f"{out}.tsv"), Path(f"{out}.nodes.csv")
    tsv.parent.mkdir(parents=True, exist_ok=True)
    click.echo(f"gzip -dc {snap} > {tsv}", err=True)
    with tsv.open("wb") as f:
        subprocess.run(["gzip", "-dc", snap], stdout=f, check=True)
    try:
        code = execute(
            [
                sys.executable,
                str(BUILD_NODELIST),
                str(tsv),
                str(nodes),
                "--sep",
                "\t",
                "--no-header",
                "--comment",
                "#",
            ]
        )
        if code:
            sys.exit(code)
        click.echo(f"{tsv} > {out}.indptr.parquet {out}.indices.parquet", err=True)
        fmt.convert(
            fmt.GraphDescriptor(str(tsv), fmt.CsvEdgelist.Read(sep="\t", skip_rows=0)),
            fmt.GraphDescriptor(out, fmt.CsrParquet.Write()),
            nodes=fmt.NodeDescriptor(str(nodes), fmt.Nodelist.Csv(skip_rows=1)),
            num_threads=threads,
            sort_neighbors=True,
        )
    finally:
        tsv.unlink()


@main.command()
@click.argument("csr_")
@click.argument("out")
def ligra(csr_: str, out: str) -> None:
    """Write OUT.config, OUT.adj and OUT.idx from the CSR at prefix CSR_: Ligra's binary
    format, read with -b by a build with -DLONG."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    indptr, indices = read_csr(csr_)
    Path(f"{out}.config").write_text(f"{len(indptr) - 1}\n")
    indices.tofile(f"{out}.adj")
    indptr[:-1].astype(np.int64).tofile(f"{out}.idx")


@main.command()
@click.argument("csr_")
@click.argument("out")
def galois(csr_: str, out: str) -> None:
    """Write OUT.sgr from the CSR at prefix CSR_: Galois's version 1 .gr format with no
    edge data."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    indptr, indices = read_csr(csr_)
    n, m = len(indptr) - 1, len(indices)
    with open(f"{out}.sgr", "wb") as f:
        np.array([1, 0, n, m], dtype=np.uint64).tofile(f)
        indptr[1:].tofile(f)
        indices.tofile(f)
        if m % 2:
            f.write(b"\0" * 4)


@main.command()
@click.argument("csr_")
@click.argument("out")
def gbbs(csr_: str, out: str) -> None:
    """Write OUT.bin from the CSR at prefix CSR_: GBBS's binary format, read with -b by
    a build with -DGBBSLONG."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    indptr, indices = read_csr(csr_)
    n, m = len(indptr) - 1, len(indices)
    with open(f"{out}.bin", "wb") as f:
        np.array([n, m, 0], dtype=np.int64).tofile(f)
        indptr.tofile(f)
        indices.tofile(f)


@main.command()
@click.argument("csr_")
@click.argument("out")
@click.option("-t", "--threads", type=int, default=1, help="Threads for writing OUT.e.")
def grape(csr_: str, out: str, threads: int) -> None:
    """Write OUT.v, one vertex id per line, and OUT.e, each undirected edge once as
    "u v", from the CSR at prefix CSR_."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    indptr = pq.read_table(f"{csr_}.indptr.parquet").column("indptr")
    np.savetxt(f"{out}.v", np.arange(len(indptr) - 1), fmt="%d")
    fmt.convert(
        fmt.GraphDescriptor(f"{csr_}.indices.parquet", fmt.CsrParquet.Read()),
        fmt.GraphDescriptor(out, fmt.CsvEdgelist.Write(sep=" ", header=False)),
        num_threads=threads,
    )
    Path(f"{out}.csv").rename(f"{out}.e")


if __name__ == "__main__":
    main()
