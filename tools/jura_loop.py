#!/usr/bin/env python3
"""Deploy the published component and exercise the live Jura connection."""

import argparse
import asyncio
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urljoin, urlsplit, urlunsplit

import aiohttp
import yaml

ROOT = Path(__file__).resolve().parents[1]
CONFIGURATION = "jura-coffee-machine.yaml"
ADDON = "5c53de3b_esphome"
ACTION = "jura_coffee_machine_send_command"
SOURCE = "github://opajonk/jura-f50-esphome"
ANSI = re.compile(r"(?:\x1b|\\033)\[[0-9;]*m")


class LoopError(Exception):
    pass


class CommandTimeout(LoopError):
    pass


class BadResponse(LoopError):
    pass


class ConfigLoader(yaml.SafeLoader):
    pass


def secret(loader, node):
    return loader.construct_scalar(node)


ConfigLoader.add_constructor("!secret", secret)
ConfigLoader.add_constructor("!lambda", secret)


def validate_config(text):
    try:
        config = yaml.load(text, Loader=ConfigLoader)
    except yaml.YAMLError as exc:
        raise LoopError(f"Cannot safely parse device configuration: {type(exc).__name__}") from exc
    sources = config.get("external_components", []) if isinstance(config, dict) else []
    matching = [
        item for item in sources
        if isinstance(item, dict) and item.get("source") == SOURCE
        and "jura_f50" in item.get("components", [])
    ]
    if len(matching) != 1 or str(matching[0].get("refresh")) not in ("0s", "0"):
        raise LoopError("Device must use the expected GitHub source with refresh: 0s.")


