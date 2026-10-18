#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check repeatable native local AI play, including real economy activity.

Use --ticks 1800 --repeats 2 --validate-traces for the regular native check.
The benchmark retains complete logs and never applies a performance threshold.
"""
import json
import sys

from benchmark_local_ai import main


if __name__ == "__main__":
    try:
        sys.exit(main(require_activity=True))
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
