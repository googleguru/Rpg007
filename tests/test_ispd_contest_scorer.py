import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from ispd_contest_scorer import (
    ScoreComponents,
    original_score,
    runtime_factor,
    scaled_score,
    rank_teams,
    classify_drc_markers,
)


class ScoreComponentsTests(unittest.TestCase):
    def test_from_partial_tracks_unmeasured_fields(self):
        c = ScoreComponents.from_partial({"num_spacing_violations": 2})
        self.assertIn("num_short_violations", c.unmeasured)
        self.assertNotIn("num_spacing_violations", c.unmeasured)

    def test_from_partial_rejects_unknown_field(self):
        with self.assertRaises(ValueError):
            ScoreComponents.from_partial({"not_a_real_field": 1})

    def test_open_net_makes_invalid(self):
        c = ScoreComponents.from_partial({}, open_nets=1)
        self.assertFalse(c.is_valid())
        self.assertIsNone(scaled_score(c, 10.0, 10.0))


class OriginalScoreTests(unittest.TestCase):
    def test_weighted_sum_matches_official_weights(self):
        c = ScoreComponents.from_partial({
            "num_spacing_violations": 2,
            "total_wire_length_m2_pitch": 1000,
        })
        # 500*2 + 0.5*1000 = 1500, per metrics_and_ranking.pdf's weight table.
        self.assertEqual(original_score(c), 1500.0)


class RuntimeFactorTests(unittest.TestCase):
    def test_eight_times_slower_is_six_percent_penalty(self):
        # Worked example from metrics_and_ranking.pdf: 8x slower/faster
        # than the median -> ~6% score penalty/benefit.
        self.assertAlmostEqual(runtime_factor(800.0, 100.0), 0.06, places=6)
        self.assertAlmostEqual(runtime_factor(100.0, 800.0), -0.06, places=6)

    def test_clamped_to_plus_minus_point_one(self):
        self.assertLessEqual(runtime_factor(1e9, 1.0), 0.1)
        self.assertGreaterEqual(runtime_factor(1.0, 1e9), -0.1)


class RankTeamsTests(unittest.TestCase):
    def test_matches_official_worked_example(self):
        # Verbatim from metrics_and_ranking.pdf slide 8's "Scaled Score
        # Table" -> "Final Ranking Result" example.
        scores = {
            "benchmark1": {"team1": 80, "team2": 200, "team3": 200, "team4": 250, "team5": 100},
            "benchmark2": {"team1": 90, "team2": 180, "team3": 70, "team4": 130, "team5": 60},
            "benchmark3": {"team1": 70, "team2": None, "team3": 40, "team4": None, "team5": 180},
            "benchmark4": {"team1": 300, "team2": 800, "team3": 180, "team4": 250, "team5": 400},
            "benchmark5": {"team1": 150, "team2": None, "team3": 150, "team4": 170, "team5": 160},
        }
        result = rank_teams(scores)
        expected = {"team1": 1.75, "team2": 4.5, "team3": 1.25, "team4": 3.75, "team5": 2.25}
        for team, exp in expected.items():
            self.assertAlmostEqual(result[team], exp, places=9, msg=team)

    def test_lower_average_rank_wins(self):
        scores = {
            "b1": {"a": 10, "b": 20},
            "b2": {"a": 15, "b": 5},
        }
        result = rank_teams(scores)
        # Ties in the drop-worst average here; both teams have one
        # first-place and one dropped worst -> both end up at 1.0.
        self.assertEqual(result["a"], 1.0)
        self.assertEqual(result["b"], 1.0)


class ClassifyDrcMarkersTests(unittest.TestCase):
    def test_buckets_known_types_only(self):
        markers = [{"type": "SHORT"}, {"type": "SPACING"}, {"type": "SPACING"}, {"type": "WIDTH"}]
        counts = classify_drc_markers(markers)
        self.assertEqual(counts["num_short_violations"], 1.0)
        self.assertEqual(counts["num_spacing_violations"], 2.0)
        self.assertEqual(counts["num_min_area_violations"], 0.0)
        # WIDTH has no official-metric equivalent and must not leak into
        # any bucket.
        self.assertEqual(sum(counts.values()), 3.0)


if __name__ == "__main__":
    unittest.main()
