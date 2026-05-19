import unittest

import numpy as np

from analyze_results import compute_charge_metrics, event_marker, parse_state_transition


class TestAnalyzeResultsParsing(unittest.TestCase):
    def test_parse_transition_line(self):
        msg = "TRANSITION: STATE_LTEM_CONNECTING -> STATE_BACKOFF"
        from_state, to_state = parse_state_transition(msg)
        self.assertEqual(from_state, "STATE_LTEM_CONNECTING")
        self.assertEqual(to_state, "STATE_BACKOFF")

    def test_parse_field_log_state(self):
        msg = (
            "Field log state #42: STATE_GNSS_ACQUIRE -> STATE_NTN_CONNECTING "
            "reason=EVT_TIMEOUT rat=LTE-M/NTN loc=none rsrp=-105 dBm"
        )
        from_state, to_state = parse_state_transition(msg)
        self.assertEqual(from_state, "STATE_GNSS_ACQUIRE")
        self.assertEqual(to_state, "STATE_NTN_CONNECTING")

    def test_event_marker_lte_probe(self):
        label, key, from_state, to_state = event_marker(
            "LTE probe: TN still bad -> returning to NTN"
        )
        self.assertEqual(label, "Stay on NTN")
        self.assertEqual(key, "stay-ntn")
        self.assertIsNone(from_state)
        self.assertIsNone(to_state)

    def test_event_marker_modem_switch(self):
        label, key, from_state, to_state = event_marker(
            "switch: sending XSYSTEMMODE NTN"
        )
        self.assertEqual(label, "Modem mode NTN")
        self.assertEqual(key, "modem-mode-ntn")
        self.assertIsNone(from_state)
        self.assertIsNone(to_state)

    def test_charge_metrics_constant_signal(self):
        samples = np.full(10, 1000.0, dtype=np.float32)
        metrics = compute_charge_metrics(samples, sample_rate=1.0, include_clipped=False)
        self.assertAlmostEqual(metrics["charge_C"], 0.01, places=6)
        self.assertAlmostEqual(metrics["charge_mC"], 10.0, places=6)
        self.assertAlmostEqual(metrics["charge_uAh"], 2.777777, delta=1e-6)

    def test_charge_metrics_clipped(self):
        samples = np.array([1000.0, -500.0, 1000.0], dtype=np.float32)
        metrics = compute_charge_metrics(samples, sample_rate=1.0, include_clipped=True)
        self.assertAlmostEqual(metrics["charge_C"], 0.0015, places=6)
        self.assertAlmostEqual(metrics["charge_C_clipped"], 0.002, places=6)


if __name__ == "__main__":
    unittest.main()
