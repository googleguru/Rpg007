import json
import tempfile
import unittest
from pathlib import Path

from scripts.evaluate_rba import build_experiment_report


class EvaluationReportTests(unittest.TestCase):
    def test_build_experiment_report_includes_absolute_stats(self):
        raw_results = {
            "baseline": [
                {
                    "name": "ispd18_test1",
                    "method": "baseline",
                    "drc_count": 100,
                    "via_count": 200,
                    "wirelength": 1000.0,
                    "runtime_sec": 10.0,
                    "contest_score": 100,
                },
                {
                    "name": "ispd18_test1",
                    "method": "baseline",
                    "drc_count": 120,
                    "via_count": 220,
                    "wirelength": 1100.0,
                    "runtime_sec": 12.0,
                    "contest_score": 90,
                },
            ],
            "rba": [
                {
                    "name": "ispd18_test1",
                    "method": "rba",
                    "drc_count": 80,
                    "via_count": 180,
                    "wirelength": 1050.0,
                    "runtime_sec": 14.0,
                    "contest_score": 110,
                },
                {
                    "name": "ispd18_test1",
                    "method": "rba",
                    "drc_count": 85,
                    "via_count": 190,
                    "wirelength": 1080.0,
                    "runtime_sec": 15.0,
                    "contest_score": 105,
                },
            ],
        }

        report = build_experiment_report(raw_results)
        self.assertEqual(len(report), 1)
        entry = report[0]
        self.assertEqual(entry["benchmark"], "ispd18_test1")
        self.assertEqual(entry["baseline"]["drc_count"]["mean"], 110.0)
        self.assertEqual(entry["rba"]["drc_count"]["mean"], 82.5)
        self.assertEqual(entry["delta_pct"]["drc_count"], -25.0)
        self.assertEqual(entry["equal_runtime"]["drc_count"]["winner"], "rba")
        self.assertEqual(entry["equal_compute_budget"]["drc_count"]["winner"], "rba")


if __name__ == "__main__":
    unittest.main()
