"""Make sibling CLI helper modules importable when pytest loads scripts by path."""

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
# Tests load scripts by filename, so their sibling helpers need to be importable.
sys.path.insert(0, str(TOOLS))
