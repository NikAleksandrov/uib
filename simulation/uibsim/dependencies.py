# SPDX-License-Identifier: Apache-2.0

import sys

try:
    import jsonschema
    import yaml
except ImportError as error:
    print(f"uib-sim requires PyYAML and jsonschema: {error}", file=sys.stderr)
    sys.exit(2)
