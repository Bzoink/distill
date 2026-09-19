"""The terminal front end, driven through a real pseudo-terminal.

--dump-cells gates what the terminal renderer lays out. This gates what the
cell dump cannot reach: the shell around it -- keys, the address prompt,
history, forms and the session they share, resize, the mouse, the audio
transport, and above all putting the terminal back the way it was found,
after a clean exit and after a crash.

Each check starts build/distill-tui under a pty with a known size and reads
the screen through pyte, a terminal emulator in Python, so assertions are on
what a terminal would show rather than on the bytes sent to it. Media plays
through the audio core's null backend: real time, no device, no sound.

What still needs a real terminal, and is not claimed here: capability
negotiation against real terminals, and whether kitty or sixel images
actually appear. Those were checked by hand in kitty and xterm; see
src/documents/TUI_PLAN.md.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import tempfile
import termios
import time

import resource

# check.py --stack runs the suite with the shell held to the stack Muroxi
# guarantees; None leaves the limit alone.
SHELL_STACK_KIB = None

try:
    import pyte
except ImportError:  # the caller reports this; the suite cannot run without it
    pyte = None


class Terminal:
    """One distill-tui run in a pseudo-terminal of a fixed size."""

    def __init__(self, binary, url, columns=80, rows=24, env=None, home=None):
        self.columns, self.rows = columns, rows
        self.screen = pyte.Screen(columns, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        self.status = None
        environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "TERM": "xterm-256color",
            "HOME": home,
            "XDG_CONFIG_HOME": os.path.join(home, "config"),
            "TMPDIR": home,
            # The suite asserts on text; images and the colour probe are
            # decided here so no reply from a real terminal is awaited.
            "DISTILL_TUI_GRAPHICS": "none",
            "DISTILL_TUI_COLORS": "16",
            "DISTILL_AUDIO_BACKEND": "null",
        }
        # tools/measure_budgets.py measures the shell through this suite, and
        # check.py --stack hands it an engine with a stack of its own; these
        # reach the shell and every engine the shell spawns.
        for name in ("DISTILL_STATS", "DISTILL_STACK_PROBE", "DISTILL_ENGINE"):
            if os.environ.get(name):
                environment[name] = os.environ[name]
        environment.update(env or {})
        pid, fd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            if SHELL_STACK_KIB:
                hard = resource.getrlimit(resource.RLIMIT_STACK)[1]
                resource.setrlimit(resource.RLIMIT_STACK, (SHELL_STACK_KIB * 1024, hard))
            os.execve(binary, [binary] + ([url] if url else []), environment)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def pump(self, seconds=0.1):
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            self.raw.extend(data)
            self.stream.feed(data)

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def wait_for(self, needle, seconds=20):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            if needle in self.text():
                return True
        return False

    def wait_gone(self, needle, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            if needle not in self.text():
                return True
        return False

    def send(self, data, settle=0.25):
        os.write(self.fd, data if isinstance(data, bytes) else data.encode())
        self.pump(settle)

    def click(self, x, y):
        """A left click at 0-based cell (x, y), SGR-encoded."""
        self.send(f"\x1b[<0;{x + 1};{y + 1}M\x1b[<0;{x + 1};{y + 1}m")

    def resize(self, columns, rows):
        self.columns, self.rows = columns, rows
        self.screen.resize(rows, columns)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.pump(0.6)

    def find(self, needle):
        """(x, y) of the first occurrence of needle on screen, or None."""
        for y, line in enumerate(self.screen.display):
            x = line.find(needle)
            if x >= 0:
                return x, y
        return None

    def termios_modes(self):
        attributes = termios.tcgetattr(self.fd)
        return attributes[3]  # lflag

    def finish(self, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = status
                break
        else:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        self.pump(0.2)
        return self.status

    def quit(self):
        """End the run the way a reader does, so its cleanup is what is tested."""
        if self.status is None:
            self.send("\x1b", 0.2)
            self.send("q", 0.1)
            self.finish()
        return self.status is not None and os.WIFEXITED(self.status) and os.WEXITSTATUS(self.status) == 0

    def close(self):
        try:
            if self.status is None:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass


RESTORED = [b"\x1b[?1049l", b"\x1b[?25h"]


def restored(raw):
    """Did the run end by leaving the alternate screen and showing the cursor?"""
    tail = bytes(raw[-4096:])
    return all(sequence in tail for sequence in RESTORED)


class Check:
    def __init__(self):
        self.results = []

    def __call__(self, ok, label, detail=""):
        self.results.append(("pass" if ok else "fail", label, "" if ok else detail))
        return ok


def run(binary, base):
    """[(status, label, detail)] for every check against a served corpus."""
    if pyte is None:
        return [("fail", "tui suite needs python pyte (pacman -S python-pyte)", "")]
    check = Check()
    home = tempfile.mkdtemp(prefix="distill-tui-suite-")
    os.makedirs(os.path.join(home, "config"), exist_ok=True)

    def term(url, columns=80, rows=24, env=None):
        return Terminal(binary, url, columns, rows, env, home)

    # A page renders, scrolls to what its script added, and quits cleanly.
    t = term(f"{base}/09-article.html")
    try:
        check(t.wait_for("A careful reader"), "article renders in the terminal", t.text()[:200])
        lines = t.text().split("\n")
        check(all(not line.strip() for line in lines[1:4]) and lines[4].strip(),
              "the page opens with a margin above its first line", "\n".join(lines[:6]))
        t.send("G", 0.5)
        check("This sentence was added after the page loaded" in t.text(),
              "G reaches the script-inserted paragraph", t.text()[-300:])
        modes_running = t.termios_modes()
        check(not (modes_running & termios.ICANON), "the terminal is raw while running")
        t.send("q")
        status = t.finish()
        check(status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
              "q exits with status 0", str(status))
        check(restored(t.raw), "a clean exit leaves the alternate screen and shows the cursor")
        modes = t.termios_modes()
        check(bool(modes & termios.ICANON) and bool(modes & termios.ECHO),
              "a clean exit restores canonical mode and echo")
    finally:
        t.close()

    # The address prompt, and history back and forward without a refetch.
    t = term(f"{base}/09-article.html")
    try:
        t.wait_for("A careful reader")
        t.send("o")
        check(t.wait_for("Go to:"), "o opens the address prompt")
        t.send(f"{base}/11-table.html\r")
        check(t.wait_for("Release comparison"), "an address typed at the prompt loads", t.text()[:200])
        t.send("\x7f")
        check(t.wait_for("A careful reader") and "Release comparison" not in t.text(),
              "Backspace goes back", t.text()[:200])
        t.send("L")
        check(t.wait_for("Release comparison"), "L goes forward again")
        t.send("o")
        t.wait_for("Go to:")
        t.send("\x1b", 0.4)
        check("Go to:" not in t.text(), "Escape closes the prompt without navigating")
        t.send("?")
        check(t.wait_for("Links and fields") and "\u256d" in t.text(), "? shows the keys in a box")
        t.send("x")
        check(t.wait_gone("Links and fields"), "any key closes the key list")
        t.quit()
    finally:
        t.close()

    # On a short terminal the key list scrolls instead of being cut off.
    t = term(f"{base}/09-article.html", columns=50, rows=14)
    try:
        t.wait_for("A careful reader")
        t.send("?")
        t.wait_for("Reading")
        check("more" in t.text() and "Pictures" not in t.text(), "a short key list says there is more",
              t.text())
        for _ in range(40):
            t.send("j", 0.05)
        t.pump(0.3)
        check("Pictures" in t.text() and "quit" in t.text(), "j scrolls the key list to its end", t.text())
        t.send("x")
        check(t.wait_gone("Pictures"), "another key closes the scrolled list")
        t.quit()
    finally:
        t.close()

    # Forms: a field edited in place, Enter submitting with the field as the
    # submitter, a button submitting as itself -- the desktop's contract.
    t = term(f"{base}/13-form.html", columns=160)
    try:
        t.wait_for("Search the archive")
        t.send("\t")
        t.send("\r")
        check(t.wait_for("q: reader"), "Enter on a field edits it in the prompt", t.text()[-200:])
        t.send("\x15a b&c/d\r")
        check(t.wait_for("search?q=a+b%26c%2Fd&token=CSRF-PASSTHROUGH"),
              "Enter in a field submits GET with the typed value and the hidden token",
              t.text()[:300])
        t.send("\x7f")
        t.wait_for("Search the archive")
        t.send("\t\t\r")
        check(t.wait_for("search?q=reader&token=CSRF-PASSTHROUGH&go=Search"),
              "the button submits as itself", t.text()[:300])
        t.quit()
    finally:
        t.close()

    # A POST with a password, and a session carried across engine runs.
    t = term(f"{base}/login")
    try:
        t.wait_for("LOGIN-FORM")
        t.send("\t\r")
        t.wait_for("user:")
        t.send("corpus\t")
        t.send("\r")
        t.wait_for("password:")
        t.send("fixture-password")
        check("fixture-password" not in t.text(), "a password is masked while it is typed")
        t.send("\r")
        check(t.wait_for("SIGNED-IN-AS-corpus"), "a POST login carries its session cookie",
              t.text()[:300])
        check(not any(name.startswith("distill-body") for name in os.listdir(home)),
              "the request body file is gone once the request is made")
        t.quit()
    finally:
        t.close()

    # Resize: the page reflows to the new width.
    t = term(f"{base}/09-article.html", columns=100, rows=30)
    try:
        t.wait_for("A careful reader")
        t.resize(40, 20)
        t.wait_for("A careful reader")
        widest = max(len(line.rstrip()) for line in t.screen.display[1:-1])
        check(widest <= 40 and "A careful reader" in t.text(),
              "a resize reflows the page to the new width", f"widest line {widest}")
        t.quit()
    finally:
        t.close()

    # A table wider than the terminal scrolls on its own axis.
    t = term(f"{base}/11-table.html", columns=30)
    try:
        t.wait_for("Release")
        check("›" in t.text(), "a wide table shows there is more to the right")
        t.send(">")
        check("‹" in t.text() and "Arm A" not in t.text(),
              "> scrolls the table sideways", t.text())
        t.quit()
    finally:
        t.close()

    # The mouse: the wheel scrolls, a click activates.
    t = term(f"{base}/09-article.html", columns=60, rows=12)
    try:
        t.wait_for("A careful reader")
        before = t.text()
        t.send("\x1b[<65;10;6M", 0.4)
        check(t.text() != before and "A careful reader" in t.text().splitlines()[0],
              "the wheel scrolls the page, not the bars", t.text())
        t.quit()
    finally:
        t.close()
    t = term(f"{base}/40-audio-links.html", columns=100)
    try:
        t.wait_for("Audio files on this page")
        where = t.find("Download")
        if check(where is not None, "a Download action is on screen"):
            t.click(where[0] + 1, where[1])
            check(t.wait_for("Save as:"), "clicking Download asks where to save it", t.text()[-200:])
            t.send("\x1b")
        t.quit()
    finally:
        t.close()

    # Audio: fetch, play through the null backend, transport keys.
    t = term(f"{base}/media/41-tone.wav")
    try:
        t.wait_for("[ Play ]")
        t.send("\t")
        t.send("\r", 1.5)
        shown = t.wait_for("41-tone.wav") and ("▶" in t.text() or "■" in t.text())
        check(shown, "Play fetches the file and shows the transport", t.text()[-300:])
        t.send("x", 0.4)
        check("Closed the track" in t.text(), "x closes the track", t.text()[-200:])
        t.quit()
    finally:
        t.close()

    # Every clean run above removed its private directory.
    leftovers = [name for name in os.listdir(home) if name.startswith("distill-")]
    check(not leftovers, "clean exits leave no temporary files behind", ", ".join(leftovers))

    # A crash puts the terminal back, and takes the session's cookies with it.
    for sig, name in ((signal.SIGTERM, "SIGTERM"), (signal.SIGSEGV, "SIGSEGV")):
        existing = set(os.listdir(home))
        t = term(f"{base}/09-article.html")
        try:
            t.wait_for("A careful reader")
            own = [os.path.join(home, d) for d in set(os.listdir(home)) - existing]
            os.kill(t.pid, sig)
            status = t.finish()
            check(status is not None and os.WIFSIGNALED(status) and os.WTERMSIG(status) == sig,
                  f"{name} still ends the process by that signal", str(status))
            check(restored(t.raw), f"{name} leaves the alternate screen and shows the cursor")
            modes = t.termios_modes()
            check(bool(modes & termios.ICANON) and bool(modes & termios.ECHO),
                  f"{name} restores canonical mode and echo")
            # Only this run's directory: other runs' files are not its doing.
            cookies = [os.path.join(d, f) for d in own if os.path.isdir(d)
                       for f in os.listdir(d) if f.startswith("distill-session")]
            check(len(own) == 1, f"{name}: the run made exactly one private directory",
                  ", ".join(own))
            check(not cookies, f"{name} removes the session's cookie file", ", ".join(cookies))
        finally:
            t.close()

    return check.results
