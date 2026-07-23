#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
from pathlib import Path

from tools.validate_ir import normalized

parser = argparse.ArgumentParser()
parser.add_argument("left", type=Path)
parser.add_argument("right", type=Path)
args = parser.parse_args()
left = normalized(json.loads(args.left.read_text(encoding="utf-8")))
right = normalized(json.loads(args.right.read_text(encoding="utf-8")))
if left != right:
    raise SystemExit("canonical IR models differ")
