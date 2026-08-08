from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import benchmark_stats as stats  # noqa: E402


class SummarizeTests(unittest.TestCase):
    def test_summarize_known_series(self) -> None:
        summary = stats.summarize(list(range(1, 101)))
        self.assertEqual(summary["n"], 100)
        self.assertEqual(summary["min"], 1)
        self.assertEqual(summary["max"], 100)
        self.assertAlmostEqual(summary["mean"], 50.5)
        self.assertAlmostEqual(summary["median"], 50.5)
        self.assertEqual(summary["p90"], 90)
        self.assertEqual(summary["p95"], 95)
        self.assertEqual(summary["p99"], 99)
        # Unscaled MAD of 1..100 (median 50.5) is 25.0.
        self.assertAlmostEqual(summary["mad"], 25.0)

    def test_percentile_nearest_rank(self) -> None:
        self.assertEqual(stats.percentile([1, 2, 3, 4], 50), 2)
        self.assertEqual(stats.percentile([1, 2, 3, 4, 5], 100), 5)
        self.assertEqual(stats.percentile([7], 90), 7)
        self.assertEqual(stats.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 90), 9)

    def test_percentile_bounds(self) -> None:
        with self.assertRaises(ValueError):
            stats.percentile([1, 2], 0)
        with self.assertRaises(ValueError):
            stats.percentile([1, 2], 101)
        with self.assertRaises(ValueError):
            stats.percentile([1, 2], -5)

    def test_empty_inputs_raise(self) -> None:
        for call in (
            lambda: stats.summarize([]),
            lambda: stats.median([]),
            lambda: stats.mean([]),
            lambda: stats.min_max([]),
            lambda: stats.percentile([], 90),
            lambda: stats.mad([]),
        ):
            with self.assertRaises(ValueError):
                call()

    def test_non_finite_values_rejected(self) -> None:
        with self.assertRaises(ValueError):
            stats.summarize([1.0, float("nan")])
        with self.assertRaises(ValueError):
            stats.summarize([1.0, float("inf")])
        with self.assertRaises(ValueError):
            stats.mean([float("nan")])

    def test_median_odd_and_even(self) -> None:
        self.assertEqual(stats.median([3, 1, 2]), 2)
        self.assertEqual(stats.median([4, 1, 3, 2]), 2.5)

    def test_mad_constant_series_is_zero(self) -> None:
        self.assertEqual(stats.mad([5.0, 5.0, 5.0]), 0.0)
        self.assertEqual(stats.min_max([3.0, 1.0, 2.0]), (1.0, 3.0))


class TrialSpreadTests(unittest.TestCase):
    def test_relative_spread(self) -> None:
        self.assertAlmostEqual(
            stats.trial_spread([10.0, 20.0], 15.0), 10.0 / 15.0
        )

    def test_zero_overall_mean_returns_none(self) -> None:
        self.assertIsNone(stats.trial_spread([10.0, 20.0], 0.0))

    def test_single_trial_returns_none(self) -> None:
        self.assertIsNone(stats.trial_spread([10.0], 10.0))

    def test_identical_trials_have_zero_spread(self) -> None:
        self.assertEqual(stats.trial_spread([2.0, 2.0], 2.0), 0.0)


if __name__ == "__main__":
    unittest.main()
