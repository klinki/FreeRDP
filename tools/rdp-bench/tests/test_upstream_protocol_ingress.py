"""Protocol selection binds the entire impairment and preserves owned cleanup."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('protocol_ingress', ROOT / 'upstream-method/linux-ingress.py')
ingress = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ingress)


def word(value, mask, offset):
    return dict(value=f'{value:x}', mask=f'{mask:x}', off=offset, offmask='')


def classifier(pref, flow, matches):
    return dict(kind='u32', protocol='ip', pref=pref, chain=0,
                options=dict(flowid=flow, match=matches[0] if len(matches) == 1 else matches))


def fixture(protocol='udp', loss=0):
    state = dict(loss_protocol=protocol, loss_percent=loss, installed_ingress=True,
                 interface='eth0', ingress='bench_ing', receive='bench_rx',
                 vm_ip='192.168.64.2', netns='fixture-netns', mac='02:00:00:00:00:01',
                 owner='fixture-owner', ifindices=dict(interface=4, ingress=5, receive=6))
    state['protocol_scope'] = ingress.protocol_scope(state)
    source = dict(kind='u32', protocol='ip', pref=100, chain=0,
                  options=dict(match=word(0xc0a84002, 0xffffffff, 12),
                               actions=[dict(kind='mirred', mirred_action='redirect',
                                             direction='egress', to_dev='bench_ing')]))
    filters = [classifier(1, '1:2', [word(0, 0xfe00, 0)]),
               classifier(2, '1:1', [word(0, 0, 0)])]
    if protocol != 'all':
        protocol_word = word({'tcp': 6, 'udp': 17}[protocol] << 16, 0xff0000, 8)
        filters = [classifier(1, '1:2', [protocol_word, word(0, 0xfe00, 0)]),
                   classifier(2, '1:1', [dict(protocol_word)]),
                   classifier(3, '1:3', [word(0, 0, 0)])]
    qdiscs = [dict(kind='prio', handle='1:', root=True,
                  options=dict(bands=3, priomap=[2] * 16)),
              dict(kind='netem', handle='10:', parent='1:1',
                   options=dict(limit=100000, ecn=False, gap=1,
                                delay=dict(delay=.001, jitter=0, correlation=0),
                                reorder=dict(reorder=.7, correlation=0),
                                **({'loss-random': dict(loss=loss/100, correlation=0)} if loss else {}))),
              dict(kind='netem', handle='20:', parent='1:2',
                   options=dict(limit=100000, ecn=False, gap=0,
                                **({'loss-random': dict(loss=loss/100, correlation=0)} if loss else {})))]
    if protocol != 'all':
        qdiscs.append(dict(kind='pfifo', handle='30:', parent='1:3', options=dict(limit=100000),
                           packets=10, drops=0, bytes=6000, qlen=0))
    return state, [source], filters, qdiscs


class ProtocolIngressTests(unittest.TestCase):
    def graph_command(self, state, source, filters, qdiscs, mutations):
        def command(*args, as_json=False):
            if '-j' not in args:
                mutations.append(args)
                return ''
            if 'filter' in args:
                return copy.deepcopy(source if 'ffff:' in args else filters)
            if 'qdisc' in args:
                return ([dict(kind='ingress', handle='ffff:')]
                        if state['interface'] in args else copy.deepcopy(qdiscs))
            raise AssertionError(args)
        return command

    def test_actual_iproute_duplicate_match_json_preserves_protocol_and_length(self):
        # Captured iproute2 spelling: multiple u32 words and success counters
        # repeat JSON keys. A normal JSON decoder would discard the protocol.
        raw = ('{"match":{"value":"110000","mask":"ff0000","off":8,"offmask":""},'
               '"match":{"value":"0","mask":"fe00","off":0,"offmask":""},'
               '"success":0,"success":0}')
        options = json.loads(raw, object_pairs_hook=ingress.json_object)
        self.assertEqual(ingress.match_words(options), {(0x110000, 0xff0000, 8), (0, 0xfe00, 0)})
        self.assertEqual(options['success'], [0, 0])
        with self.assertRaisesRegex(ValueError, 'duplicate JSON field'):
            json.loads('{"flowid":"1:2","flowid":"1:1"}', object_pairs_hook=ingress.json_object)

    def test_ordered_classifier_routes_only_selected_protocol_to_size_bands(self):
        for protocol in ('all', 'tcp', 'udp'):
            state, source, filters, qdiscs = fixture(protocol)
            mutations = []
            with patch.object(ingress, 'command', side_effect=self.graph_command(state, source, filters, qdiscs, mutations)):
                ingress.checked_line_graph(state, check_classes=True)
            self.assertEqual(mutations, [])
            for number in (1, 6, 17):
                for length in (100, 511, 512, 1500):
                    with self.subTest(selector=protocol, ip_protocol=number, length=length):
                        packet = bytearray(length)
                        packet[0] = 0x45
                        packet[2:4] = length.to_bytes(2, 'big')
                        packet[9] = number
                        matches = [f for f in sorted(filters, key=lambda f: f['pref']) if all(
                            int.from_bytes(packet[off:off+4], 'big') & mask == value
                            for value, mask, off in ingress.match_words(f['options']))]
                        self.assertTrue(matches)
                        # u32 returns the first matching terminal flow. Small
                        # selected packets precede the selected-protocol fallback.
                        flow = matches[0]['options']['flowid']
                        selected = protocol == 'all' or number == {'tcp': 6, 'udp': 17}[protocol]
                        self.assertEqual(flow, ('1:2' if length < 512 else '1:1') if selected else '1:3')

    def test_protocol_mask_size_offset_priority_and_bypass_mutations_fail_closed(self):
        changes = (
            lambda s, f, q: f[0]['options']['match'][0].update(value='60000'),
            lambda s, f, q: f[0]['options']['match'][0].update(mask='f0000'),
            lambda s, f, q: f[0]['options']['match'][1].update(mask='ffff'),
            lambda s, f, q: f[0]['options']['match'][1].update(off=2),
            lambda s, f, q: f[0].update(pref=2),
            lambda s, f, q: f[1].update(chain=1),
            lambda s, f, q: f[2]['options'].update(flowid='1:1'),
            lambda s, f, q: f.append(dict(kind='flower', protocol='ip', pref=99, options={})),
            lambda s, f, q: q[0]['options'].update(priomap=[0] * 16),
            lambda s, f, q: q[3].update(kind='netem'),
            lambda s, f, q: q[3]['options'].update(limit=1),
            lambda s, f, q: q.pop(1),
            lambda s, f, q: q[1]['options']['delay'].update(delay=.1),
            lambda s, f, q: q[2]['options'].update(**{'loss-random': {'loss': .25}}),
        )
        for index, change in enumerate(changes):
            with self.subTest(change=index):
                state, source, filters, qdiscs = fixture()
                change(state, filters, qdiscs)
                mutations = []
                with patch.object(ingress, 'checked_state', return_value=state), \
                     patch.object(ingress, 'command', side_effect=self.graph_command(state, source, filters, qdiscs, mutations)), \
                     patch.object(ingress, 'save') as save:
                    with self.assertRaises((ValueError, TypeError)):
                        ingress.change(Path('/unused/state.json'), 50)
                self.assertEqual(mutations, [])
                save.assert_not_called()

    def test_source_redirect_mutation_blocks_cleanup_before_any_mutation(self):
        state, source, filters, qdiscs = fixture()
        source[0]['options']['match']['mask'] = 'ffffff00'
        mutations = []
        with patch.object(ingress, 'checked_state', return_value=state), \
             patch.object(ingress, 'command', side_effect=self.graph_command(state, source, filters, qdiscs, mutations)), \
             patch.object(ingress, 'snapshot') as snapshot:
            with self.assertRaisesRegex(ValueError, 'source redirect'):
                ingress.off(Path('/unused/state.json'))
        self.assertEqual(mutations, [])
        snapshot.assert_not_called()

    def test_change_replaces_only_owned_selected_netem_and_keeps_clean_fifo(self):
        for protocol in ('all', 'tcp', 'udp'):
            with self.subTest(protocol=protocol):
                state, source, filters, qdiscs = fixture(protocol)
                mutations = []
                with patch.object(ingress, 'checked_state', return_value=state), \
                     patch.object(ingress, 'command', side_effect=self.graph_command(state, source, filters, qdiscs, mutations)), \
                     patch.object(ingress, 'save') as save, patch.object(ingress, 'snapshot', return_value={'fixture': True}):
                    ingress.change(Path('/unused/state.json'), 70)
                self.assertEqual(len(mutations), 4)
                self.assertTrue(all('1:3' not in args and '30:' not in args for args in mutations))
                additions = [args for args in mutations if 'add' in args]
                self.assertEqual(len(additions), 2)
                self.assertTrue(all('70%' in args and 'netem' in args for args in additions))
                large = next(args for args in additions if '1:1' in args)
                self.assertIn('1ms', large)
                self.assertIn('reorder', large)
                self.assertEqual(save.call_args.args[1]['loss_protocol'], protocol)

    def test_zero_loss_retains_large_packet_delay_and_reorder_on_selected_bands(self):
        state, source, filters, qdiscs = fixture('tcp', 25)
        mutations = []
        with patch.object(ingress, 'checked_state', return_value=state), \
             patch.object(ingress, 'command', side_effect=self.graph_command(state, source, filters, qdiscs, mutations)), \
             patch.object(ingress, 'save'), patch.object(ingress, 'snapshot'):
            ingress.change(Path('/unused/state.json'), 0)
        large = next(args for args in mutations if 'add' in args and '1:1' in args)
        self.assertIn('0%', large)
        self.assertIn('1ms', large)
        self.assertIn('70%', large)

    def test_selected_and_bypass_counter_scopes_are_distinct_and_absence_is_unknown(self):
        state, _, _, qdiscs = fixture()
        qdiscs[1].update(packets=8, drops=2, qlen=1, bytes=4800)
        qdiscs[2].update(packets=20, drops=0, qlen=0, bytes=2000)
        selected = ingress.netem_counters(qdiscs)
        bypass = ingress.bypass_counters(state, qdiscs)
        self.assertEqual(selected['large']['drop_fraction_completed_attempts'], .2)
        self.assertEqual(selected['small']['drop_fraction_completed_attempts'], 0)
        self.assertEqual(bypass['transmitted_packets'], 10)
        self.assertEqual(bypass['dropped_packets'], 0)
        self.assertEqual(bypass['classid'], '1:3')
        self.assertIn('nonselected', bypass['scope'])
        legacy = ingress.bypass_counters(fixture('all')[0], fixture('all')[3])
        self.assertFalse(legacy['enabled'])
        self.assertIsNone(legacy['transmitted_packets'])

    def test_setup_persists_selector_and_installs_explicit_clean_bypass(self):
        for protocol in ('all', 'tcp', 'udp'):
            with self.subTest(protocol=protocol), tempfile.TemporaryDirectory() as folder:
                state_path = Path(folder) / 'state.json'
                rp = Path(folder) / 'rp_filter'; rp.write_text('2\n')
                device = dict(ifindex=4, address='02:00:00:00:00:01', mtu=1500)
                mutations = []
                def command(*args, as_json=False):
                    if '-j' not in args:
                        mutations.append(args); return ''
                    if 'route' in args:
                        return [dict(dev='eth0')]
                    return []
                def interface(name):
                    return dict(device, ifindex={'eth0': 4, 'bench_ing': 5, 'bench_rx': 6}[name])
                args = SimpleNamespace(state=state_path, vm_ip='192.168.64.2', interface='eth0',
                                       ingress='bench_ing', receive='bench_rx', loss_protocol=protocol)
                with patch.object(ingress, 'command', side_effect=command), \
                     patch.object(ingress, 'interface', side_effect=interface), \
                     patch.object(ingress, 'rp_path', return_value=rp), \
                     patch.object(ingress, 'set_rp_filter'), patch.object(ingress, 'netns', return_value='fixture-netns'), \
                     patch.object(ingress, 'change') as change, patch.object(ingress, 'snapshot', return_value={}):
                    ingress.setup(args)
                state = json.loads(state_path.read_text())
                self.assertEqual(state['loss_protocol'], protocol)
                self.assertEqual(state['protocol_scope'], ingress.protocol_scope(state))
                self.assertIsNone(state['rate_cap_mbps'])
                change.assert_called_once_with(state_path, 0)
                classifiers = [args for args in mutations if args[:3] == ('tc', 'filter', 'add') and '1:' in args]
                self.assertEqual(len(classifiers), 2 if protocol == 'all' else 3)
                bypass = [args for args in mutations if 'pfifo' in args]
                self.assertEqual(len(bypass), 0 if protocol == 'all' else 1)
                if protocol != 'all':
                    for args in classifiers[:2]:
                        self.assertIn(('6' if protocol == 'tcp' else '17'), args)
                        self.assertIn('0xff', args)

    def test_cli_default_preserves_legacy_all_and_accepts_explicit_selector(self):
        for protocol in (None, 'tcp', 'udp'):
            argv = ['linux-ingress.py', '--state', '/unused/state.json', 'setup', '--vm-ip', '192.168.64.2']
            if protocol is not None:
                argv += ['--loss-protocol', protocol]
            with patch.object(sys, 'argv', argv), patch.object(ingress.os, 'geteuid', return_value=0), \
                 patch.object(ingress, 'setup', return_value={}) as setup, contextlib.redirect_stdout(io.StringIO()):
                ingress.main()
            self.assertEqual(setup.call_args.args[0].loss_protocol, protocol or 'all')

    def test_recorded_selector_scope_mismatch_is_rejected_before_interface_access(self):
        state = fixture()[0]
        state['loss_protocol'] = 'tcp'
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'state.json'
            path.write_text(json.dumps(state))
            with patch.object(ingress, 'interface') as interface:
                with self.assertRaisesRegex(ValueError, 'protocol scope'):
                    ingress.checked_state(path)
            interface.assert_not_called()

    def test_foreign_veth_alias_blocks_state_adoption(self):
        state = fixture()[0]
        def interface(name):
            role = next(role for role in ('interface', 'ingress', 'receive') if state[role] == name)
            return dict(ifindex=state['ifindices'][role], address=state['mac'], ifalias='foreign-owner')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'state.json'; path.write_text(json.dumps(state))
            with patch.object(ingress, 'netns', return_value=state['netns']), \
                 patch.object(ingress, 'interface', side_effect=interface), patch.object(ingress, 'command') as command:
                with self.assertRaisesRegex(ValueError, 'ownership alias'):
                    ingress.checked_state(path)
            command.assert_not_called()

    def test_snapshot_exposes_selected_and_bypass_receipts(self):
        state, source, filters, qdiscs = fixture('tcp')
        mutations = []
        graph_command = self.graph_command(state, source, filters, qdiscs, mutations)
        def command(*args, as_json=False):
            if args[0] != 'ip':
                return graph_command(*args, as_json=as_json)
            if 'address' in args:
                return [dict(addr_info=[])]
            return [dict(address=state['mac'])]
        with tempfile.TemporaryDirectory() as folder:
            rp = Path(folder) / 'rp'; rp.write_text('0\n')
            with patch.object(ingress, 'checked_state', return_value=state), \
                 patch.object(ingress, 'command', side_effect=command), patch.object(ingress, 'rp_path', return_value=rp):
                receipt = ingress.snapshot(Path('/unused/state.json'))
        self.assertEqual(receipt['loss_protocol'], 'tcp')
        self.assertEqual(receipt['protocol_scope']['selected_ipv4_protocol'], 6)
        self.assertEqual(receipt['bypass_counters']['transmitted_packets'], 10)
        self.assertEqual(set(receipt['netem_counters']), {'large', 'small'})
        self.assertEqual(receipt['state']['owner'], state['owner'])
        self.assertEqual(mutations, [])


if __name__ == '__main__':
    unittest.main()
