"""Serial console of a Juniper switch (EX2200-C), for the LLDP and switch port HIL tests.

run() only runs operational commands (show ...). configure() changes the configuration
and is only used by the opt-in switch port tests (test_switch_port.py): it locks the
configuration (configure exclusive; a private candidate cannot "commit confirmed") and
refuses to commit if the candidate already holds someone else's uncommitted changes.
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
CONFIG_PROMPT = re.compile(r"\S+@\S+# $")  # Junos configuration mode
DISCARD_PROMPT = re.compile(r"\[yes,no\] \(yes\) $")  # leaving configuration mode with uncommitted changes
CONFIG_ERROR = re.compile(r"^(error:|syntax error|unknown command)", re.MULTILINE)
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

    def configure(self, statements, confirm_minutes=None):
        """Apply set/delete statements under an exclusive lock and commit them.

        With confirm_minutes the commit is "commit confirmed": Junos rolls it back by itself
        unless another commit follows within that time. A failed statement or commit raises
        RuntimeError and discards the candidate, so nothing half-done is left behind.
        """
        for s in statements:
            if not s.startswith(("set ", "delete ")):
                raise ValueError("only set and delete statements are applied: %r" % s)
        self._write("configure exclusive")
        out = self._read_until([CONFIG_PROMPT, CLI_PROMPT], 30).replace("\r", "")
        if not CONFIG_PROMPT.search(out) or CONFIG_ERROR.search(out):
            raise RuntimeError("switch console: configure exclusive refused:\n" + out)
        try:
            self._write("show | compare")
            out = self._read_until(CONFIG_PROMPT, 30).replace("\r", "")
            # Without the echo, [edit] and the prompt, and the note of our own pending "commit confirmed"
            pending = "\n".join(line for line in out.split("\n")[1:-2]
                                if not line.startswith("# commit confirmed will be rolled back")).strip()
            if pending:
                raise RuntimeError("switch console: the candidate already has someone's uncommitted "
                                   "changes, refusing to commit them:\n" + pending)
            for s in statements:
                self._write(s)
                out = self._read_until(CONFIG_PROMPT, 30).replace("\r", "")
                if CONFIG_ERROR.search(out):
                    raise RuntimeError("switch console: %r refused:\n%s" % (s, out))
            self._write("commit confirmed %d" % confirm_minutes if confirm_minutes else "commit")
            out = self._read_until(CONFIG_PROMPT, 240).replace("\r", "")
            if "commit complete" not in out:
                raise RuntimeError("switch console: commit failed:\n" + out)
        finally:
            self._write("exit configuration-mode")
            if DISCARD_PROMPT.search(self._read_until([CLI_PROMPT, DISCARD_PROMPT], 30).replace("\r", "")):
                self._write("yes")
                self._read_until(CLI_PROMPT, 30)
