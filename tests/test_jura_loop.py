import unittest
from unittest.mock import AsyncMock, patch

from tools.jura_loop import (CommandTimeout, LoopError, ResponseTracker, deploy, format_dump, parse_dump,
                             parse_word, read_words, validate_config)


class ConfigTests(unittest.TestCase):
    def test_requires_refreshed_expected_source(self):
        config = """
external_components:
  - source: github://opajonk/jura-f50-esphome
    components: [jura_f50]
    refresh: 0s
wifi:
  password: !secret wifi_password
text_sensor:
  - lambda: !lambda 'return {};'
"""
        validate_config(config)
        with self.assertRaises(LoopError):
            validate_config(config.replace("refresh: 0s", "refresh: 1d"))
        with self.assertRaises(LoopError):
            validate_config(config.replace("opajonk", "other"))


class ResponseTests(unittest.TestCase):
    def test_requires_transmission_then_valid_response(self):
        tracker = ResponseTracker("TY:")
        self.assertIsNone(tracker.line("RX line: ty:F50"))
        self.assertIsNone(tracker.line("\\033[0;36mTX command: TY:\\033[0m"))
        self.assertEqual(tracker.line("RX line: ty:F50"), "ty:F50")

    def test_rejects_competing_command_timeout_and_bad_response(self):
        tracker = ResponseTracker("TY:")
        with self.assertRaises(LoopError):
            tracker.line("TX command: AN:01")
        tracker.line("TX command: TY:")
        with self.assertRaises(LoopError):
            tracker.line("No response to 'TY:' within timeout")
        with self.assertRaises(LoopError):
            tracker.line("RX line: ty:")


class EepromTests(unittest.IsolatedAsyncioTestCase):
    def test_dump_round_trip_keeps_addresses_and_missing_words(self):
        text = format_dump({0x0E: "4DA1", 0x10: "200c".upper()}, 0x0E, 0x11, ["label: off"])
        self.assertEqual(parse_dump(text), {0x0E: "4DA1", 0x10: "200C"})
        self.assertIn("10: 200C ????", text)
        with self.assertRaises(LoopError):
            parse_word("ty:EPSILON")
        self.assertEqual(parse_word("rr:0002", "RR"), "0002")
        with self.assertRaises(LoopError):
            parse_word("re:0002", "RR")

    async def test_retries_timed_out_and_malformed_reads(self):
        stream = AsyncMock()
        stream.query.side_effect = [CommandTimeout(), "re:re:00FF", "re:00ff", "re:1234"]
        stream.lines.return_value = []
        with patch("tools.jura_loop.asyncio.sleep", AsyncMock()):
            words = await read_words(stream, 1, 2, attempts=3)
        self.assertEqual(words, {1: "00FF", 2: "1234"})
        self.assertEqual([call.args[0] for call in stream.query.call_args_list], ["RE:01", "RE:01", "RE:01", "RE:02"])


class DeployTests(unittest.IsolatedAsyncioTestCase):
    @patch("tools.jura_loop.validate_config")
    @patch("tools.jura_loop.published_commit", return_value="abc123")
    async def test_confirms_compile_and_dependent_upload(self, _commit, _config):
        builder = AsyncMock()
        builder.request.side_effect = [
            "configuration",
            {"configured": [{"configuration": "jura-coffee-machine.yaml", "runtime_state": {"state": "online"}}]},
            [],
            {"job_id": "compile"},
            [
                {"job_id": "compile", "status": "completed"},
                {"job_id": "upload", "job_type": "upload", "depends_on": "compile", "status": "completed"},
            ],
        ]
        await deploy(builder)
        self.assertEqual(builder.request.call_count, 5)

    @patch("tools.jura_loop.validate_config")
    @patch("tools.jura_loop.published_commit", return_value="abc123")
    async def test_offline_device_is_not_queued(self, _commit, _config):
        builder = AsyncMock()
        builder.request.side_effect = [
            "configuration",
            {"configured": [{"configuration": "jura-coffee-machine.yaml", "runtime_state": {"state": "offline"}}]},
        ]
        with self.assertRaisesRegex(LoopError, "not online"):
            await deploy(builder)
        self.assertEqual(builder.request.call_count, 2)


if __name__ == "__main__":
    unittest.main()
