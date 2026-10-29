import json

import pytest

import tlk_saves
from conftest import SYNC

CASES = json.loads((SYNC / "merge_cases.json").read_text(encoding="utf-8"))


@pytest.mark.parametrize("case", CASES["decide"], ids=lambda c: c["id"])
def test_shared_merge_cases_decide(case):
    assert tlk_saves.decide(case["local"], case["remote"]) == case["want"]


@pytest.mark.parametrize("case", CASES["continue"], ids=lambda c: c["id"])
def test_shared_merge_cases_continue(case):
    got = tlk_saves.pick_continue(case["candidates"], case["build"])
    assert (got and got["where"]) == case["want"]


@pytest.mark.parametrize("case", CASES["device_name"], ids=lambda c: c["id"])
def test_shared_device_name_cases(case):
    assert tlk_saves.device_name(case["raw"]) == case["want"]
