"""Fresh trial identity, counter attribution and narrow Windows cleanup."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('upstream_host_control',ROOT/'upstream-method/host-control.py')
control=importlib.util.module_from_spec(spec);spec.loader.exec_module(control)


class HostControlTests(unittest.TestCase):
    def payload(self):
        return dict(username='David',quser=[' USERNAME SESSIONNAME ID STATE',
          ' David console 1 Active','>David rdp-tcp#3 4 Active',' Other rdp-tcp#4 5 Active'],
          counter_paths=['\\RemoteFX Graphics(RDP-Tcp 3)\\Output Frames/Second',
                         '\\RemoteFX Network(RDP-tcp 3)\\Loss Rate'])

    def test_active_authenticated_rdp_session_is_correlated_with_pdh(self):
        selected=control.select_session(self.payload())
        self.assertEqual(selected['session_id'],4)
        self.assertEqual(selected['counter_instance'].casefold(),'rdp-tcp 3')
        self.assertEqual(selected['counter_instance_evidence'],'CORROBORATED')

    def test_multiple_active_sessions_are_rejected_not_first_match(self):
        payload=self.payload();payload['quser'].append(' David rdp-tcp#5 6 Active')
        with self.assertRaisesRegex(ValueError,'exactly one'):control.select_session(payload)

    def test_missing_provider_instance_remains_unavailable(self):
        payload=self.payload();payload['counter_paths']=[]
        selected=control.select_session(payload)
        self.assertIsNone(selected['counter_instance'])
        self.assertEqual(selected['counter_instance_evidence'],'NO_MATCHING_PDH_INSTANCE')

    def test_powershell_literals_escape_single_quote_without_interpolation(self):
        self.assertEqual(control.quote("C:\\bench\\a'b.ps1"),"'C:\\bench\\a''b.ps1'")

    def controller(self,folder):
        cfg=dict(ssh_target='fixture@fixture',ssh_socket='/tmp/fixture.sock',
                 windows_root='C:\\bench\\loss50',scripts_root='C:\\bench\\scripts',
                 task_name='RdpBench-Pr13608-loss50')
        runner=control.Controller(cfg,folder)
        control.save(runner.metadata,dict(owner='ab'*16,config=cfg))
        return runner

    def test_cleanup_uses_exact_task_and_pid_creation_session_receipts(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder);commands=[]
            runner.ps=lambda expression,**kwargs:(commands.append(expression) or '{}')
            runner.cleanup();expression=commands[0]
            self.assertIn("Get-ScheduledTask -TaskName 'RdpBench-Pr13608-loss50'",expression)
            self.assertIn("ProcessId='+$r.pid",expression)
            self.assertIn('$p.SessionId -ne $r.session_id',expression)
            self.assertIn('$r.creation_utc',expression)
            self.assertIn('Task action differs',expression)
            self.assertNotIn("Name='powershell.exe'",expression)
            self.assertNotIn('ForEach-Object {Stop-Process',expression)

    def test_prepare_never_overwrites_existing_trial_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder)
            runner.ps=lambda *_:self.fail('no remote mutation allowed for reused trial')
            with self.assertRaisesRegex(ValueError,'refusing to overwrite'):runner.prepare()

    def test_foreground_collector_uses_correlated_instance_and_unique_outputs(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder)
            control.save(Path(folder)/'session-discovery.json',dict(counter_instance='RDP-Tcp 3'))
            commands=[]
            runner.ps=lambda expression,**kwargs:(commands.append((expression,kwargs)) or '')
            runner.collector(180,10);expression,kwargs=commands[0]
            self.assertIn("-Instance 'RDP-Tcp 3'",expression)
            self.assertIn('C:\\bench\\loss50\\collector-pid.json',expression)
            self.assertIn('C:\\bench\\loss50\\counters.jsonl',expression)
            self.assertIn('creation_utc',expression)
            self.assertEqual(kwargs['timeout'],270)
            self.assertNotIn('Start-Process',expression)


if __name__=='__main__':unittest.main()
