from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from benchmark_protocol import parse_timing_line  # noqa: E402


class ProtocolTests(unittest.TestCase):
    def test_valid_sample_with_gpu(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.5, '
            '"finalize_seconds": 0.25, "gpu_seconds": 1.4}'
        )
        self.assertEqual(result.kind, "sample")
        sample = result.sample
        self.assertEqual(sample.producer_seconds, 1.5)
        self.assertEqual(sample.finalize_seconds, 0.25)
        self.assertEqual(sample.gpu_seconds, 1.4)

    def test_valid_sample_without_gpu(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.5, '
            '"finalize_seconds": 0.25}'
        )
        self.assertEqual(result.kind, "sample")
        self.assertIsNone(result.sample.gpu_seconds)

    def test_integer_timing_accepted(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 2, "finalize_seconds": 0}'
        )
        self.assertEqual(result.kind, "sample")
        self.assertEqual(result.sample.producer_seconds, 2.0)
        self.assertEqual(result.sample.finalize_seconds, 0.0)

    def test_extra_fields_preserved_in_raw(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.0, '
            '"finalize_seconds": 0.1, "frame": 42}'
        )
        self.assertEqual(result.kind, "sample")
        self.assertEqual(result.sample.raw["frame"], 42)

    def test_missing_required_field_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.0}'
        )
        self.assertEqual(result.kind, "invalid")
        self.assertIn("finalize_seconds", result.message)

    def test_negative_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": -1.0, '
            '"finalize_seconds": 0.1}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_nan_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": NaN, '
            '"finalize_seconds": 0.1}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_infinity_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.0, '
            '"finalize_seconds": Infinity}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_string_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": "1.5", '
            '"finalize_seconds": 0.1}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_boolean_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": true, '
            '"finalize_seconds": 0.1}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_bad_gpu_timing_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "sample", "producer_seconds": 1.0, '
            '"finalize_seconds": 0.1, "gpu_seconds": -2.0}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_non_json_line_invalid(self) -> None:
        result = parse_timing_line("hello world")
        self.assertEqual(result.kind, "invalid")

    def test_json_array_invalid(self) -> None:
        result = parse_timing_line("[1, 2, 3]")
        self.assertEqual(result.kind, "invalid")

    def test_empty_lines_are_empty(self) -> None:
        self.assertEqual(parse_timing_line("").kind, "empty")
        self.assertEqual(parse_timing_line("   \n").kind, "empty")

    def test_hello_event_ignored(self) -> None:
        result = parse_timing_line(
            '{"event": "hello", "backend": "opengl2"}'
        )
        self.assertEqual(result.kind, "ignored")

    def test_timing_fields_without_sample_event_invalid(self) -> None:
        result = parse_timing_line(
            '{"event": "frame", "producer_seconds": 1.0, '
            '"finalize_seconds": 0.1}'
        )
        self.assertEqual(result.kind, "invalid")

    def test_whitespace_padded_line_parses(self) -> None:
        result = parse_timing_line(
            '  {"event": "sample", "producer_seconds": 1.0, '
            '"finalize_seconds": 0.1}  '
        )
        self.assertEqual(result.kind, "sample")


if __name__ == "__main__":
    unittest.main()
