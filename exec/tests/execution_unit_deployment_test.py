# SPDX-License-Identifier: Apache-2.0

"""Semantic tests for mixed execution-unit deployments."""

from __future__ import annotations

import copy
import unittest

from tools.exec_deployment import validate_semantics


def model() -> dict:
    return {
        "platform": {"dinit": {"nativeServices": []}},
        "units": [
            {
                "id": 1,
                "name": "storage",
                "kind": "one_shot",
                "bootstrap": True,
                "executable": "/bin/prepare-storage",
                "arguments": [],
                "nativeService": "",
                "stopExecutable": "",
                "stopArguments": [],
                "readiness": "successful_exit",
                "startTimeoutMs": 1000,
                "stopTimeoutMs": 1000,
                "retry": {"maxAttempts": 1, "delayMs": 0},
                "dependencies": [],
                "exclusiveResources": [],
            },
            {
                "id": 2,
                "name": "network",
                "kind": "service",
                "bootstrap": False,
                "executable": "/bin/network",
                "arguments": [],
                "nativeService": "",
                "stopExecutable": "",
                "stopArguments": [],
                "readiness": "process_started",
                "startTimeoutMs": 1000,
                "stopTimeoutMs": 1000,
                "retry": {"maxAttempts": 1, "delayMs": 0},
                "dependencies": [1],
                "exclusiveResources": [],
            },
        ],
        "domains": [
            {
                "id": 1,
                "name": "machine",
                "initialMode": 1,
                "replacement": "reject_while_busy",
                "recovery": {
                    "action": "hold_observed_configuration",
                    "deadlineMs": 1000,
                },
                "modes": [
                    {"id": 1, "name": "boot", "units": [], "constraints": []},
                    {
                        "id": 2,
                        "name": "operational",
                        "units": [2],
                        "constraints": [],
                    },
                ],
            }
        ],
    }


class ExecutionUnitDeploymentTest(unittest.TestCase):
    def test_accepts_explicit_bootstrap_and_mode_membership(self) -> None:
        self.assertEqual(validate_semantics(model()), [])

    def test_rejects_unassigned_non_bootstrap_unit(self) -> None:
        value = model()
        value["domains"][0]["modes"][1]["units"] = []
        self.assertTrue(
            any("no explicit mode membership" in error for error in validate_semantics(value))
        )

    def test_rejects_bootstrap_unit_in_mode(self) -> None:
        value = model()
        value["domains"][0]["modes"][0]["units"] = [1]
        self.assertTrue(
            any("bootstrap units must not be mode members" in error
                for error in validate_semantics(value))
        )

    def test_rejects_bootstrap_dependency_on_managed_unit(self) -> None:
        value = model()
        value["units"][0]["dependencies"] = [2]
        self.assertTrue(
            any("bootstrap unit depends on managed unit" in error
                for error in validate_semantics(value))
        )

    def test_rejects_readiness_incompatible_with_kind(self) -> None:
        value = copy.deepcopy(model())
        value["units"][1]["readiness"] = "successful_exit"
        self.assertTrue(
            any("unsupported for service" in error for error in validate_semantics(value))
        )

    def test_requires_external_service_to_be_supplied_by_platform(self) -> None:
        value = model()
        external = copy.deepcopy(value["units"][1])
        external.update(
            {
                "id": 3,
                "name": "native_logger",
                "kind": "external",
                "executable": "",
                "nativeService": "platform-logger",
                "readiness": "supervisor_notification",
                "dependencies": [],
            }
        )
        value["units"].append(external)
        value["domains"][0]["modes"][1]["units"].append(3)
        self.assertTrue(
            any("not supplied by the platform" in error for error in validate_semantics(value))
        )
        value["platform"]["dinit"]["nativeServices"] = ["platform-logger"]
        self.assertEqual(validate_semantics(value), [])


if __name__ == "__main__":
    unittest.main()
