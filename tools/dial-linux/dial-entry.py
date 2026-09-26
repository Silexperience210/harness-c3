"""Entry point for the Hermes agent shown on the Harness C3 dial.

Started by hermes-dial.sh through the ~/.harness/bin/hermes-agent symlink, so the
process is named "hermes-agent" (what the Harness daemon matches) instead of
"python3" (what ~/.local/bin/hermes would be).

This mirrors the bootstrap the Hermes launcher itself uses; it is reproduced here
only because we need the process name, not the wrapper.
"""

import os
import re
import sys

HERMES_DIR = os.path.join(os.path.expanduser("~"), ".hermes", "hermes-agent")

# The launcher clears these: inheriting a foreign PYTHONHOME/PYTHONPATH breaks
# the embedded interpreter.
os.environ.pop("PYTHONHOME", None)
os.environ.pop("PYTHONPATH", None)
os.environ.pop("VIRTUAL_ENV", None)
sys.path.insert(0, HERMES_DIR)

from hermes_constants import get_default_hermes_root  # noqa: E402

os.environ["HERMES_HOME"] = os.environ.get("HERMES_HOME") or str(get_default_hermes_root())

import hermes_bootstrap  # noqa: E402,F401
from hermes_cli.main import main  # noqa: E402

# Hermes expects argv[0] without a Windows script suffix.
sys.argv[0] = re.sub(r"(-script\.pyw|\.exe)?$", "", sys.argv[0])
sys.exit(main())
