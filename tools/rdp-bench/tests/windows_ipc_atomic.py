#!/usr/bin/env python3
"""Real Windows sharing-lock regressions in one new, owned test directory.

Run only between benchmark campaigns. Deploy an isolated copy of rdpbench
live/bridge/common.py and windows/agent.py alongside this tests directory;
the currently running workload agent and its IPC paths are never touched.
"""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import stat
import sys
import threading
import time
import uuid

SUITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SUITE))
from rdpbench.live import ipc_publish_script


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="New, isolated test directory; must not exist")
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("this regression requires real Windows sharing semantics")
    root = Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location("isolated_windows_agent", SUITE / "windows/agent.py")
    agent = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(agent)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                 ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int

    def hold(path):
        # Read/write sharing deliberately omits DELETE sharing. Windows must
        # refuse atomic replacement until this owned handle has closed.
        handle = kernel.CreateFileW(str(path), 0x80000000, 3, None, 3, 0x80, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            raise ctypes.WinError(ctypes.get_last_error())
        closed = threading.Event()
        observations = []
        def release():
            try:
                observations.append(json.loads(path.read_text()))
            finally:
                if not kernel.CloseHandle(handle):
                    raise ctypes.WinError(ctypes.get_last_error())
                closed.set()
        return release, closed, observations

    results = []
    target = root / "python-status.json"
    before, after = {"phase": "old"}, {"phase": "new", "padding": "x" * 4096}
    target.write_text(json.dumps(before))
    release, closed, observations = hold(target)
    attempted_codes = []
    sharing_probes = []
    calls = 0
    original = agent.os.replace
    original_probe = agent.target_sharing_violation
    def probe(path):
        answer=original_probe(path)
        sharing_probes.append(answer)
        return answer
    def counted(source, destination):
        nonlocal calls
        calls += 1
        try:
            return original(source, destination)
        except OSError as error:
            attempted_codes.append(error.winerror)
            raise
    agent.os.replace = counted
    agent.target_sharing_violation = probe
    timer = threading.Timer(.35, release)
    timer.start()
    begin = time.monotonic()
    try:
        agent.write(target, after)
    finally:
        agent.os.replace = original
        agent.target_sharing_violation = original_probe
        timer.join(2)
        if not closed.is_set():
            release()
    assert attempted_codes and set(attempted_codes) <= {5, 32, 33}, attempted_codes
    assert 5 not in attempted_codes or (sharing_probes and all(sharing_probes)), sharing_probes
    assert 2 <= calls <= 24, calls
    assert observations == [before], observations
    assert json.loads(target.read_text()) == after
    results.append(dict(test="agent_atomic_status_locked_release", passed=True,
                        attempts=calls, transient_codes=attempted_codes,
                        access_denied_sharing_confirmed=sharing_probes,
                        seconds=time.monotonic() - begin, old_visible_until_release=True))

    target = root / "controller-command.json"
    target.write_text(json.dumps(before))
    script = root / "controller-publish.ps1"
    started = root / "controller-started.flag"
    body = ipc_publish_script(str(target), json.dumps(after).encode(), uuid.uuid4().hex)
    marker_path = str(started).replace("'", "''")
    script.write_text("$ErrorActionPreference='Stop';\n"
                     f"[IO.File]::WriteAllText('{marker_path}','ready');\n" + body + "\n"
                     "[pscustomobject]@{attempts=($ipcAttempt+1);lastTransientCode=$ipcCode}|ConvertTo-Json -Compress\n", encoding="utf-8")
    release, closed, observations = hold(target)
    process = subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+5
        while not started.is_file() and process.poll() is None and time.monotonic()<deadline:
            time.sleep(.01)
        assert started.is_file(), "publisher did not begin its owned file operation"
        # Prove the publisher cannot erase or partially overwrite the locked file.
        time.sleep(.25)
        assert process.poll() is None, "publisher exited while replacement target remained locked"
        assert json.loads(target.read_text()) == before
    finally:
        release()
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode == 0, stderr.decode("utf-8", "replace")
    assert observations == [before], observations
    assert json.loads(target.read_text()) == after
    controller = json.loads(stdout.decode("utf-8-sig"))
    assert 2 <= controller["attempts"] <= 24, controller
    assert controller["lastTransientCode"] in (32,33), controller
    results.append(dict(test="controller_atomic_command_locked_release", passed=True,
                        old_visible_until_release=True, publisher_waited_for_release=True,
                        attempts=controller["attempts"], transient_code=controller["lastTransientCode"]))

    target = root / "bounded-status.json"
    source = root / "bounded-status.new"
    target.write_text(json.dumps(before))
    source.write_text(json.dumps(after))
    release, closed, observations = hold(target)
    try:
        try:
            agent.atomic_replace(source,target,attempts=3)
        except TimeoutError as error:
            assert "after 3 attempts" in str(error), error
        else:
            raise AssertionError("replacement succeeded despite unreleased DELETE-sharing lock")
        assert json.loads(target.read_text()) == before
        assert source.is_file()
    finally:
        release()
    results.append(dict(test="agent_atomic_status_bounded_lock_failure",passed=True,
                        attempts=3,old_target_preserved=True,new_temp_preserved=True))

    target = root / "readonly-status.json"
    source = root / "readonly-status.new"
    target.write_text(json.dumps(before))
    source.write_text(json.dumps(after))
    os.chmod(target,stat.S_IREAD)
    calls = 0
    attempted_codes = []
    agent.os.replace = counted
    try:
        try:
            agent.atomic_replace(source,target)
        except OSError as error:
            assert error.winerror == 5, error
        else:
            raise AssertionError("read-only target unexpectedly accepted atomic replacement")
        assert calls == 1,calls
        assert json.loads(target.read_text()) == before
    finally:
        agent.os.replace = original
        os.chmod(target,stat.S_IREAD|stat.S_IWRITE)
    results.append(dict(test="agent_readonly_denial_fail_fast",passed=True,
                        attempts=calls,winerror=attempted_codes[0],old_target_preserved=True))

    restricted = root / "acl-case"
    restricted.mkdir()
    target = restricted / "status.json"
    source = restricted / "status.new"
    target.write_text(json.dumps(before))
    source.write_text(json.dumps(after))
    quote = lambda path: "'"+str(path).replace("'","''")+"'"
    folder_sddl,file_sddl,source_sddl = root/"folder-acl.txt",root/"file-acl.txt",root/"source-acl.txt"
    setup = ("$ErrorActionPreference='Stop';$who=[Security.Principal.SecurityIdentifier]::new('S-1-1-0');"
        f"$folder={quote(restricted)};$file={quote(target)};$source={quote(source)};$a=Get-Acl -LiteralPath $folder;$b=Get-Acl -LiteralPath $file;$c=Get-Acl -LiteralPath $source;"
        f"[IO.File]::WriteAllText({quote(folder_sddl)},$a.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access));"
        f"[IO.File]::WriteAllText({quote(file_sddl)},$b.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access));"
        f"[IO.File]::WriteAllText({quote(source_sddl)},$c.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access));"
        "$a.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($who,[Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles,[Security.AccessControl.AccessControlType]::Deny));"
        "$b.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($who,[Security.AccessControl.FileSystemRights]::Delete,[Security.AccessControl.AccessControlType]::Deny));"
        "$c.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($who,[Security.AccessControl.FileSystemRights]::Delete,[Security.AccessControl.AccessControlType]::Deny));"
        "Set-Acl -LiteralPath $folder -AclObject $a;Set-Acl -LiteralPath $file -AclObject $b;Set-Acl -LiteralPath $source -AclObject $c;")
    restore = ("$ErrorActionPreference='Stop';"
        f"$folder={quote(restricted)};$file={quote(target)};$a=Get-Acl -LiteralPath $folder;$b=Get-Acl -LiteralPath $file;"
        f"$a.SetSecurityDescriptorSddlForm([IO.File]::ReadAllText({quote(folder_sddl)}),[Security.AccessControl.AccessControlSections]::Access);"
        f"$b.SetSecurityDescriptorSddlForm([IO.File]::ReadAllText({quote(file_sddl)}),[Security.AccessControl.AccessControlSections]::Access);"
        f"if(Test-Path -LiteralPath {quote(source)}){{$c=Get-Acl -LiteralPath {quote(source)};$c.SetSecurityDescriptorSddlForm([IO.File]::ReadAllText({quote(source_sddl)}),[Security.AccessControl.AccessControlSections]::Access);Set-Acl -LiteralPath {quote(source)} -AclObject $c}};"
        "Set-Acl -LiteralPath $file -AclObject $b;Set-Acl -LiteralPath $folder -AclObject $a;")
    subprocess.run(["powershell.exe","-NoProfile","-Command",setup],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    calls = 0
    attempted_codes = []
    agent.os.replace = counted
    acl_denial_reproduced = False
    try:
        try:
            agent.atomic_replace(source,target)
        except OSError as error:
            assert error.winerror == 5,error
            acl_denial_reproduced = True
            assert calls == 1,calls
            assert json.loads(target.read_text()) == before
    finally:
        agent.os.replace = original
        subprocess.run(["powershell.exe","-NoProfile","-Command",restore],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    results.append(dict(test="agent_acl_delete_denial_fail_fast",passed=True if acl_denial_reproduced else None,
                        status="PASS" if acl_denial_reproduced else "UNSUPPORTED_DENIAL_NOT_REPRODUCED",
                        attempts=calls,winerror=attempted_codes[0] if attempted_codes else None,
                        old_target_preserved=True if acl_denial_reproduced else None,acl_restored=True))
    result = dict(version=1, all_required_passed=True, all_cases_passed=acl_denial_reproduced, tests=results,
                  scope="Owned temporary JSON files; no RDP task or workload IPC modification")
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
