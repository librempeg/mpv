"""
The python wrapper module for the embedded and extended functionalities
"""


import mpv as _mpv
from pathlib import Path

# __all__ = ['client_name', 'mpv']

client_name = Path(_mpv.filename).stem

class Registry:
    script_message = {}
    binds = {}

    def flush_key_bindings(self):
        forced = []
        reg = []
        for name, value in binds.items():
            if value.get("input"):
                if value.get("forced"):
                    forced.append(value)
                else:
                    reg.append(value)
        reg = sorted([v['input'] for v in reg])
        forced = sorted([v['input'] for v in forced])

        section_name = "input_" + client_name

        self.commandv("define-section", section_name, "\n".join(reg), "default")
        self.commandv("enable-section", section_name, "allow-hide-cursor+allow-vo-dragging")

        section_name = "input_forced_" + client_name

        self.commandv("define-section", section_name, "\n".join(forced), "forced")
        self.commandv("enable-section", section_name, "allow-hide-cursor+allow-vo-dragging")

    def commandv(self, *args):
        _mpv.commandv(*args)


registry = Registry()


class Mpv:
    """

    This class wraps the mpv client (/libmpv) API hooks defined in the
    embedded/extended python. See: player/python.c

    """

    def log(self, level, *args):
        msg = ' '.join([str(msg) for msg in args])
        _mpv.handle_log([level, f"({client_name}) {msg}"])

    def info(self, *args):
        self.log("info", *args)

    def debug(self, *args):
        self.log("debug", *args)

    def warn(self, *args):
        self.log("warn", *args)

    def error(self, *a):
        self.log("error", *a)

    def fatal(self, *a):
        self.log("fatal", *a)

    def read_script(self, filename):
        file_path = Path(filename).resolve()
        if file_path.is_dir():
            file_path = file_path / "main.py"
        with file_path.open("r") as f:
            return str(file_path), f.read()

    def extension_ok(self) -> bool:
        return _mpv.extension_ok()

    def process_event(self, event_id):
        self.debug(f"received event: {event_id}")

    next_bid = 1

    def add_binding(self, key=None, name=None, forced=False, **opts):
        """
        Args:
            opts: boolean memebers (repeatable, complex)
        """
        # copied from defaults.js (not sure what e and emit is yet)
        key_data = opts
        self.next_bid += 1
        key_data.update(forced=forced, id=self.next_bid)
        if name is None:
            name = f"__keybinding{self.next_bid}"  # unique name

        def decorate(fn):
            registry.script_message[name] = fn

            def key_cb(state):
                e = state[0]
                emit = state[1] == "m" if e == "u" else e == "d"
                if (emit or e == "p" or e == "r") and key_data.get("repeatable", False):
                    fn()
            key_data['cb'] = key_cb

        if key is not None:
            key_data["input"] = key + " script-binding " + client_name + "/" + name
        registry.binds[name] = key_data

        return decorate


mpv = Mpv()
mpv.info(f"okay from extension {client_name}: {mpv.extension_ok()}")
