#!/usr/bin/env python3
"""Signal only a birth-time/ownership-verified process through its Linux pidfd.

tcpdump's privilege drop can make /proc/PID/exe unreadable even to a container's
root. Only that specific case permits the stricter exact-argv/comm fallback.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal


class IdentityError(ValueError):
    pass


def capture_footer(text):
    """tcpdump uses singular 'packet' for one; absent footer fields are unknown."""
    captured = re.search(r'(\d+) packets? captured', text)
    drops = re.search(r'(\d+) packets? dropped by kernel', text)
    return dict(captured_packets=int(captured[1]) if captured else None,
                kernel_drops=int(drops[1]) if drops else None)


def checked_record(record, owner):
    if record.get('owner') != owner or not isinstance(owner, str) or not owner:
        raise IdentityError('process receipt ownership differs')
    for name in ('pid', 'start_ticks'):
        value = record.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise IdentityError('positive process PID and birth time required')
    executable = record.get('executable')
    if not isinstance(executable, str) or not PurePosixPath(executable).is_absolute():
        raise IdentityError('absolute recorded executable required')
    arguments = record.get('arguments')
    if not isinstance(arguments, list) or any(not isinstance(x, str) or '\0' in x for x in arguments):
        raise IdentityError('exact recorded argument vector required')


def checked_snapshot(record, owner, stat, *, executable=None, exe_denied=False,
                     argv=None, comm=None):
    """Pure identity validation used by the pidfd path and deterministic tests."""
    checked_record(record, owner)
    fields = stat.rsplit(')', 1)[1].split()
    if int(fields[19]) != record['start_ticks']:
        raise IdentityError('process birth time differs; refusing to signal')
    if fields[0] in ('Z', 'X'):
        return dict(alive=False, outcome='ALREADY_EXITED', pid=record['pid'])
    capture = PurePosixPath(record['executable']).name == 'tcpdump'
    if exe_denied and not capture:
        raise IdentityError('executable access denied for a non-capture process')
    if not exe_denied and executable != record['executable']:
        raise IdentityError('process executable differs; refusing to signal')
    if capture:
        if argv != [record['executable'], *record['arguments']] or comm != 'tcpdump':
            raise IdentityError('capture argv or comm differs; refusing to signal')
    guard = 'pidfd + owner + birth time + executable'
    if capture:
        guard = 'pidfd + owner + exact birth time + exact argv + tcpdump comm'
        guard += ' (exe denied after privilege drop)' if exe_denied else ' + executable'
    return dict(alive=True, outcome='SAME_OWNED_PROCESS', pid=record['pid'],
                start_ticks=int(fields[19]), identity_guard=guard,
                executable=record['executable'], executable_access_denied=exe_denied)


def action(record, owner, sig=0, *, proc_root=Path('/proc'),
           pidfd_open=None, pidfd_send_signal=None, close=None):
    checked_record(record, owner)
    if pidfd_open is None:
        pidfd_open = os.pidfd_open
    if pidfd_send_signal is None:
        pidfd_send_signal = signal.pidfd_send_signal
    close = close or os.close
    try:
        fd = pidfd_open(record['pid'])
    except ProcessLookupError:
        return dict(alive=False, outcome='ALREADY_EXITED', pid=record['pid'])
    try:
        process = proc_root / str(record['pid'])
        try:
            stat = (process / 'stat').read_text()
        except FileNotFoundError:
            return dict(alive=False, outcome='ALREADY_EXITED', pid=record['pid'])
        fields = stat.rsplit(')', 1)[1].split()
        if fields[0] in ('Z', 'X'):
            return checked_snapshot(record, owner, stat)
        denied = False
        executable = None
        try:
            executable = str((process / 'exe').resolve(strict=True))
        except PermissionError:
            denied = True
        except FileNotFoundError:
            return dict(alive=False, outcome='ALREADY_EXITED', pid=record['pid'])
        argv = comm = None
        if PurePosixPath(record['executable']).name == 'tcpdump':
            argv = [x.decode('utf-8', 'surrogateescape')
                    for x in (process / 'cmdline').read_bytes().split(b'\0') if x]
            comm = (process / 'comm').read_text().strip()
        receipt = checked_snapshot(record, owner, stat, executable=executable,
                                   exe_denied=denied, argv=argv, comm=comm)
        if sig:
            try:
                pidfd_send_signal(fd, sig)
            except ProcessLookupError:
                return dict(receipt, alive=False, outcome='ALREADY_EXITED_BEFORE_SIGNAL')
            receipt.update(outcome='SIGNALED_OWNED_PROCESS', signal=int(sig))
        return receipt
    finally:
        close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--signal', type=int, choices=[0, 2, 9, 15], default=0)
    args = parser.parse_args()
    result = action(json.loads(args.receipt.read_text()), args.owner, args.signal)
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    main()
