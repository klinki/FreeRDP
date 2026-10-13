"""Loss-ramp boundaries preserve unreached steps and owned network cleanup."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('upstream_ramp', ROOT / 'upstream-method/run-ramp.py')
ramp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ramp)


class Clock:
    now = 0.0
    def read(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds


class Control:
    def __init__(self):
        self.calls = []
    def call(self, operation, output, loss=None):
        self.calls.append((operation, loss))
        return {'state': {'loss_percent': loss}, 'qdisc': 'fixture'}


class RampTests(unittest.TestCase):
    def setup_run(self, temporary):
        provenance = Path(temporary) / 'provenance.json'
        provenance.write_text(json.dumps({'client_pid': 123, 'client_start_ticks': 42,
                                          'binary_sha256': 'fixture'}))
        return SimpleNamespace(pid=123, seconds=2.0, loss=[25.0, 50.0, 70.0],
                               provenance=provenance, output=Path(temporary) / 'campaign',
                               state=Path(temporary) / 'state.json')

    def test_second_step_death_retains_third_not_run_and_resets_zero(self):
        clock = Clock(); controller = Control()
        def probe(_pid):
            dead = clock.now >= 3
            return {'alive': not dead, 'start_ticks': 42, 'exit_code': 7 if dead else None}
        with tempfile.TemporaryDirectory() as temporary:
            args = self.setup_run(temporary)
            campaign = ramp.run(args, controller=controller, probe=probe,
                                clock=clock.read, sleep=clock.sleep)
            self.assertEqual([step['outcome'] for step in campaign['steps']],
                             ['COMPLETE', 'SESSION_ENDED', 'NOT_RUN'])
            self.assertEqual([step.get('observed_duration_s') for step in campaign['steps']],
                             [2.0, 1.0, None])
            self.assertEqual(campaign['steps'][1]['client_exit_code'], 7)
            self.assertFalse(campaign['steps'][1]['client_identity_changed'])
            self.assertEqual([loss for operation, loss in controller.calls if operation == 'change'],
                             [25.0, 50.0, 0])
            self.assertEqual(campaign['cleanup']['outcome'], 'LOSS_RESET_TO_ZERO')
            self.assertIsNone(campaign['verified_goodput_mbps'])
            self.assertEqual(json.loads((args.output / 'campaign.json').read_text()), campaign)

    def test_interrupt_retains_started_phase_and_restores_zero(self):
        clock = Clock(); controller = Control()
        def interrupt(_seconds):
            clock.now += 0.5
            raise KeyboardInterrupt()
        with tempfile.TemporaryDirectory() as temporary:
            campaign = ramp.run(self.setup_run(temporary), controller=controller,
                                probe=lambda _: {'alive': True, 'start_ticks': 42, 'exit_code': None},
                                clock=clock.read, sleep=interrupt)
            self.assertTrue(campaign['interrupted'])
            self.assertEqual([step['outcome'] for step in campaign['steps']],
                             ['INCOMPLETE', 'NOT_RUN', 'NOT_RUN'])
            self.assertEqual(campaign['steps'][0]['observed_duration_s'], 0.5)
            self.assertIn('qdisc_after', campaign['steps'][0]['network'])
            self.assertEqual(controller.calls[-1], ('change', 0))

    def test_normal_process_disappearance_does_not_claim_pid_reuse(self):
        clock = Clock(); controller = Control()
        def probe(_pid):
            return ({'alive': True, 'start_ticks': 42, 'exit_code': None} if clock.now < 1
                    else {'alive': False, 'start_ticks': None, 'exit_code': None})
        with tempfile.TemporaryDirectory() as temporary:
            args = self.setup_run(temporary)
            campaign = ramp.run(args, controller=controller, probe=probe,
                                clock=clock.read, sleep=clock.sleep)
            self.assertEqual([step['outcome'] for step in campaign['steps']],
                             ['SESSION_ENDED', 'NOT_RUN', 'NOT_RUN'])
            self.assertFalse(campaign['steps'][0]['client_identity_changed'])
            self.assertIsNone(campaign['steps'][0]['client_exit_code'])
            self.assertEqual(campaign['steps'][0]['observed_duration_s'], 1.0)
            self.assertEqual([loss for operation, loss in controller.calls if operation == 'change'],
                             [25.0, 0])
            self.assertEqual(json.loads((args.output / 'campaign.json').read_text()), campaign)

    def test_live_pid_replacement_is_proven_identity_change(self):
        clock = Clock(); controller = Control()
        def probe(_pid):
            return {'alive': True, 'start_ticks': 42 if clock.now < 1 else 99,
                    'exit_code': None}
        with tempfile.TemporaryDirectory() as temporary:
            campaign = ramp.run(self.setup_run(temporary), controller=controller, probe=probe,
                                clock=clock.read, sleep=clock.sleep)
            self.assertEqual([step['outcome'] for step in campaign['steps']],
                             ['SESSION_ENDED', 'NOT_RUN', 'NOT_RUN'])
            self.assertTrue(campaign['steps'][0]['client_identity_changed'])
            self.assertFalse(campaign['steps'][0]['client_alive_end'])
            self.assertIsNone(campaign['steps'][0]['client_exit_code'])
            self.assertEqual([loss for operation, loss in controller.calls if operation == 'change'],
                             [25.0, 0])

    def test_ownership_failure_never_attempts_cleanup_mutation(self):
        controller = Control()
        def reject(operation, output, loss=None):
            controller.calls.append((operation, loss))
            raise ramp.OwnershipError('ownership fixture')
        controller.call = reject
        with tempfile.TemporaryDirectory() as temporary:
            campaign = ramp.run(self.setup_run(temporary), controller=controller)
            self.assertEqual(campaign['cleanup']['outcome'], 'NOT_ATTEMPTED')
            self.assertFalse(any(operation == 'change' for operation, _ in controller.calls))
            self.assertTrue(all(step['outcome'] == 'NOT_RUN' for step in campaign['steps']))

    def test_pid_reuse_before_start_never_applies_a_loss_step(self):
        controller = Control()
        with tempfile.TemporaryDirectory() as temporary:
            campaign = ramp.run(self.setup_run(temporary), controller=controller,
                                probe=lambda _: {'alive': True, 'start_ticks': 999, 'exit_code': None})
            self.assertIn('identity differs', campaign['error'])
            self.assertTrue(all(step['outcome'] == 'NOT_RUN' for step in campaign['steps']))
            self.assertEqual([loss for operation, loss in controller.calls if operation == 'change'], [0])

    def test_missing_recorded_process_identity_is_retained_before_start(self):
        controller = Control()
        with tempfile.TemporaryDirectory() as temporary:
            args = self.setup_run(temporary)
            args.provenance.write_text(json.dumps({'client_pid': 123}))
            campaign = ramp.run(args, controller=controller,
                                probe=lambda _: {'alive': True, 'start_ticks': 42, 'exit_code': None})
            self.assertIn('client_start_ticks', campaign['error'])
            self.assertTrue(all(step['outcome'] == 'NOT_RUN' for step in campaign['steps']))
            self.assertEqual(controller.calls[-1], ('change', 0))


if __name__ == '__main__':
    unittest.main()
