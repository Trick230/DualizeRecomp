"""Merge played pipeline caches into the seed shipped next to the executable.

usage: pipeline_seed.py [cache.db ...]

Aurora merges DualizeRecomp/resources/initial_pipeline_cache.db into the
user's cache at each start, and the render thread compiles everything the
cache knows before the game runs, so a seed made from playing every stage
leaves nothing to compile (or pop in) mid-play on a fresh install.

The default source is the user's own cache
(%LOCALAPPDATA%/DualizeRecomp/Cache/NativeGX/pipeline_cache.db). Only rows
of each type's newest config version are kept: older ones no longer load.
"""
import os
import sqlite3
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEED = os.path.join(REPO, "DualizeRecomp", "resources", "initial_pipeline_cache.db")
SCHEMA = 1


def default_cache():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "DualizeRecomp", "Cache", "NativeGX", "pipeline_cache.db")


def open_seed():
    os.makedirs(os.path.dirname(SEED), exist_ok=True)
    db = sqlite3.connect(SEED)
    db.executescript(f"""
        CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);
        CREATE TABLE IF NOT EXISTS pipeline_cache (
          type INTEGER NOT NULL,
          hash INTEGER NOT NULL,
          config_version INTEGER NOT NULL,
          config_size INTEGER NOT NULL,
          config BLOB NOT NULL,
          first_frame_used INTEGER NOT NULL,
          PRIMARY KEY (type, hash)
        );
        DELETE FROM aurora_schema;
        INSERT INTO aurora_schema VALUES ({SCHEMA});
    """)
    return db


def main(sources):
    seed = open_seed()
    for path in sources:
        src = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        rows = src.execute(
            "SELECT type, hash, config_version, config_size, config, first_frame_used FROM pipeline_cache"
        ).fetchall()
        seed.executemany(
            "INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(type, hash) DO UPDATE SET "
            "config_version = excluded.config_version, config_size = excluded.config_size, "
            "config = excluded.config, "
            "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
            rows,
        )
        src.close()
        print(f"{path}: {len(rows)} rows")
    seed.execute(
        "DELETE FROM pipeline_cache WHERE config_version < "
        "(SELECT MAX(config_version) FROM pipeline_cache AS newest WHERE newest.type = pipeline_cache.type)"
    )
    seed.commit()
    seed.execute("VACUUM")
    for type_, version, count in seed.execute(
        "SELECT type, config_version, COUNT(*) FROM pipeline_cache GROUP BY type, config_version"
    ):
        print(f"seed: type {type_} version {version}: {count} pipelines")
    seed.close()


if __name__ == "__main__":
    main(sys.argv[1:] or [default_cache()])
