import os
import sys
import unittest


SCRIPT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from fieldlog_decode import parse_v2_row


class TestFieldlogDecode(unittest.TestCase):
    def test_parse_v2_conneval_missing_dropped_messages(self):
        row = "conneval,2,4,,,,,,,,,,,,,,,,,,,1,0,40,25,37,118,14,1,1"
        parsed = parse_v2_row(row)
        self.assertEqual(parsed["type"], "conn_eval_v1")
        self.assertEqual(parsed["seq"], 2)
        self.assertEqual(parsed["uptime_s"], 4)
        self.assertEqual(parsed["rrc_state"], 1)
        self.assertEqual(parsed["ce_level"], 0)
        self.assertEqual(parsed["rsrp"], 40)
        self.assertEqual(parsed["rsrq"], 25)
        self.assertEqual(parsed["snr"], 37)
        self.assertEqual(parsed["dl_pathloss"], 118)
        self.assertEqual(parsed["tx_power"], 14)
        self.assertEqual(parsed["tx_rep"], 1)
        self.assertEqual(parsed["rx_rep"], 1)


if __name__ == "__main__":
    unittest.main()
