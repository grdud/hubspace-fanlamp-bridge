import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from configure import candidates, remote_config, render


class SetupTests(unittest.TestCase):
    def test_supported_capture_and_render(self):
        config = dict(id=0x12345678, index=0, tx_count=17, codec_params=[True, [16, 0, 86]])
        row = dict(record=4, matches=[dict(codec="fanlamp_pro_v2/r", config=config)])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"capture.jsonl"
            path.write_text(json.dumps(row)+"\n")
            parsed = remote_config(path, 4)
            text = render(parsed, "1111111111111111", "")
            self.assertIn("#define BRIDGE_BLE_ID 305419896u", text)
            self.assertIn("#define BRIDGE_SWITCH_1_MIN 1", text)
            self.assertNotIn("RECEIVER", text)
            row["matches"][0]["codec"] = "fanlamp_pro_v1"
            path.write_text(json.dumps(row)+"\n")
            with self.assertRaises(ValueError): remote_config(path, 4)

    def test_bad_parameters_and_injected_ids_rejected(self):
        config = dict(id=1,index=0,tx_count=0)
        with self.assertRaises(ValueError):render(config, '"\n#define BAD 1', "")
        with self.assertRaises(ValueError):render(config,"2222222222222222","2222222222222222")
        with self.assertRaises(ValueError):render(config,"1111111111111111","",100,1)

    def test_switch_required(self):
        with self.assertRaises(ValueError):render(dict(id=1,index=0,tx_count=0),"","")

    def test_only_valid_unique_physical_devices_are_selectable(self):
        item = dict(deviceId="1111111111111111", friendlyName="Demo",description=dict(device=dict(deviceClass="switch")))
        self.assertEqual(candidates([item,item,{"deviceId":"bad"}],"switch"),[("Demo","1111111111111111")])


if __name__ == "__main__":unittest.main()
