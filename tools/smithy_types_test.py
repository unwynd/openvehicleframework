#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for the Smithy type profile shared by communication and persistence."""

import unittest

from tools.smithy_types import compile_types


NAMESPACE = "example.model"


def integer(kind: str = "integer", bits: int = 32) -> dict:
    return {
        "type": kind,
        "traits": {"ovf.model#ovfInteger": {"signed": False, "bits": bits}},
    }


def bounded_string(capacity: int = 24, maximum: int = 24) -> dict:
    return {
        "type": "string",
        "traits": {
            "ovf.model#ovfCollection": {"storage": "bounded", "capacity": capacity},
            "smithy.api#length": {"max": maximum},
        },
    }


class SmithyTypesTest(unittest.TestCase):
    def test_communication_and_persistence_compile_the_same_type_model(self) -> None:
        shapes = {
            f"{NAMESPACE}#Counter": integer(),
            f"{NAMESPACE}#Label": bounded_string(),
            f"{NAMESPACE}#State": {
                "type": "structure",
                "members": {
                    "counter": {
                        "target": f"{NAMESPACE}#Counter",
                        "traits": {"ovf.model#ovfTag": {"value": 1}},
                    },
                    "label": {
                        "target": f"{NAMESPACE}#Label",
                        "traits": {
                            "ovf.model#ovfTag": {"value": 2},
                            "smithy.api#required": {},
                        },
                    },
                },
            },
        }
        communication = compile_types(shapes, NAMESPACE)
        persistence = compile_types(shapes, NAMESPACE, persistent=True)
        self.assertEqual(communication, persistence)

    def test_rejects_non_fixed_width_integer(self) -> None:
        with self.assertRaisesRegex(ValueError, "bigInteger"):
            compile_types({f"{NAMESPACE}#Counter": integer("bigInteger", 64)}, NAMESPACE)

    def test_rejects_native_width_mismatch(self) -> None:
        with self.assertRaisesRegex(ValueError, "width"):
            compile_types({f"{NAMESPACE}#Counter": integer("short", 32)}, NAMESPACE)

    def test_rejects_collection_bound_mismatch(self) -> None:
        with self.assertRaisesRegex(ValueError, "must agree"):
            compile_types({f"{NAMESPACE}#Label": bounded_string(24, 20)}, NAMESPACE)

    def test_persistence_requires_explicit_length_bound(self) -> None:
        shape = bounded_string()
        del shape["traits"]["smithy.api#length"]
        with self.assertRaisesRegex(ValueError, "requires @length"):
            compile_types({f"{NAMESPACE}#Label": shape}, NAMESPACE, persistent=True)

    def test_rejects_duplicate_field_tags(self) -> None:
        shape = {
            "type": "structure",
            "members": {
                "first": {
                    "target": "smithy.api#Boolean",
                    "traits": {"ovf.model#ovfTag": {"value": 1}},
                },
                "second": {
                    "target": "smithy.api#Boolean",
                    "traits": {"ovf.model#ovfTag": {"value": 1}},
                },
            },
        }
        with self.assertRaisesRegex(ValueError, "duplicate field tag"):
            compile_types({f"{NAMESPACE}#State": shape}, NAMESPACE)


if __name__ == "__main__":
    unittest.main()
