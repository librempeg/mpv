import threading
import mpvmainloop


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

    def info(self, *args):
        mpvmainloop.handle_log(['info', ' '.join([str(msg) for msg in args])])

    def debug(self, *args):
        mpvmainloop.handle_log(['debug', ' '.join([str(msg) for msg in args])])

    def warn(self, *args):
        mpvmainloop.handle_log(['warn', ' '.join([str(msg) for msg in args])])

    def error(self, *a):
        mpvmainloop.handle_log(['error', ' '.join([str(msg) for msg in a])])

    def fatal(self, *a):
        mpvmainloop.handle_log(['fatal', ' '.join([str(msg) for msg in a])])

    clients = []

    def __init__(self, clients):
        self.clients = clients

    def get_client_index(self, client_name):
        if client_name not in self.clients:
            raise Exception
        return self.clients.index(client_name)

    def handle_event(self, event_id, data):
        """
        Returns:
            boolean specifing whether some event loop breaking
            condition has been satisfied.
        """
        self.debug(f"event_id: {event_id}")
        if event_id == self.MPV_EVENT_SHUTDOWN:
            self.info("shutting down python")
            return True
        elif event_id == self.MPV_EVENT_NONE:
            return False
        else:
            mpvmainloop.notify_clients(event_id, data)
        return False

    initialized = False

    def wait_events(self):
        while True:
            event_id, data = mpvmainloop.wait_event(-1)
            if self.handle_event(event_id, data):
                break

    def run(self):
        self.wait_events()


ml = MainLoop(mpvmainloop.clients)
