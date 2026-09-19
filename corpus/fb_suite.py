"""The framebuffer shell, driven through its memory sink (check.py --fb).

distill-fb runs as it would on a console, with its display replaced by an
in-memory device (src/display_memory.c). A script feeds it input -- console
bytes through the shared decoder, as the Linux console and Muroxi send them,
SGR mouse reports included, and pixel clicks as evdev or a window sends them
-- and asks for the shell's state and for screenshots of what reached the
device. Nothing here needs a console, a window or root.

The memory device's rows are padded past width * 4 on purpose: the real
console on the machine this was written on is not, and a blit that ignores
the stride has to fail somewhere a gate can see. distill-fb-defect is built
with exactly that blit, and the stride check must catch it.

What cannot be gated, and is checked by hand on a VT (FRAMEBUFFER_PLAN.md
phase D): VT switching, KD_GRAPHICS and console restore, tty decoding under a
real keymap, and evdev.
"""

import json
import os
import resource
import shutil
import signal
import subprocess
import tempfile
import time

# Set by check.py --stack to run the shell in that many KiB of stack.
SHELL_STACK_KIB = None

WIDTH, HEIGHT = 1280, 800


def limit_stack():
    """The shell's own stack, soft limit only: the engines it starts raise
    theirs back to the 8 MiB they are given, as on Muroxi."""
    if SHELL_STACK_KIB:
        hard = resource.getrlimit(resource.RLIMIT_STACK)[1]
        resource.setrlimit(resource.RLIMIT_STACK, (SHELL_STACK_KIB * 1024, hard))


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    parts = data.split(b"\n", 3)
    width, height = (int(v) for v in parts[1].split())
    return width, height, parts[3]


def pixel(shot, x, y):
    width, _, data = shot
    at = (y * width + x) * 3
    return tuple(data[at:at + 3])


def dark_fraction(shot, x0, y0, x1, y1, limit=90):
    dark = total = 0
    for y in range(y0, y1):
        for x in range(x0, x1, 2):
            r, g, b = pixel(shot, x, y)
            dark += r < limit and g < limit and b < limit
            total += 1
    return dark / max(total, 1)


class Run:
    """One run of the shell over a script; states and screenshots after."""

    def __init__(self, binary, url, steps, env, scratch, width=WIDTH, height=HEIGHT, timeout=90,
                 extra=None):
        self.dir = tempfile.mkdtemp(prefix="run-", dir=scratch)
        self.state_path = os.path.join(self.dir, "state.jsonl")
        self.shots = {}
        lines = []
        for step in steps:
            if step == "STATE":
                lines.append(f"state {self.state_path}")
            elif step.startswith("SHOT "):
                name = step[5:]
                path = os.path.join(self.dir, name + ".ppm")
                self.shots[name] = path
                lines.append(f"screenshot {path}")
            else:
                lines.append(step)
        script = os.path.join(self.dir, "script")
        with open(script, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        command = [binary, "--display", f"memory:{width}x{height},script={script}"] + (extra or [])
        if url:
            command.append(url)
        started = time.time()
        self.process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout,
                                      preexec_fn=limit_stack)
        self.seconds = time.time() - started
        self.states = []
        if os.path.exists(self.state_path):
            with open(self.state_path, encoding="utf-8") as handle:
                self.states = [json.loads(line) for line in handle if line.strip()]

    @property
    def status(self):
        return self.process.returncode

    def shot(self, name):
        path = self.shots.get(name)
        return read_ppm(path) if path and os.path.exists(path) else None

    def state(self, index=-1):
        return self.states[index] if self.states and -len(self.states) <= index < len(self.states) else {}


class Check:
    def __init__(self):
        self.results = []

    def __call__(self, ok, label, detail=""):
        self.results.append(("pass" if ok else "fail", label, "" if ok else str(detail)))
        return ok


def screen_point(state):
    """The centre of the focused thing, in display pixels."""
    focus = state.get("focus") or {}
    x = focus.get("x", 0) + focus.get("w", 0) // 2
    y = focus.get("y", 0) - state.get("scroll", 0) + state.get("view_top", 0) + focus.get("h", 0) // 2
    return x, y


