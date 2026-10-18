"""Restore script-local imports for the isolated bundled CPython runtime."""
import sys
from pathlib import Path
repo = Path(__file__).resolve().parents[2]
if sys.argv and sys.argv[0] not in ("", "-c", "-m"):
    script = Path(sys.argv[0]).resolve()
    if script.is_relative_to(repo):
        sys.path.insert(0, str(script.parent))
sys.path.append(str(repo / "shared/factory-python"))
