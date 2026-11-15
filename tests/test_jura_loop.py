import asyncio
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from tools.jura_loop import (CommandTimeout, LogStream, LoopError, ResponseTracker, STATUS_PAUSE_ACTION,
                             deploy, dump, format_dump, parse_dump, parse_word, read_words, validate_config)


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
        self.assertIsNone(tracker.line("TX command: RR:44"))
        self.assertIsNone(tracker.line("RX line: rr:0121"))
        tracker.line("TX command: TY:")
        with self.assertRaises(LoopError):
            tracker.line("TX command: RR:44")
        tracker = ResponseTracker("TY:")
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
            words = await read_words(stream, 1, 2, attempts=3, pause_status=True)
        self.assertEqual(words, {1: "00FF", 2: "1234"})
        self.assertEqual([call.args[0] for call in stream.query.call_args_list], ["RE:01", "RE:01", "RE:01", "RE:02"])
        self.assertEqual(stream.ensure_status_pause.await_count, 4)


class StatusPauseTests(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_drain_acknowledgement_and_renews(self):
        ha = AsyncMock()
        stream = LogStream(ha, AsyncMock())
        stream.lines = AsyncMock(side_effect=[
            ["[I][jura_f50:1]: Status polling pause requested (expires in 60s)", "RX line: rr:0100"],
            ["[I][jura_f50:2]: Status polling pause ready"],
            ["[I][jura_f50:2]: Status polling pause ready"],
            ["[I][jura_f50:3]: Status polling resumed"],
        ])
        await stream.ensure_status_pause()
        ha.request.assert_awaited_once_with(
            "call_service", domain="esphome", service=STATUS_PAUSE_ACTION, service_data={"paused": True},
        )
        self.assertEqual(stream.lines.await_count, 2)
        await stream.ensure_status_pause()
        self.assertEqual(ha.request.await_count, 1)
        stream.status_pause_renew_at = 0
        await stream.ensure_status_pause()
        self.assertEqual(ha.request.await_count, 2)
        await stream.set_status_polling_paused(False)
        self.assertFalse(stream.status_pause_active)
        self.assertEqual(ha.request.call_args.kwargs["service_data"], {"paused": False})

    async def test_expiry_or_missing_action_aborts_paused_reads(self):
        stream = LogStream(AsyncMock(), AsyncMock())
        stream.status_pause_active = True
        with patch("tools.jura_loop.receive_json", AsyncMock(return_value={
            "event": "output", "data": "[I][jura_f50:3]: Status polling resumed",
        })):
            with self.assertRaisesRegex(LoopError, "resumed during"):
                await stream.lines(1)
        stream.ha.request.side_effect = LoopError("service_not_found")
        with self.assertRaisesRegex(LoopError, "Could not pause.*service_not_found"):
            await stream.ensure_status_pause()

    async def test_dump_releases_pause_on_failure_cancellation_and_failed_acquisition(self):
        args = SimpleNamespace(start=0, end=0, attempts=1, kind="RE", pause_status=True)
        for failure, acquire_failure in [
            (LoopError("read failed"), False), (asyncio.CancelledError(), False),
            (LoopError("pause failed"), True),
        ]:
            with self.subTest(failure=type(failure).__name__, acquire_failure=acquire_failure):
                stream = AsyncMock()
                if acquire_failure:
                    stream.ensure_status_pause.side_effect = failure
                with patch("tools.jura_loop.LogStream", return_value=stream), \
                     patch("tools.jura_loop.read_words", AsyncMock(side_effect=failure)) as reads:
                    with self.assertRaises(type(failure)):
                        await dump(AsyncMock(), AsyncMock(), args)
                stream.set_status_polling_paused.assert_awaited_once_with(False)
                if acquire_failure:
                    reads.assert_not_awaited()

    async def test_dump_opt_in_preserves_default_and_saves_after_resuming(self):
        with TemporaryDirectory() as directory:
            for paused in (False, True):
                with self.subTest(paused=paused):
                    output = Path(directory) / "dump.txt"
                    args = SimpleNamespace(start=0, end=0, attempts=1, kind="RE", label="test",
                                           pause_status=paused, output=str(output))
                    stream = AsyncMock()
                    with patch("tools.jura_loop.LogStream", return_value=stream), \
                         patch("tools.jura_loop.read_words", AsyncMock(return_value={0: "1234"})) as reads:
                        await dump(AsyncMock(), AsyncMock(), args)
                    self.assertEqual(parse_dump(output.read_text()), {0: "1234"})
                    self.assertEqual(reads.call_args.kwargs["pause_status"], paused)
                    if paused:
                        stream.ensure_status_pause.assert_awaited_once()
                        stream.set_status_polling_paused.assert_awaited_once_with(False)
                    else:
                        stream.ensure_status_pause.assert_not_awaited()
                        stream.set_status_polling_paused.assert_not_awaited()

    async def test_cleanup_failure_is_reported_without_hiding_read_failure(self):
        args = SimpleNamespace(start=0, end=0, attempts=1, kind="RE", pause_status=True)
        stream = AsyncMock()
        stream.set_status_polling_paused.side_effect = LoopError("resume failed")
        with patch("tools.jura_loop.LogStream", return_value=stream), \
             patch("tools.jura_loop.read_words", AsyncMock(side_effect=LoopError("read failed"))), \
             patch("tools.jura_loop.print") as output:
            with self.assertRaisesRegex(LoopError, "read failed"):
                await dump(AsyncMock(), AsyncMock(), args)
        self.assertIn("resume failed", output.call_args.args[0])


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
