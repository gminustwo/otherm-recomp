"""Builds runtime/assets/pipeline/initial_pipeline_cache.zip (the pipelines every install compiles before play) from
the aurora pipeline caches of real Other M playthroughs (<UserData>/Cache/pipeline_cache.db, its -wal beside it).

    python -I make_pipeline_seed.py <out.zip | out.db> <pipeline_cache.db> [<pipeline_cache.db> ...]

Only rows at the config versions the current renderer uses are kept (read from aurora's headers): aurora drops other
versions at startup, so a stale seed precompiles nothing: a seed at an older version leaves every shader to compile
mid-play (NOTES.md 2026-11-23). Rebuild the
seed whenever GXPipelineConfigVersion changes, and pass the current seed as an input to keep what it already holds. A
pipeline in several caches keeps its earliest first frame (startup compiles in that order). The rows describe GX
pipeline state; they hold no compiled code and no game data.

A .zip output holds the database as initial_pipeline_cache.db (runtime/cmake/PublicProducts.cmake unpacks it into the
build). Each 2.8 KB row fills a 4 KB SQLite page, so 14,600 pipelines are 58 MB raw but 1.2 MB zipped: the repository
keeps the zip, well under GitHub's file size limits. The zip is written with fixed dates, so the same inputs give the
same bytes.
"""
import re
import sqlite3
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repo
TYPE_CLEAR, TYPE_GX = 0, 1
DB_NAME = "initial_pipeline_cache.db"


def current_versions() -> dict:
    def find(path: Path, name: str) -> int:
        match = re.search(rf"{name}\s*=\s*(\d+)\s*;", path.read_text(encoding="utf-8"))
        if not match:
            sys.exit(f"{name} not found in {path}")
        return int(match.group(1))
    return {TYPE_CLEAR: find(ROOT / "aurora/lib/gfx/clear.hpp", "ClearPipelineConfigVersion"),
            TYPE_GX: find(ROOT / "aurora/lib/gx/pipeline.hpp", "GXPipelineConfigVersion")}


def source_database(source: str, scratch: Path) -> str:
    """A seed zip is read through a copy of the database it holds; anything else is opened as it is."""
    if not source.lower().endswith(".zip"):
        return source
    with zipfile.ZipFile(source) as archive:
        target = scratch / f"input{len(list(scratch.iterdir()))}.db"
        target.write_bytes(archive.read(DB_NAME))
    return str(target)


def build(out_db: Path, sources: list, scratch: Path) -> None:
    versions = current_versions()
    first = sqlite3.connect(source_database(sources[0], scratch))
    # The schema comes from the first input as aurora wrote it; its rows are replaced below.
    first.execute(f"VACUUM INTO '{out_db.as_posix()}'")
    first.close()
    db = sqlite3.connect(out_db)
    db.execute("DELETE FROM pipeline_cache")
    for source in sources:
        db.execute("ATTACH DATABASE ? AS src", (source_database(source, scratch),))
        before = db.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
        total = db.execute("SELECT COUNT(*) FROM src.pipeline_cache").fetchone()[0]
        kept = 0
        for shader_type, version in versions.items():
            cursor = db.execute(
                "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
                "SELECT type, hash, config_version, config_size, config, first_frame_used FROM src.pipeline_cache "
                "WHERE type = ? AND config_version = ? "
                "ON CONFLICT(type, hash) DO UPDATE SET first_frame_used = "
                "MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
                (shader_type, version))
            kept += cursor.rowcount
        db.commit()
        after = db.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
        db.execute("DETACH DATABASE src")
        print(f"{source}: {total} rows, {kept} at the current versions, +{after - before} new")
    rows = db.execute("SELECT type, config_version, COUNT(*) FROM pipeline_cache GROUP BY type, config_version").fetchall()
    db.execute("PRAGMA journal_mode=DELETE")
    db.execute("VACUUM")
    db.close()
    print(f"{sum(r[2] for r in rows)} pipelines (type, version, count) {rows}, {out_db.stat().st_size // 1024} KiB raw")


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as temp:
        scratch = Path(temp)
        (scratch / "inputs").mkdir()
        built = scratch / DB_NAME
        build(built, sys.argv[2:], scratch / "inputs")
        if out.suffix.lower() == ".zip":
            entry = zipfile.ZipInfo(DB_NAME, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            with zipfile.ZipFile(out, "w") as archive:
                archive.writestr(entry, built.read_bytes(), compresslevel=9)
        else:
            out.write_bytes(built.read_bytes())
    print(f"{out}: {out.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
