import mpvmainloop, time

def log_handle(lname: str, *args):
    mpvmainloop.handle_log([lname, ' '.join([str(msg) for msg in args])])

def test_warn(*args):
    log_handle('warn', *args)


class MainLoop(object):
    MPV_EVENT_NONE = 0
    MPV_EVENT_SHUTDOWN = 1
    MPV_EVENT_LOG_MESSAGE = 2
    MPV_EVENT_GET_PROPERTY_REPLY = 3
    MPV_EVENT_SET_PROPERTY_REPLY = 4
    MPV_EVENT_COMMAND_REPLY = 5
    MPV_EVENT_START_FILE = 6
    MPV_EVENT_END_FILE = 7
    MPV_EVENT_FILE_LOADED = 8
    MPV_EVENT_CLIENT_MESSAGE = 16
    MPV_EVENT_VIDEO_RECONFIG = 17
    MPV_EVENT_AUDIO_RECONFIG = 18
    MPV_EVENT_SEEK = 20
    MPV_EVENT_PLAYBACK_RESTART = 21
    MPV_EVENT_PROPERTY_CHANGE = 22
    MPV_EVENT_QUEUE_OVERFLOW = 24
    MPV_EVENT_HOOK = 25

    def sleep(self, sec):
        from time import sleep
        sleep(sec)

    def info(self, *args):
        mpvmainloop.handle_log(['info', ' '.join([str(msg) for msg in args])])

    def debug(self, *args):
        mpvmainloop.handle_log(['debug', ' '.join([str(msg) for msg in args])])

    def warn(self, *args):
        mpvmainloop.handle_log(['warn', ' '.join([str(msg) for msg in args])])

    def error(self, *a):
        mpvmainloop.handle_log(['error', ' '.join([str(msg) for msg in args])])

    def fatal(self, *a):
        mpvmainloop.handle_log(['fatal', ' '.join([str(msg) for msg in args])])


    clients: list[str] = []

    def __init__(self, clients):
        if clients is not None:
            self.clients = clients

    def get_client_index(self, client_name):
        if client_name not in self.clients:
            raise Exception
        return self.clients.index(client_name)

    def handle_event(self, event_id: int) -> bool:
        """
        Returns:
            boolean specifing whether some event loop breaking
            condition has been satisfied.
        """
        if event_id == self.MPV_EVENT_SHUTDOWN:
            self.info("shutting down python")
            return True
        return False

    def wait_events(self):
        while True:
            event_id = mpvmainloop.wait_event(100)
            if self.handle_event(event_id):
                break

    def run(self):
        # self.debug("sleeping 1 sec")
        # self.sleep(1)  # this works
        # time.sleep(1)  # but this doesn't
        # conclusion, PyRun_* looses the global frame.
        # self.debug("slept 1 sec")
        self.wait_events()
        # mpvmainloop.handle_log(["warn", "initiating shutdown sequence"])
        self.shutdown()

    def shutdown(self):
        mpvmainloop.shutdown()

ml = MainLoop(globals()['clients'])
ml.debug("running main loop")
ml.run()
