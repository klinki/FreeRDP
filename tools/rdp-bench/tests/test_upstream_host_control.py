"""Fresh trial identity, counter attribution and narrow Windows cleanup."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

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
            replies=iter([{'status':'LEGACY_NO_STOP_FILE'},{'owner':'ab'*16,'actions':[]}])
            runner.ps=lambda expression,**kwargs:(commands.append(expression) or json.dumps(next(replies)))
            receipt=runner.cleanup();expression=commands[1]
            self.assertEqual(receipt['collector_stop']['status'],'LEGACY_NO_STOP_FILE')
            self.assertIn('Collector process identity differs',commands[0])
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


    def test_collector_graceful_mode_is_explicit_and_owned(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder)
            control.save(Path(folder)/'session-discovery.json',dict(counter_instance='RDP-Tcp 3'))
            commands=[]
            runner.ps=lambda expression,**kwargs:(commands.append(expression) or '')
            runner.collector(180,10)
            self.assertNotIn(' -StopFile ',commands[-1])
            runner.collector(180,10,True)
            self.assertIn("-StopFile 'C:\\bench\\loss50\\collector-stop.request.json'",commands[-1])
            self.assertIn('stop_file=',commands[-1])

    def test_graceful_footer_and_timeout_are_retained(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder)
            footer={'type':'collector_end','requested_stop':True,'terminal_error':None}
            replies=iter([{'status':'STOP_REQUESTED','process_present':True},footer])
            runner.ps=lambda expression,**kwargs:json.dumps(next(replies))
            result=runner.stop_collector()
            self.assertEqual(result['status'],'GRACEFUL_FOOTER_RECEIVED')
            self.assertEqual(result['footer'],footer)
            self.assertFalse(result['fallback_owned_cleanup_required'])
            runner.ps=lambda expression,**kwargs:json.dumps({'status':'STOP_REQUESTED','process_present':True})
            with mock.patch.object(control.time,'monotonic',side_effect=[0,16,16]):
                result=runner.stop_collector()
            self.assertEqual(result['status'],'GRACEFUL_STOP_TIMEOUT')
            self.assertTrue(result['fallback_owned_cleanup_required'])
            self.assertIsNone(result['footer'])

    def test_malformed_stop_status_is_not_hidden(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=self.controller(folder);runner.ps=lambda expression,**kwargs:'{}'
            with self.assertRaisesRegex(ValueError,'malformed collector stop'):
                runner.cleanup()

    def test_stop_sentinel_disposes_query_without_fabricated_provider_error(self):
        source=(ROOT/'upstream-method/Collect-Counters.ps1').read_text()
        self.assertIn("[string]$StopFile=''",source)
        self.assertIn('throw [OperationCanceledException]',source)
        self.assertIn('if(!$script:requestedStop)',source)
        self.assertIn("stop_reason=if($script:requestedStop) {'REQUESTED_STOP'}",source)
        self.assertGreater(source.index('$script:requestedStop=$true'),source.index('$previousTimestamp=$end'))
        self.assertIn("type='collector_end'",source)


if __name__=='__main__':unittest.main()
