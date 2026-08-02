import json
import tempfile
import unittest
from pathlib import Path

from scripts.evaluate_rba import (
    build_experiment_report,
    write_experiment_report,
    provenance_is_complete,
)


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


class ProvenanceGateTests(unittest.TestCase):
    def test_incomplete_provenance_missing_fields_detected(self):
        incomplete = {"git_commit": None, "openroad_version": "OpenROAD 1.0", "rba_bin_sha256": "abc"}
        missing = provenance_is_complete(incomplete)
        self.assertEqual(missing, ["git_commit"])

    def test_complete_provenance_has_no_missing_fields(self):
        complete = {"git_commit": "deadbeef", "openroad_version": "OpenROAD 1.0", "rba_bin_sha256": "abc"}
        self.assertEqual(provenance_is_complete(complete), [])

    def _sample_report(self):
        raw_results = {
            "baseline": [{"name": "b1", "method": "baseline", "drc_count": 10,
                         "via_count": 20, "wirelength": 100.0, "runtime_sec": 1.0}],
            "rba": [{"name": "b1", "method": "rba", "drc_count": 8,
                    "via_count": 18, "wirelength": 95.0, "runtime_sec": 1.5}],
        }
        return build_experiment_report(raw_results)

    def test_non_empty_report_refuses_to_write_without_provenance(self):
        report = self._sample_report()
        incomplete = {"git_commit": None, "openroad_version": None, "rba_bin_sha256": None}
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError):
                write_experiment_report(report, tmp, provenance=incomplete)
            # Must not have written a report claiming provenance it doesn't have.
            self.assertFalse((Path(tmp) / "experiment_report.json").exists())

    def test_non_empty_report_writes_with_complete_provenance(self):
        report = self._sample_report()
        complete = {"git_commit": "deadbeef", "openroad_version": "OpenROAD 1.0", "rba_bin_sha256": "abc"}
        with tempfile.TemporaryDirectory() as tmp:
            write_experiment_report(report, tmp, provenance=complete)
            self.assertTrue((Path(tmp) / "experiment_report.json").exists())

    def test_empty_report_placeholder_is_exempt_from_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            # No provenance at all — must still succeed, since an empty
            # report carries no claims to protect.
            write_experiment_report([], tmp, provenance=None)
            self.assertTrue((Path(tmp) / "experiment_report.json").exists())


if __name__ == "__main__":
    unittest.main()
