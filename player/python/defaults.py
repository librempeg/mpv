"""
The python wrapper module for the embedded and extended functionalities
"""


import mpv as _mpv
from pathlib import Path

client_name = Path(_mpv.filename).stem


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
        with Path(filename).open("r") as f:
            return f.read()

    def extension_ok(self) -> bool:
        return _mpv.extension_ok()

    def process_event(self, event_id: int):
        self.debug(f"received event: {event_id}")


mpv = Mpv()
mpv.info(f"okay from extension {client_name}: {mpv.extension_ok()}")