def layout_intact(shot, page_rgb):
    """The frame is where it belongs: the bar across the whole top, the same
    bar across the whole bottom, the page colour at both edges between. A
    blit that ignores the stride shears every row after the first and leaves
    the device's last rows unwritten, so this cannot hold for it."""
    width, height, _ = shot
    top = pixel(shot, width // 2, 2)
    edges = [pixel(shot, 2, y) for y in (60, height // 2, height - 60)] + \
            [pixel(shot, width - 30, y) for y in (60, height // 2)]
    bars = [pixel(shot, 2, 2), pixel(shot, width - 3, 2), pixel(shot, 2, height - 3),
            pixel(shot, width - 3, height - 3)]
    return all(abs(a - b) <= 2 for edge in edges for a, b in zip(edge, page_rgb)) and \
        all(bar == top for bar in bars)


def run(binary, base, defect=None):
    """[(status, label, detail)] for every check against a served corpus."""
    check = Check()
    scratch = tempfile.mkdtemp(prefix="distill-fb-suite-")
    home = os.path.join(scratch, "home")
    tmp = os.path.join(scratch, "tmp")
    os.makedirs(os.path.join(home, "config"))
    os.makedirs(tmp)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, "config"), TMPDIR=tmp,
               DISTILL_AUDIO_BACKEND="null", LANG=os.environ.get("LANG", "C.UTF-8"))
    env.pop("DISTILL_DISPLAY", None)

    def fb(url, steps, **kwargs):
        return Run(binary, url, steps, env, scratch, **kwargs)

    # A page renders, scrolls to what its script added, and quits cleanly.
    r = fb(f"{base}/09-article.html", ["wait", "SHOT top", "STATE", "send G", "STATE", "SHOT end", "send q"])
    s0, s1 = r.state(0), r.state(1)
    check(r.status == 0, "q exits with status 0", r.process.stderr[-300:])
    check(s0.get("title") == "A careful reader" and not s0.get("loading"), "the article loads", s0)
    top = r.shot("top")
    check(top is not None and top[:2] == (WIDTH, HEIGHT), "the device is the size asked for")
    if top:
        check(dark_fraction(top, 330, 100, 700, 160) > 0.05,
              "the headline is drawn where the reader column puts it",
              dark_fraction(top, 330, 100, 700, 160))
        page_rgb = pixel(top, 2, 300)
        check(layout_intact(top, page_rgb), "chrome and page sit where they belong on a padded device")
    check(s1.get("scroll", 0) == max(0, s1.get("page_height", 0) - s1.get("viewport", 0)) and s1.get("scroll", 0) > 0,
          "G reaches the bottom of the page", s1)

    # The stride: the same run through a blit that ignores both strides.
    if defect and top:
        d = Run(defect, f"{base}/09-article.html", ["wait", "SHOT top", "send q"], env, scratch)
        bad = d.shot("top")
        caught = bad is not None and bad[2] != top[2] and not layout_intact(bad, pixel(top, 2, 300))
        check(caught, "a blit that ignores the stride is caught (distill-fb-defect)")

    # The address line, and history back and forward without a refetch.
    r = fb(f"{base}/09-article.html", [
        "wait", "send o", "STATE", f"send {base}/11-table.html\\r", "wait", "STATE",
        "send \\x7f", "STATE", "send L", "STATE", "send o", "send \\e", "STATE",
        "send ?", "STATE", "SHOT help", "send x", "STATE", "quit"])
    states = r.states
    check(len(states) == 7, "the history script ran", r.process.stderr[-300:])
    if len(states) == 7:
        check((states[0].get("prompt") or {}).get("kind") == "address", "o opens the address line")
        check(states[1].get("url", "").endswith("/11-table.html") and states[1].get("back"),
              "an address typed at the prompt loads", states[1].get("url"))
        check(states[2].get("url", "").endswith("/09-article.html") and states[2].get("forward"),
              "Backspace goes back", states[2].get("url"))
        check(states[3].get("url", "").endswith("/11-table.html"), "L goes forward again")
        check(states[4].get("prompt") is None and states[4].get("url", "").endswith("/11-table.html"),
              "Escape closes the address line without navigating")
        check(states[5].get("help") and not states[6].get("help"), "? shows the keys and any key closes them")

    # Forms: a field edited in place, Enter submitting with the field as the
    # submitter, a button submitting as itself -- the desktop's contract, and
    # the terminal's: the same requests from the same form.
    r = fb(f"{base}/13-form.html", [
        "wait", "send \\t", "send \\x15a b&c/d", "STATE", "SHOT typed", "send \\r", "wait", "STATE",
        "send \\x7f", "wait", "send \\t\\t", "STATE", "send \\r", "wait", "STATE", "quit"])
    s = r.states
    if check(len(s) == 4, "the form script ran", r.process.stderr[-300:]):
        typed = [c for c in s[0].get("controls", []) if c["name"] == "q"]
        check(s[0].get("editing") and typed and typed[0]["value"] == "a b&c/d",
              "a focused field is edited in place", s[0])
        check(s[1].get("url", "").endswith("search?q=a+b%26c%2Fd&token=CSRF-PASSTHROUGH"),
              "Enter in a field submits GET with the typed value and the hidden token", s[1].get("url"))
        check((s[2].get("focus") or {}).get("type") == "submit", "Tab reaches the button", s[2].get("focus"))
        check(s[3].get("url", "").endswith("search?q=reader&token=CSRF-PASSTHROUGH&go=Search"),
              "the button submits as itself", s[3].get("url"))

    # A POST with a password, and a session carried across engine runs.
    r = fb(f"{base}/login", ["wait", "send \\t", "send corpus\\t", "send fixture-password", "STATE",
                             "SHOT password", "send \\r", "wait", "STATE", "quit"])
    s = r.states
    if check(len(s) == 2, "the login script ran", r.process.stderr[-300:]):
        password = [c for c in s[0].get("controls", []) if c["name"] == "password"]
        check(password and password[0]["value"] == "•" * len("fixture-password"),
              "a password is masked, on screen and in the state line", password)
        check(s[1].get("url", "").endswith("/account") and s[1].get("title") == "Account",
              "a POST login carries its session cookie", s[1].get("url"))

    # Choices and lists: a select opened, stepped and chosen; a checkbox
    # toggled by the keyboard and a radio by a click on the screen.
    r = fb(f"{base}/forms-lab.html", ["wait"] + ["send \\t", "STATE"] * 24 + ["quit"], height=900)
    focus = [(st.get("focus") or {}) for st in r.states]
    tabs_to = {}
    for index, f in enumerate(focus):
        tabs_to.setdefault((f.get("name"), f.get("type")), index + 1)
    select = tabs_to.get(("frequency", "select-one")) or next(
        (v for k, v in tabs_to.items() if k[0] == "frequency"), None)
    checkbox = tabs_to.get(("topic", "checkbox"))
    radios = [index + 1 for index, f in enumerate(focus) if f.get("type") == "radio"]
    if check(select and checkbox and len(radios) >= 2, "the choices are all in the Tab order", tabs_to):
        steps = ["wait"] + ["send \\t"] * select + ["send \\r", "STATE", "send \\e[B", "send \\r", "STATE"]
        steps += ["send \\e", "send g"] + ["send \\t"] * checkbox + ["send \\x20", "STATE"]
        r = fb(f"{base}/forms-lab.html", steps + ["quit"], height=900)
        s = r.states
        if check(len(s) == 3, "the choices script ran", r.process.stderr[-300:]):
            before = next(c["value"] for c in s[0]["controls"] if c["name"] == "frequency")
            after = next(c["value"] for c in s[1]["controls"] if c["name"] == "frequency")
            check(s[0].get("popup") and not s[1].get("popup") and after != before,
                  "Enter opens a list and Enter chooses from it", (before, after))
            was = [c["checked"] for c in s[1]["controls"] if c["name"] == "topic"]
            now = [c["checked"] for c in s[2]["controls"] if c["name"] == "topic"]
            check(was != now, "Space toggles a checkbox", (was, now))
        # A click on the screen, at the second radio button, found through
        # the state's own geometry: the first is the one checked to begin.
        r = fb(f"{base}/forms-lab.html", ["wait"] + ["send \\t"] * radios[1] + ["STATE", "quit"], height=900)
        if check(len(r.states) == 1, "the radio was found", r.process.stderr[-300:]):
            x, y = screen_point(r.states[0])
            scrolled = r.states[0].get("scroll", 0)
            r = fb(f"{base}/forms-lab.html", ["wait"] + ["send \\t"] * radios[1] + ["send \\e", "STATE",
                   f"click {x} {y}", "STATE", "quit"], height=900)
            s = r.states
            if check(len(s) == 2 and s[0].get("scroll") == scrolled, "the page stayed where the radio was"):
                modes = [c["checked"] for c in s[1]["controls"] if c["name"] == "mode"]
                check(modes[:2] == [False, True] and (s[1].get("focus") or {}).get("type") == "radio",
                      "a click on a radio button chooses it", modes)

    # The mouse as Muroxi and a terminal send it: SGR reports in cells.
    r = fb(f"{base}/15-gallery.html", ["wait", "send \\t", "STATE", "send \\e", "quit"])
    if check(r.states and (r.state(0).get("focus") or {}).get("kind") == "link", "the gallery has a link to find"):
        x, y = screen_point(r.state(0))
        column, row = x // 8 + 1, y // 16 + 1
        r = fb(f"{base}/15-gallery.html", ["wait", f"send \\e[<35;{column};{row}M", "STATE",
                                           f"send \\e[<0;{column};{row}M\\e[<0;{column};{row}m", "wait",
                                           "STATE", "SHOT viewer", "send n", "wait", "STATE", "send \\e",
                                           "STATE", f"send \\e[<65;{column};{row}M", "STATE", "quit"])
        s = r.states
        if check(len(s) == 5, "the viewer script ran", r.process.stderr[-300:]):
            check(s[0].get("hover", "").startswith("distill-media:"), "a pointer over a link shows where it goes",
                  s[0].get("hover"))
            viewer = s[1].get("viewer") or {}
            check(viewer.get("loaded") and viewer.get("count") == 4 and viewer.get("width", 0) > 0,
                  "a click on an image link opens it in the viewer", viewer)
            shot = r.shot("viewer")
            if shot:
                page_rgb = pixel(shot, 2, 300)
                cx, cy = WIDTH // 2, s[1].get("view_top", 0) + (s[1].get("viewport", 0) - 25) // 2
                check(pixel(shot, cx, cy) != page_rgb, "the image is drawn in the middle of the view")
            check((s[2].get("viewer") or {}).get("index") == 1, "n steps to the next image")
            check(s[3].get("viewer") is None, "Escape leaves the viewer")
            check(s[4].get("scroll", 0) >= s[3].get("scroll", 0), "an SGR wheel report scrolls", s[4].get("scroll"))

    # Audio: fetch, play through the null backend, the transport.
    r = fb(f"{base}/media/41-tone.wav", ["wait", "send \\t", "send \\r", "wait", "STATE", "SHOT playing",
                                         "send x", "STATE", "quit"])
    s = r.states
    if check(len(s) == 2, "the audio script ran", r.process.stderr[-300:]):
        audio = s[0].get("audio") or {}
        check(audio.get("phase") in ("playing", "finished") and audio.get("title") == "41-tone.wav",
              "Play fetches the file and shows the transport", audio)
        check(s[0].get("viewport", 0) < s[1].get("viewport", 0) and s[1].get("audio") is None,
              "x closes the track and gives the page its room back", (s[0].get("viewport"), s[1].get("viewport")))

    # A resize relays the page out; a VT switch away stops drawing.
    r = fb(f"{base}/09-article.html", ["wait", "STATE", "resize 500 600", "STATE", "SHOT small",
                                       "suspend", "send G", "STATE", "SHOT away", "resume", "STATE",
                                       "SHOT back", "quit"])
    s = r.states
    if check(len(s) == 4, "the resize script ran", r.process.stderr[-300:]):
        check(s[1].get("width") == 500 and s[1].get("page_height", 0) > s[0].get("page_height", 0),
              "a resize reflows the page to the new width", (s[0].get("page_height"), s[1].get("page_height")))
        small, away, back = r.shot("small"), r.shot("away"), r.shot("back")
        check(small is not None and small[:2] == (500, 600), "the screenshot follows the device's size")
        check(s[2].get("suspended") and away is not None and small is not None and away[2] == small[2],
              "nothing is drawn while suspended", s[2].get("scroll"))
        check(not s[3].get("suspended") and back is not None and away is not None and back[2] != away[2],
              "everything is redrawn on resume")

    # The theme and the type size reload the page, and are remembered.
    r = fb(f"{base}/09-article.html", ["wait", "SHOT warm", "send t", "wait", "STATE", "SHOT night",
                                       "send +", "wait", "STATE", "send 0", "wait", "quit"])
    s = r.states
    if check(len(s) == 2, "the theme script ran", r.process.stderr[-300:]):
        warm, night = r.shot("warm"), r.shot("night")
        check(s[0].get("theme") == "night" and warm and night and pixel(warm, 2, 300) != pixel(night, 2, 300),
              "t changes the theme and the page is drawn in it")
        check(s[1].get("zoom", 0) > 100, "+ makes the type larger", s[1].get("zoom"))
        settings = os.path.join(home, "config", "distill", "settings.ini")
        text = open(settings, encoding="utf-8").read() if os.path.exists(settings) else ""
        check("theme=night" in text, "the theme is saved to the shared settings file", text)

    # The desktop's two menus, by the keyboard: F10 opens settings, the
    # arrows and Right go into a submenu, Esc comes back out one level.
    config = os.path.join(home, "config", "distill", "settings.ini")
    r = fb(f"{base}/09-article.html", ["wait", "STATE", "SHOT closed", "send \\e[21~", "STATE", "SHOT open",
                                       "send \\e[B", "send \\e[B", "send \\e[C", "STATE", "send \\e", "STATE",
                                       "send \\e", "STATE",
                                       "send \\e[21~", "send \\e[F", "send \\r", "send \\e[B", "send \\e[B",
                                       "send \\r", "STATE",
                                       "send \\e[21~", "send \\e[B", "send \\e[B", "send \\e[B", "send \\e[C",
                                       "send \\e[B", "send \\e[B", "send \\r", "wait", "STATE",
                                       "send +", "wait", "STATE", "quit"])
    s = r.states
    if check(len(s) == 8, "the menu keys script ran", r.process.stderr[-300:]):
        menu = s[1].get("menu") or {}
        check(s[0].get("menu") is None and menu.get("open") == "settings" and
              [i["label"] for i in menu.get("items", [])] ==
              ["Open externally", "Export page", "Theme", "Zoom", "Start page"] and
              menu["items"][0]["selected"],
              "F10 opens the settings menu with the desktop's items, the first selected", menu)
        closed, opened = r.shot("closed"), r.shot("open")
        if closed and opened and menu.get("items"):
            item = menu["items"][0]
            check(pixel(closed, item["x"] + 4, item["y"] + 2) != pixel(opened, item["x"] + 4, item["y"] + 2),
                  "the open menu is drawn over the page")
        sub = s[2].get("menu") or {}
        check(sub.get("submenu") == "theme" and sub.get("in_submenu") and
              [i["label"] for i in sub.get("subitems", []) if i["checked"]] ==
              [{"warm": "Warm (Light)", "night": "Night (Dark)", "paper": "Paper (Light)"}[s[2]["theme"]]],
              "Right opens Theme with the current theme chosen", sub)
        check((s[3].get("menu") or {}).get("submenu") is None and (s[3].get("menu") or {}).get("open") == "settings"
              and s[4].get("menu") is None, "Esc closes the submenu, then the menu",
              (s[3].get("menu"), s[4].get("menu")))
        text = open(config, encoding="utf-8").read() if os.path.exists(config) else ""
        check(s[5].get("menu") is None and "start-page=blank" in text,
              "Start page, Start blank is saved to the shared settings file", text)
        check(s[6].get("zoom") == 125, "Zoom, 125% through the menu sets the zoom", s[6].get("zoom"))
        check(s[7].get("zoom") == 150 and "zoom=150" in open(config, encoding="utf-8").read(),
              "+ steps through the menu's levels, and the zoom is saved", s[7].get("zoom"))

    # The same by the pointer: a click opens a menu, resting on an item
    # opens its submenu, a click chooses, and a click elsewhere closes.
    r = fb(f"{base}/09-article.html", ["wait", "send \\e[21~", "STATE", "send \\e", "send 0", "wait", "quit"])
    geometry = r.state(0)
    menu = geometry.get("menu") or {}
    buttons = geometry.get("buttons") or {}
    if check(len(menu.get("items", [])) == 5 and buttons.get("settings"), "the menu geometry was read", geometry):
        sx, sy, sw, sh = buttons["settings"]
        cx, cy, cw, ch = buttons["sections"]
        theme = menu["items"][2]
        tx, ty = theme["x"] + theme["w"] // 2, theme["y"] + theme["h"] // 2
        paper_y = theme["y"] + 2 * theme["h"] + theme["h"] // 2
        r = fb(f"{base}/09-article.html", [
            "wait", "send G", f"click {sx + sw // 2} {sy + sh // 2}", "STATE", f"move {tx} {ty}", "STATE",
            f"move {theme['x'] - 40} {ty}", f"move {theme['x'] - 40} {paper_y}", f"click {theme['x'] - 40} {paper_y}",
            "wait", "STATE", "SHOT paper",
            f"click {cx + cw // 2} {cy + ch // 2}", "STATE", f"click 300 {HEIGHT // 2}", "STATE",
            "send G", f"click {cx + cw // 2} {cy + ch // 2}", "STATE", "quit"])
        s = r.states
        if check(len(s) == 6, "the menu pointer script ran", r.process.stderr[-300:]):
            m0, m1 = s[0].get("menu") or {}, s[1].get("menu") or {}
            check(m0.get("open") == "settings" and m0.get("submenu") is None, "a click on the cog opens settings", m0)
            check(m1.get("submenu") == "theme" and not m1.get("in_submenu"), "resting on Theme opens its submenu", m1)
            check(s[2].get("theme") == "paper" and s[2].get("menu") is None and
                  "theme=paper" in open(config, encoding="utf-8").read(),
                  "a click on Paper (Light) chooses and saves it", (s[2].get("theme"), s[2].get("menu")))
            sections = s[3].get("menu") or {}
            items = {i["label"]: i for i in sections.get("items", [])}
            check(sections.get("open") == "sections" and list(items) ==
                  ["Top", "Images", "Tables", "Forms", "Videos", "Results", "Links"] and
                  items["Top"]["enabled"] and not items["Tables"]["enabled"],
                  "the sections menu lists the desktop's sections, and only those on the page are enabled",
                  sections)
            check(s[4].get("menu") is None and s[4].get("url") == s[3].get("url") and
                  s[4].get("focus") is None, "a click outside closes the menu and does nothing else", s[4].get("menu"))
            top = items.get("Top", {})
            r2 = fb(f"{base}/09-article.html", [
                "wait", "send G", "STATE", f"click {cx + cw // 2} {cy + ch // 2}",
                f"click {top.get('x', 0) + 10} {top.get('y', 0) + top.get('h', 0) // 2}", "STATE", "quit"])
            t = r2.states
            check(len(t) == 2 and t[0].get("scroll", 0) > 0 and t[1].get("scroll") == 0 and t[1].get("menu") is None,
                  "Top in the sections menu goes to the top", [x.get("scroll") for x in t])

    # Export page, as HTML and as Markdown, to a name typed at the prompt.
    exported_html = os.path.join(scratch, "exported.html")
    exported_md = os.path.join(scratch, "exported.md")
    r = fb(f"{base}/09-article.html", ["wait", "send \\e[21~", "send \\e[B", "send \\e[C", "send \\r", "STATE",
                                       "send \\x15", f"send {exported_html}", "send \\r", "STATE",
                                       "send \\e[21~", "send \\e[B", "send \\e[C", "send \\e[B", "send \\r",
                                       "STATE", "send \\x15", f"send {exported_md}", "send \\r", "STATE", "quit"])
    s = r.states
    if check(len(s) == 4, "the export script ran", r.process.stderr[-300:]):
        prompt = s[0].get("prompt") or {}
        check(prompt.get("kind") == "export-html" and prompt.get("text", "").endswith("09-article.html"),
              "As HTML asks where, suggesting the page's name", prompt)
        html = open(exported_html, encoding="utf-8").read() if os.path.exists(exported_html) else ""
        check("A careful reader" in html and "<style" in html, "the reader page is exported as HTML",
              s[1].get("status"))
        prompt = s[2].get("prompt") or {}
        check(prompt.get("kind") == "export-markdown" and prompt.get("text", "").endswith("09-article.md"),
              "As Markdown suggests a .md name", prompt)
        md = open(exported_md, encoding="utf-8").read() if os.path.exists(exported_md) else ""
        check("A careful reader" in md and "<" not in md.split("\n", 1)[0],
              "the reader page is exported as Markdown by the engine", (s[3].get("status"), md[:120]))

    # The help that was the ? button is now the status bar's "? keys".
    r = fb(f"{base}/09-article.html", ["wait", f"click {WIDTH - 20} {HEIGHT - 8}", "STATE", "quit"])
    check(r.state(0).get("help") is True, "a click on ? keys shows the keys", r.state(0).get("help"))

    # Every clean run above removed its private directory.
    leftovers = os.listdir(tmp)
    check(not leftovers, "clean exits leave no temporary files behind", ", ".join(leftovers))

    # A crash takes the session's cookies with it.
    for sig, name in ((signal.SIGTERM, "SIGTERM"), (signal.SIGSEGV, "SIGSEGV")):
        before = set(os.listdir(tmp))
        script = os.path.join(scratch, f"crash-{name}")
        with open(script, "w", encoding="utf-8") as handle:
            handle.write("wait\nsleep 20000\nquit\n")
        command = [binary, "--display", f"memory:640x480,script={script}", f"{base}/09-article.html"]
        process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                   preexec_fn=limit_stack)
        own = []
        for _ in range(100):
            own = [os.path.join(tmp, d) for d in set(os.listdir(tmp)) - before]
            sessions = [f for d in own if os.path.isdir(d) for f in os.listdir(d) if f.startswith("distill-session")]
            if sessions:
                break
            time.sleep(0.05)
        time.sleep(0.8)
        process.send_signal(sig)
        status = process.wait(timeout=20)
        check(status == -sig, f"{name} still ends the process by that signal", status)
        remaining = [f for d in own if os.path.isdir(d) for f in os.listdir(d) if f.startswith("distill-session")]
        check(own and not remaining, f"{name} removes the session's cookie file", remaining)
        for d in own:
            for f in os.listdir(d) if os.path.isdir(d) else []:
                os.remove(os.path.join(d, f))
            if os.path.isdir(d):
                os.rmdir(d)

    # Only a virtual console is a console to draw on.
    result = subprocess.run([binary, f"{base}/09-article.html"], env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=20)
    check(result.returncode == 1 and "virtual console" in result.stderr,
          "outside a virtual console it refuses, and says where to run it", result.stderr.strip())
    result = subprocess.run([binary, "--display", "wayland"], env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=20)
    check(result.returncode == 1 and "no display" in result.stderr,
          "an unknown display is refused by name", result.stderr.strip())
    shutil.rmtree(scratch, ignore_errors=True)
    return check.results