def git(*args):
    result = subprocess.run(
        ["git", "-C", str(ROOT), *args],
        capture_output=True, text=True, check=False,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    if result.returncode:
        raise LoopError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def published_commit():
    if git("branch", "--show-current") != "main":
        raise LoopError("Deployment requires the main branch.")
    if git("remote", "get-url", "origin") not in (
        "https://github.com/opajonk/jura-f50-esphome",
        "https://github.com/opajonk/jura-f50-esphome.git",
        "git@github.com:opajonk/jura-f50-esphome.git",
    ):
        raise LoopError("The origin remote does not match the device's external component source.")
    if git("status", "--porcelain", "--", "components/jura_f50"):
        raise LoopError("Commit component changes before deploying.")
    commit = git("rev-parse", "HEAD")
    remote = git("ls-remote", "--symref", "origin", "HEAD", "refs/heads/main").splitlines()
    if "ref: refs/heads/main\tHEAD" not in remote or f"{commit}\trefs/heads/main" not in remote:
        raise LoopError("Local HEAD does not match origin/main; request approval before pushing.")
    return commit


def websocket_url(url):
    parts = urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.netloc:
        raise LoopError("Home Assistant URL must be an HTTP(S) URL.")
    return urlunsplit(("wss" if parts.scheme == "https" else "ws", parts.netloc, parts.path, "", ""))


async def receive_json(ws, timeout=30):
    message = await asyncio.wait_for(ws.receive(), timeout)
    if message.type != aiohttp.WSMsgType.TEXT:
        raise LoopError("WebSocket closed or returned a non-text message.")
    return message.json()


class HomeAssistant:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0

    async def request(self, kind, **data):
        self.next_id += 1
        request_id = self.next_id
        await self.ws.send_json({"id": request_id, "type": kind, **data})
        while True:
            msg = await receive_json(self.ws)
            if msg.get("id") != request_id:
                continue
            if not msg.get("success"):
                raise LoopError(f"Home Assistant {kind} failed: {msg.get('error', {}).get('code', 'unknown')}")
            return msg.get("result")


class Builder:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0

    async def request(self, command, **args):
        self.next_id += 1
        message_id = str(self.next_id)
        await self.ws.send_json({"command": command, "message_id": message_id, "args": args})
        while True:
            msg = await receive_json(self.ws)
            if msg.get("message_id") != message_id:
                continue
            if msg.get("error_code"):
                raise LoopError(f"Builder {command} failed: {msg['error_code']}")
            if "result" in msg:
                return msg["result"]

    async def logs(self):
        self.next_id += 1
        message_id = str(self.next_id)
        await self.ws.send_json({
            "command": "devices/logs", "message_id": message_id,
            "args": {"configuration": CONFIGURATION, "port": "OTA"},
        })
        return message_id


async def connect(session, base_url, token):
    ha_ws = await session.ws_connect(websocket_url(urljoin(base_url, "/api/websocket")))
    msg = await receive_json(ha_ws)
    if msg.get("type") != "auth_required":
        raise LoopError("Home Assistant did not request authentication.")
    await ha_ws.send_json({"type": "auth", "access_token": token})
    if (await receive_json(ha_ws)).get("type") != "auth_ok":
        raise LoopError("Home Assistant authentication failed.")
    ha = HomeAssistant(ha_ws)
    info = await ha.request("supervisor/ingress/info", slug=ADDON)
    ingress = await ha.request("supervisor/ingress/session", slug=ADDON)
    url = urljoin(base_url, info["ingress_url"].rstrip("/") + "/ws")
    if (urlsplit(url).scheme, urlsplit(url).netloc) != (
        urlsplit(base_url).scheme, urlsplit(base_url).netloc
    ):
        raise LoopError("Unexpected ingress scheme or host.")
    builder_ws = await session.ws_connect(
        websocket_url(url), headers={"Cookie": f"ingress_session={ingress['session']}"}
    )
    await receive_json(builder_ws)
    return ha, Builder(builder_ws)


async def deploy(builder):
    commit = published_commit()
    validate_config(await builder.request("devices/get_config", configuration=CONFIGURATION))
    devices = await builder.request("devices/list")
    target = next((d for d in devices["configured"] if d.get("configuration") == CONFIGURATION), None)
    if not target or target.get("runtime_state", {}).get("state") != "online":
        raise LoopError("Device is not online; refusing to schedule a deferred OTA.")
    jobs = await builder.request("firmware/get_jobs", configuration=CONFIGURATION)
    if any(job["status"] in ("running", "queued", "pending") for job in jobs):
        raise LoopError("A device job is already active; refusing to replace it.")
    job = await builder.request("firmware/install", configuration=CONFIGURATION, port="OTA")
    compile_id = job["job_id"]
    print(f"Started OTA for {commit[:12]} (compile job {compile_id}).", flush=True)
    deadline = asyncio.get_running_loop().time() + 1200
    while asyncio.get_running_loop().time() < deadline:
        jobs = await builder.request("firmware/get_jobs", configuration=CONFIGURATION)
        compile_job = next((j for j in jobs if j["job_id"] == compile_id), None)
        upload = next((j for j in jobs if j.get("depends_on") == compile_id and j["job_type"] == "upload"), None)
        if not compile_job:
            raise LoopError("Compile job disappeared; inspect the dashboard.")
        if compile_job.get("is_deferred_install"):
            raise LoopError("OTA was deferred; inspect and clear the queued update in the dashboard if needed.")
        if compile_job["status"] in ("failed", "cancelled"):
            raise LoopError(f"Compile job {compile_id} {compile_job['status']}; inspect the dashboard.")
        if upload and upload["status"] in ("failed", "cancelled"):
            raise LoopError(f"Upload job {upload['job_id']} {upload['status']}; inspect the dashboard.")
        if compile_job["status"] == "completed" and upload and upload["status"] == "completed":
            if published_commit() != commit:
                raise LoopError("GitHub main changed during deployment; deployed revision is uncertain.")
            print(f"Compile and OTA upload completed (upload job {upload['job_id']}).")
            return
        await asyncio.sleep(2)
    raise LoopError("OTA timed out; inspect the dashboard before retrying.")


class ResponseTracker:
    def __init__(self, command):
        self.command = command
        self.sent = False

    def line(self, line):
        line = ANSI.sub("", line)
        if "TX command: " in line:
            sent = line.split("TX command: ", 1)[1].strip()
            if sent != self.command or self.sent:
                raise LoopError("Another command was transmitted during the test; response is ambiguous.")
            self.sent = True
        if self.sent and "No response to " in line:
            raise CommandTimeout("Jura command timed out.")
        if self.sent and "RX line: " in line:
            response = line.split("RX line: ", 1)[1].strip()
            if self.command == "TY:" and not (response.startswith("ty:") and response[3:].strip()):
                raise LoopError("Jura returned an unexpected response to TY:.")
            return response
        return None


async def smoke_test(ha, builder, command, attempts):
    message_id = await builder.logs()
    tracker = ResponseTracker(command)
    deadline = asyncio.get_running_loop().time() + 60
    requested = False
    ready_at = None
    sent_attempts = 0
    while asyncio.get_running_loop().time() < deadline:
        now = asyncio.get_running_loop().time()
        if not requested and ready_at is not None and now >= ready_at:
            await ha.request("call_service", domain="esphome", service=ACTION,
                             service_data={"command": command})
            requested = True
            sent_attempts += 1
        try:
            msg = await receive_json(builder.ws, timeout=min(1, deadline - now))
        except asyncio.TimeoutError:
            continue
        if msg.get("message_id") != message_id:
            continue
        if msg.get("error_code") or msg.get("event") == "result":
            raise LoopError("Device log stream stopped; inspect the dashboard.")
        if msg.get("event") != "output":
            continue
        for line in msg.get("data", "").splitlines():
            clean = ANSI.sub("", line)
            if not requested and ("Successful handshake" in clean or (ready_at is not None and "[C][" in clean)):
                ready_at = asyncio.get_running_loop().time() + 2
            try:
                response = tracker.line(line)
            except CommandTimeout:
                if sent_attempts >= attempts:
                    raise
                print(f"Attempt {sent_attempts} timed out; retrying {command}.", file=sys.stderr)
                tracker = ResponseTracker(command)
                requested = False
                ready_at = asyncio.get_running_loop().time() + 2
                continue
            if response is not None:
                print(f"{command} -> {response}")
                return
    raise LoopError("Timed out waiting for the device log or response.")


READS = ("RE", "RR")
WORDS_PER_LINE = 16


def read_command(address, kind="RE"):
    # The machine parses only the first two address digits: RE:0100 reads word 01.
    if kind not in READS or not 0 <= address <= 0xFF:
        raise LoopError("Read must be RE: or RR: with a word address between 00 and FF.")
    return f"{kind}:{address:02X}"


def parse_word(response, kind="RE"):
    match = re.fullmatch(rf"{kind.lower()}:([0-9A-Fa-f]{{4}})", response.strip())
    if not match:
        raise BadResponse(f"Unexpected {kind}: response: {response!r}")
    return match.group(1).upper()


def format_dump(words, start, end, header):
    lines = [f"# {line}" for line in header]
    first = start - start % WORDS_PER_LINE
    for base in range(first, end + 1, WORDS_PER_LINE):
        cells = []
        for address in range(base, base + WORDS_PER_LINE):
            if start <= address <= end:
                cells.append(words.get(address, "????"))
            else:
                cells.append("----")
        lines.append(f"{base:02X}: {' '.join(cells)}")
    return "\n".join(lines) + "\n"


def parse_dump(text):
    words = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        base, cells = line.split(":", 1)
        for offset, cell in enumerate(cells.split()):
            if cell not in ("????", "----"):
                words[int(base, 16) + offset] = cell
    return words


class LogStream:
    def __init__(self, ha, builder):
        self.ha = ha
        self.builder = builder
        self.message_id = None

    async def lines(self, timeout):
        try:
            msg = await receive_json(self.builder.ws, timeout=timeout)
        except asyncio.TimeoutError:
            return []
        if msg.get("message_id") != self.message_id:
            return []
        if msg.get("error_code") or msg.get("event") == "result":
            raise LoopError("Device log stream stopped; inspect the dashboard.")
        if msg.get("event") != "output":
            return []
        return [ANSI.sub("", line) for line in msg.get("data", "").splitlines()]

    async def open(self):
        self.message_id = await self.builder.logs()
        loop = asyncio.get_running_loop()
        deadline = loop.time() + 60
        ready_at = None
        while ready_at is None or loop.time() < ready_at:
            if loop.time() > deadline:
                raise LoopError("Timed out waiting for the device log stream.")
            for line in await self.lines(timeout=0.5):
                if "Successful handshake" in line or (ready_at is not None and "[C][" in line):
                    ready_at = loop.time() + 2

    async def query(self, command, timeout=10):
        tracker = ResponseTracker(command)
        await self.ha.request("call_service", domain="esphome", service=ACTION,
                              service_data={"command": command})
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            for line in await self.lines(timeout=min(1, deadline - loop.time())):
                response = tracker.line(line)
                if response is not None:
                    return response
        raise CommandTimeout(f"No log response for {command}.")


async def read_words(stream, start, end, attempts, progress=None, kind="RE"):
    words = {}
    for address in range(start, end + 1):
        command = read_command(address, kind)
        for attempt in range(1, attempts + 1):
            try:
                words[address] = parse_word(await stream.query(command), kind)
                break
            except (CommandTimeout, BadResponse):
                # Let a late reply to the timed-out read pass before the next request.
                await asyncio.sleep(1)
                await stream.lines(timeout=0.5)
                if attempt == attempts:
                    print(f"{command} failed after {attempts} attempts.", file=sys.stderr)
        if progress:
            progress(address)
    return words


async def dump(ha, builder, args):
    stream = LogStream(ha, builder)
    await stream.open()
    started = datetime.now(timezone.utc)
    total = args.end - args.start + 1

    def progress(address):
        done = address - args.start + 1
        if done % WORDS_PER_LINE == 0 or done == total:
            print(f"\r{done}/{total} words", end="", file=sys.stderr, flush=True)

    words = await read_words(stream, args.start, args.end, args.attempts, progress, args.kind)
    print(file=sys.stderr)
    header = [f"label: {args.label}", f"command: {args.kind}:", f"started: {started.isoformat(timespec='seconds')}",
              f"range: {args.start:04X}-{args.end:04X}", f"missing: {total - len(words)}"]
    output = Path(args.output) if args.output else (
        ROOT / "dumps" / f"{started:%Y%m%dT%H%M%SZ}-{args.kind.lower()}-{re.sub(r'[^A-Za-z0-9_-]+', '-', args.label)}.txt")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(format_dump(words, args.start, args.end, header))
    print(f"Wrote {len(words)}/{total} words to {output}")


def diff_dumps(paths):
    dumps = [parse_dump(Path(path).read_text()) for path in paths]
    addresses = sorted(set().union(*dumps))
    print("addr  " + "  ".join(Path(path).stem[-12:].rjust(12) for path in paths))
    for address in addresses:
        values = [dump.get(address, "????") for dump in dumps]
        if len(set(values) - {"????"}) > 1:
            print(f"{address:04X}  " + "  ".join(value.rjust(12) for value in values))


async def run(args):
    if args.action == "diff":
        diff_dumps(args.dumps)
        return
    token_file = Path(args.token_file).expanduser()
    if token_file.stat().st_mode & 0o077:
        raise LoopError("Token file must not be accessible to group or others.")
    token = token_file.read_text().strip()
    if not token:
        raise LoopError("Token file is empty.")
    if urlsplit(args.home_assistant).scheme == "http" and not args.allow_http:
        raise LoopError("HTTP exposes credentials on the network; use HTTPS or explicitly pass --allow-http.")
    timeout = aiohttp.ClientTimeout(total=None, sock_connect=15)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        ha, builder = await connect(session, args.home_assistant, token)
        if args.action == "deploy":
            await deploy(builder)
        elif args.action == "dump":
            await dump(ha, builder, args)
        else:
            await smoke_test(ha, builder, args.command, args.attempts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # The dev container forwards JURA_HOME_ASSISTANT as an empty string when the host leaves it unset.
    parser.add_argument("--home-assistant", default=os.environ.get("JURA_HOME_ASSISTANT") or None,
                        required=not os.environ.get("JURA_HOME_ASSISTANT"))
    parser.add_argument("--token-file", default="~/.config/jura-loop/home-assistant-token")
    parser.add_argument("--allow-http", action="store_true", help="accept plaintext LAN credential exposure")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("deploy", help="compile and OTA the published main branch")
    test = sub.add_parser("command", help="send a command and wait for its response")
    test.add_argument("command", nargs="?", default="TY:")
    test.add_argument("--confirm-command", action="store_true",
                      help="confirm a command other than the read-only TY: handshake")
    test.add_argument("--attempts", type=int, choices=range(1, 4), default=1,
                      help="number of attempts for TY: (maximum 3)")
    eeprom = sub.add_parser("dump", help="read 16-bit words 00-FF with read-only RE: (EEPROM) or RR: (RAM)")
    eeprom.add_argument("--kind", choices=READS, default="RE")
    eeprom.add_argument("--start", type=lambda value: int(value, 16), default=0)
    eeprom.add_argument("--end", type=lambda value: int(value, 16), default=0xFF)
    eeprom.add_argument("--label", default="dump", help="machine state note stored in the dump")
    eeprom.add_argument("--output", help="dump file (default: dumps/<timestamp>-<label>.txt)")
    eeprom.add_argument("--attempts", type=int, choices=range(1, 4), default=3)
    compare = sub.add_parser("diff", help="list words whose read values differ between dump files")
    compare.add_argument("dumps", nargs="+")
    args = parser.parse_args()
    if args.action == "dump" and not 0 <= args.start <= args.end <= 0xFF:
        parser.error("EEPROM word range must satisfy 00 <= start <= end <= FF")
    if args.action == "command" and args.command != "TY:" and not args.confirm_command:
        parser.error("other live commands require --confirm-command")
    if args.action == "command" and args.command != "TY:" and args.attempts != 1:
        parser.error("retries are only supported for the read-only TY: handshake")
    if args.action == "command" and (not args.command or "\n" in args.command or "\r" in args.command):
        parser.error("command must be a non-empty single line")
    try:
        asyncio.run(run(args))
    except (LoopError, OSError, asyncio.TimeoutError, aiohttp.ClientError) as exc:
        if isinstance(exc, aiohttp.ClientError):
            print(f"Connection failed: {type(exc).__name__}", file=sys.stderr)
        else:
            print(f"Error: {str(exc) or type(exc).__name__}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
