"""
Pytest configuration for all tests.

Handles path setup so test modules can import from tests.libs.
"""

import sys
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))
