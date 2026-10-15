"""Validate the bound listener published by the owned Microsoft test host.

The server binds port zero itself and publishes only after bind/listen succeed.
Reading this file never opens a TCP connection or consumes a test's accept.
"""

import json
import os
from pathlib import Path
import secrets
import stat

FILE_ENV = "FREERDP_WPTS_LISTENER_FILE"
TOKEN_ENV = "FREERDP_WPTS_LISTENER_TOKEN"
MAX_RECORD = 4096


class ListenerHandshake:
    def __init__(self, path):
        self.path = Path(path).resolve()
        if self.path.exists():
            raise ValueError("Listener readiness path already exists")
        self.token = secrets.token_hex(32)
        self.driver_group = None
        self.record = None

    @property
    def environment(self):
        return {FILE_ENV: str(self.path), TOKEN_ENV: self.token}

    def read(self, *, require_alive=True):
        # The containing result directory is private and the path is unique.
        # Reject links/oversized records rather than following arbitrary files.
        descriptor = os.open(self.path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(descriptor, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_RECORD:
                raise ValueError("Invalid listener readiness file")
            data = stream.read(MAX_RECORD + 1)
            if len(data) > MAX_RECORD:
                raise ValueError("Invalid listener readiness file")
            record = json.loads(data)
        if not isinstance(record, dict) or (type(record.get("version")) is not int or record["version"] != 1):
            raise ValueError("Unsupported listener readiness record")
        token = record.get("token")
        if not isinstance(token, str) or not token.isascii() or not secrets.compare_digest(token, self.token):
            raise ValueError("Listener readiness token does not match this case")
        if record.get("address") != "127.0.0.1":
            raise ValueError("Listener must bind IPv4 loopback")
        port, pid = record.get("port"), record.get("pid")
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("Listener did not publish a concrete TCP port")
        if type(pid) is not int or pid <= 0:
            raise ValueError("Invalid listener process ID")
        if require_alive:
            if self.driver_group is None or os.getpgid(pid) != self.driver_group:
                raise ValueError("Listener process does not belong to this driver")
        public = {key: record[key] for key in ("version", "address", "port", "pid")}
        if self.record is not None and self.record != public:
            raise ValueError("Listener endpoint changed during this case")
        self.record = public
        return public

    def snapshot(self):
        if self.record is not None:
            return self.record
        try:
            return self.read(require_alive=False)
        except (OSError, ValueError) as error:
            return {"error": str(error)}
