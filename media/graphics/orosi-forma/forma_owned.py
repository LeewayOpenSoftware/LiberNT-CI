# Shared check for the render scripts: paths listed in forma-owned.txt carry Orosi Forma art.
from pathlib import Path

_OWNED = tuple(l.strip() for l in Path(__file__).with_name("forma-owned.txt").read_text(encoding="utf-8").splitlines()
               if l.strip() and not l.startswith("#"))


def is_owned(path):
    p = str(path).replace("\\", "/")
    return any(p == o or p.endswith("/" + o) for o in _OWNED)
