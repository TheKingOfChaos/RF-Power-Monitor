"""Serial console of a Juniper switch (EX2200-C), for the LLDP HIL tests.

Only operational commands are run (show ...); the configuration is never touched.
The login is read from a local file that must not be committed (secret/ is in
.gitignore); it is never printed, logged or put in an exception.

    RFPOWER_LLDP_SWITCH_CONSOLE      serial port of the console, e.g. COM45 (9600 baud, Junos default)
    RFPOWER_LLDP_SWITCH_CREDENTIALS  login file, default secret/switch.md, lines "username: ..." and "password: ..."
"""

import os
import re
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.environ.get("RFPOWER_LLDP_SWITCH_CONSOLE")
CREDENTIALS = os.environ.get("RFPOWER_LLDP_SWITCH_CREDENTIALS", os.path.join(ROOT, "secret", "switch.md"))

CLI_PROMPT = re.compile(r"\S+@\S+> $")  # Junos operational mode
SHELL_PROMPT = re.compile(r"(%|#) $")  # root logs in to the FreeBSD shell first
LOGIN_PROMPT = re.compile(r"login: $")


def _read_credentials(path):
    user = password = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            key, _, value = line.partition(":")
            if key.strip().lower() == "username":
                user = value.strip()
            elif key.strip().lower() == "password":
                password = value.strip()
    if not user or not password:
        raise RuntimeError("switch login file %s needs 'username:' and 'password:' lines" % path)
    return user, password


class SwitchConsole:
    """A logged-in session on the switch's serial console. Use as a context manager."""

    def __init__(self, port=PORT, credentials=CREDENTIALS):
        self.port = port
        self.credentials = credentials
        self.ser = None
        self.shell = False  # entered the CLI from the shell (root): one more exit to log out

    def __enter__(self):
        self.ser = serial.Serial(self.port, 9600, timeout=0.2)
        try:
            self._log_in()
        except Exception:
            self.ser.close()
            raise
        return self

    def __exit__(self, *exc):
        try:
            self._log_out()
        finally:
            self.ser.close()

    def _read_until(self, pattern, timeout):
        """Read until the text read so far ends with pattern (a compiled regex); return the text"""
        out = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            out += self.ser.read(4096).decode("ascii", "replace")
            for p in pattern if isinstance(pattern, (list, tuple)) else [pattern]:
                if p.search(out.replace("\r", "")):
                    return out
        raise TimeoutError("switch console: no prompt within %.0f s" % timeout)

    def _write(self, text):
        self.ser.write(text.encode("ascii") + b"\r")

    def _log_in(self):
        self._write("")
        out = self._read_until([LOGIN_PROMPT, CLI_PROMPT, SHELL_PROMPT], 10)
        if LOGIN_PROMPT.search(out.replace("\r", "")):
            user, password = _read_credentials(self.credentials)
            self._write(user)
            self._read_until(re.compile(r"[Pp]assword:\s*$"), 10)
            self._write(password)
            try:
                out = self._read_until([CLI_PROMPT, SHELL_PROMPT, LOGIN_PROMPT], 30)
            except TimeoutError:
                raise RuntimeError("switch console: no prompt after logging in") from None
            if LOGIN_PROMPT.search(out.replace("\r", "")):
                raise RuntimeError("switch console: login refused (check the login file)")
        if SHELL_PROMPT.search(out.replace("\r", "")) and not CLI_PROMPT.search(out.replace("\r", "")):
            self.shell = True
            self._write("cli")
            self._read_until(CLI_PROMPT, 30)

    def _log_out(self):
        for _ in range(3):
            self._write("exit")
            try:
                out = self._read_until([LOGIN_PROMPT, SHELL_PROMPT, CLI_PROMPT], 10)
            except TimeoutError:
                return
            if LOGIN_PROMPT.search(out.replace("\r", "")):
                return

    def run(self, command, timeout=30):
        """Run an operational command (show ...) and return its output without the prompt"""
        if not command.startswith("show "):
            raise ValueError("only show commands are run on the switch")
        self._write(command + " | no-more")
        out = self._read_until(CLI_PROMPT, timeout).replace("\r", "")
        lines = out.split("\n")
        return "\n".join(lines[1:-1])  # drop the echoed command and the prompt
