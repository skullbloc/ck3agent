"""`python -m ck3agent`: the same CLI as the `ck3agent` script."""
import sys

from .cli import main

sys.exit(main())
