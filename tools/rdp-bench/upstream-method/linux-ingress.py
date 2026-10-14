#!/usr/bin/env python3
"""Owned Linux veth ingress adaptation of OpenThinRDP's IFB/netem method.

Run in a dedicated benchmark network namespace. The veth receive endpoint has
no IP address and the same MAC as the ordinary interface. Only IPv4 packets
from --vm-ip are redirected. No rate cap or client-to-server loss is added.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import subprocess
import uuid


def json_object(pairs):
    # iproute2 prints one JSON "match" key per u32 selector word. Preserve
    # repeated match keys; ordinary json.loads silently discards all but one.
    result = {}
    for key, value in pairs:
        if key in result:
            if key not in ('match', 'success'):
                raise ValueError('duplicate JSON field: ' + key)
            result[key] = (result[key] if isinstance(result[key], list) else [result[key]]) + [value]
        else:
            result[key] = value
    return result


def command(*args, as_json=False):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(' '.join(args) + ': ' + result.stderr.strip())
    return json.loads(result.stdout or '[]', object_pairs_hook=json_object) if as_json else result.stdout


def loss_protocol(state):
    value = state.get('loss_protocol', 'all')
    if value not in ('all', 'tcp', 'udp'):
        raise ValueError('invalid recorded loss protocol')
    return value


def protocol_scope(state):
    protocol = loss_protocol(state)
    return dict(loss_protocol=protocol, selected_ipv4_protocol={'tcp': 6, 'udp': 17}.get(protocol),
                direction='Windows VM to client only', rate_cap_mbps=None,
                selected_treatment='random loss; packets >=512 bytes also have 1ms delay and 70% reorder',
                nonselected_treatment='clean FIFO bypass, no added loss/delay/reorder' if protocol != 'all'
                else 'none; legacy treatment applies to all IPv4 from VM')


def hexadecimal(value):
    if isinstance(value, bool):
        raise ValueError('boolean classifier word')
    result = value if isinstance(value, int) else int(value, 16)
    if not 0 <= result <= 0xffffffff:
        raise ValueError('classifier word outside u32')
    return result


def match_words(options):
    values = options.get('match')
    if isinstance(values, dict):
        values = [values]
    if not isinstance(values, list) or not values:
        raise ValueError('classifier lacks complete u32 match words')
    words = []
    for value in values:
        if not isinstance(value, dict) or isinstance(value.get('off'), bool) or not isinstance(value.get('off'), int):
            raise ValueError('invalid classifier match offset')
        if hexadecimal(value.get('offmask') or '0') != 0:
            raise ValueError('dynamic classifier offset is not owned')
        words.append((hexadecimal(value['value']), hexadecimal(value['mask']), value['off']))
    if len(set(words)) != len(words):
        raise ValueError('duplicate classifier match word')
    return set(words)


def classifier_leaves(filters, priorities):
    if not isinstance(filters, list):
        raise ValueError('malformed classifier inventory')
    leaves = []
    for leaf in filters:
        if (not isinstance(leaf, dict) or leaf.get('kind') != 'u32' or leaf.get('protocol') != 'ip'
                or leaf.get('pref') not in priorities or leaf.get('chain', 0) != 0):
            raise ValueError('foreign classifier kind/protocol/priority/chain')
        options = leaf.get('options', {})
        if not isinstance(options, dict):
            raise ValueError('malformed classifier options')
        if not options:
            continue  # iproute2's classifier header, not a rule.
        if 'ht_divisor' in options:
            if options['ht_divisor'] != 1 or set(options) - {'fh', 'ht_divisor'}:
                raise ValueError('foreign classifier hash table')
            continue
        if any(key in options for key in ('offset', 'link', 'indev', 'police')):
            raise ValueError('foreign classifier offset/action')
        match_words(options)
        leaves.append(leaf)
    return leaves


def checked_netem_options(qdisc, loss, large):
    options = qdisc.get('options')
    if not isinstance(options, dict) or set(options) - {'limit', 'delay', 'reorder', 'loss-random', 'ecn', 'gap'}:
        raise ValueError('owned netem treatment has foreign options')
    if options.get('limit') != 100000 or options.get('ecn', False) is not False or options.get('gap', 0) != int(large):
        raise ValueError('owned netem queue settings changed')
    for key, field, expected in (('delay', 'delay', .001 if large else 0),
                                 ('reorder', 'reorder', .7 if large else 0),
                                 ('loss-random', 'loss', loss / 100)):
        values = options.get(key, {})
        if not isinstance(values, dict):
            raise ValueError('malformed netem ' + key)
        actual = values.get(field, 0)
        if (isinstance(actual, bool) or not isinstance(actual, (int, float)) or not math.isfinite(actual)
                or not math.isclose(actual, expected, abs_tol=1e-6)
                or values.get('correlation', 0) != 0 or values.get('jitter', 0) != 0
                or set(values) - {field, 'jitter', 'correlation'}):
            raise ValueError('owned netem ' + key + ' treatment changed')


def now():
    return datetime.now(timezone.utc).isoformat()


def save(path, state):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    temporary.replace(path)


def netns():
    return os.readlink('/proc/self/ns/net')


def interface(name):
    return command('ip', '-j', 'link', 'show', 'dev', name, as_json=True)[0]


def checked_state(path):
    state = json.loads(Path(path).read_text())
    protocol = loss_protocol(state)
    if state.get('protocol_scope') is not None and state['protocol_scope'] != protocol_scope(state):
        raise ValueError('recorded protocol scope changed')
    state['loss_protocol'] = protocol
    if state['netns'] != netns():
        raise ValueError('state belongs to a different network namespace')
    if not isinstance(state.get('ifindices'), dict):
        raise ValueError('state lacks interface identity receipts; refusing to adopt it')
    links = {role: interface(state[role]) for role in ('interface', 'ingress', 'receive')}
    for role, link in links.items():
        if link.get('ifindex') != state['ifindices'].get(role):
            raise ValueError('interface identity changed: ' + role)
        if role != 'interface' and link.get('ifalias') != state['owner'] + ':' + role:
            raise ValueError('refusing to alter an interface without the recorded ownership alias')
    if links['interface'].get('address') != state['mac'] or links['receive'].get('address') != state['mac']:
        raise ValueError('recorded interface/receive MAC identity changed')
    addresses = command('ip', '-j', 'address', 'show', 'dev', state['receive'], as_json=True)
    if any(addr.get('family') == 'inet' for link in addresses for addr in link.get('addr_info', [])):
        raise ValueError('receive endpoint must not own an IPv4 address')
    return state


def checked_line_graph(state, check_classes=False):
    # Check only the owned source redirect and the parent/classifier identities
    # we intend to mutate. Dynamic statistics and u32 table IDs are not identity.
    if state.get('installed_ingress'):
        qdiscs = command('tc', '-j', 'qdisc', 'show', 'dev', state['interface'], as_json=True)
        ingress = [q for q in qdiscs if q.get('kind') in ('ingress', 'clsact')]
        if len(ingress) != 1 or ingress[0].get('kind') != 'ingress' or ingress[0].get('handle') != 'ffff:':
            raise ValueError('owned ingress qdisc was replaced')
        filters = command('tc', '-j', 'filter', 'show', 'dev', state['interface'], 'parent', 'ffff:', as_json=True)
        leaves = classifier_leaves(filters, {100})
        if len(leaves) != 1:
            raise ValueError('owned ingress source filter was replaced or augmented')
        leaf = leaves[0]; options = leaf.get('options', {})
        actions = options.get('actions', [])
        if (leaf.get('kind') != 'u32' or leaf.get('protocol') != 'ip' or leaf.get('pref') != 100
                or match_words(options) != {(int(ipaddress.IPv4Address(state['vm_ip'])), 0xffffffff, 12)}
                or not isinstance(actions, list) or len(actions) != 1 or not isinstance(actions[0], dict)
                or actions[0].get('kind') != 'mirred'
                or actions[0].get('mirred_action') != 'redirect' or actions[0].get('direction') != 'egress'
                or actions[0].get('to_dev') != state['ingress']):
            raise ValueError('owned source redirect no longer matches the recorded VM/interface')
    if check_classes:
        protocol = loss_protocol(state)
        selected = protocol != 'all'
        qdiscs = command('tc', '-j', 'qdisc', 'show', 'dev', state['ingress'], as_json=True)
        roots = [q for q in qdiscs if q.get('root')]
        if len(roots) != 1 or roots[0].get('kind') != 'prio' or roots[0].get('handle') != '1:':
            raise ValueError('owned priority qdisc was replaced')
        root_options = roots[0].get('options', {})
        if root_options.get('bands') != 3 or root_options.get('priomap') != [2] * 16:
            raise ValueError('owned priority bands/default bypass changed')
        filters = command('tc', '-j', 'filter', 'show', 'dev', state['ingress'], 'parent', '1:', as_json=True)
        expected = {1: ('1:2', {(0, 0xfe00, 0)}), 2: ('1:1', {(0, 0, 0)})}
        if selected:
            word = ({'tcp': 6, 'udp': 17}[protocol] << 16, 0xff0000, 8)
            expected = {1: ('1:2', {word, (0, 0xfe00, 0)}),
                        2: ('1:1', {word}), 3: ('1:3', {(0, 0, 0)})}
        leaves = classifier_leaves(filters, set(expected))
        if len(leaves) != len(expected) or len({leaf['pref'] for leaf in leaves}) != len(expected):
            raise ValueError('owned protocol/size classifiers were replaced or augmented')
        for leaf in leaves:
            flow, words = expected[leaf['pref']]
            options = leaf['options']
            if options.get('flowid') != flow or match_words(options) != words or options.get('actions'):
                raise ValueError('owned protocol/size classifier changed')
        children = {}
        for qdisc in qdiscs:
            parent = qdisc.get('parent')
            if not parent:
                continue
            if parent in children or parent not in ('1:1', '1:2') + (('1:3',) if selected else ()):
                raise ValueError('unexpected child qdisc on owned priority queue')
            expected_kind, expected_handle = ('pfifo', '30:') if parent == '1:3' else ('netem', '10:' if parent == '1:1' else '20:')
            if qdisc.get('kind') != expected_kind or qdisc.get('handle') != expected_handle:
                raise ValueError('foreign child qdisc on owned priority queue')
            children[parent] = qdisc
        if selected and ('1:3' not in children or children['1:3'].get('options', {}).get('limit') != 100000):
            raise ValueError('clean protocol bypass qdisc changed or disappeared')
        if state.get('loss_percent') is not None and not {'1:1', '1:2'}.issubset(children):
            raise ValueError('owned netem qdisc disappeared')
        if state.get('loss_percent') is not None:
            loss = state['loss_percent']
            if isinstance(loss, bool) or not isinstance(loss, (int, float)) or not math.isfinite(loss) or not 0 <= loss <= 100:
                raise ValueError('invalid recorded loss percentage')
            for parent in ('1:1', '1:2'):
                checked_netem_options(children[parent], loss, parent == '1:1')


def rp_path(name):
    return Path('/proc/sys/net/ipv4/conf') / name / 'rp_filter'


def set_rp_filter(name, value):
    # Docker may expose read-only sysctl files whose effective values already
    # match. Do not require a write in that case, but never hide a mismatch.
    path = rp_path(name)
    desired = str(value).strip()
    if path.read_text().strip() == desired:
        return
    path.write_text(desired + '\n')
    if path.read_text().strip() != desired:
        raise RuntimeError('rp_filter write did not take effect: ' + name)


def setup(args):
    if Path(args.state).exists():
        raise ValueError('state already exists; use change/snapshot/off')
    vm_ip = str(ipaddress.IPv4Address(args.vm_ip))
    protocol = loss_protocol({'loss_protocol': getattr(args, 'loss_protocol', 'all')})
    for name in (args.interface, args.ingress, args.receive):
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,15}', name):
            raise ValueError('unsafe or overlong interface name')
    if len({args.interface, args.ingress, args.receive}) != 3:
        raise ValueError('interface names must be distinct')
    links = command('ip', '-j', 'link', 'show', as_json=True)
    if any(link['ifname'] in (args.ingress, args.receive) for link in links):
        raise ValueError('veth name already exists; refusing to adopt it')
    device = interface(args.interface)
    routes = command('ip', '-j', 'route', 'get', vm_ip, as_json=True)
    if not routes or routes[0].get('dev') != args.interface:
        raise ValueError('VM route does not use the selected interface')
    qdiscs = command('tc', '-j', 'qdisc', 'show', 'dev', args.interface, as_json=True)
    if any(qdisc['kind'] in ('ingress', 'clsact') for qdisc in qdiscs):
        raise ValueError('existing ingress/clsact qdisc; refusing to replace it')
    state = dict(schema_version=2, created_utc=now(), netns=netns(), vm_ip=vm_ip,
                 interface=args.interface, ingress=args.ingress, receive=args.receive,
                 mac=device['address'], pair_mtu=1500, original_interface_mtu=device['mtu'],
                 ifindices={'interface': device['ifindex']},
                 owner='freerdp-pr13608:' + uuid.uuid4().hex,
                 original_rp_filter={name: rp_path(name).read_text().strip()
                                     for name in ('all', args.interface)},
                 installed_ingress=False, veth_created=False, loss_percent=None,
                 loss_protocol=protocol,
                 method='upstream ingress loss via veth, not IFB', rate_cap_mbps=None)
    state['protocol_scope'] = protocol_scope(state)
    save(args.state, state)
    try:
        command('ip', 'link', 'add', args.ingress, 'type', 'veth', 'peer', 'name', args.receive)
        state['veth_created'] = True
        for role in ('ingress', 'receive'):
            name = state[role]
            command('ip', 'link', 'set', 'dev', name, 'alias', state['owner'] + ':' + role)
            command('ip', 'link', 'set', 'dev', name, 'mtu', '1500')
            state['ifindices'][role] = interface(name)['ifindex']
        command('ip', 'link', 'set', 'dev', args.receive, 'address', state['mac'])
        command('ip', 'link', 'set', 'dev', args.interface, 'mtu', '1500')
        command('ip', 'link', 'set', 'dev', args.ingress, 'up')
        command('ip', 'link', 'set', 'dev', args.receive, 'up')
        for name in ('all', args.interface, args.receive):
            set_rp_filter(name, '0')
        save(args.state, state)
        command('tc', 'qdisc', 'add', 'dev', args.interface, 'handle', 'ffff:', 'ingress')
        state['installed_ingress'] = True
        save(args.state, state)
        command('tc', 'filter', 'add', 'dev', args.interface, 'parent', 'ffff:',
                'protocol', 'ip', 'prio', '100', 'u32', 'match', 'ip', 'src', vm_ip + '/32',
                'action', 'mirred', 'egress', 'redirect', 'dev', args.ingress)
        command('tc', 'qdisc', 'add', 'dev', args.ingress, 'root', 'handle', '1:',
                'prio', 'bands', '3', 'priomap', *(['2'] * 16))
        selector = [] if protocol == 'all' else ['match', 'ip', 'protocol', str({'tcp': 6, 'udp': 17}[protocol]), '0xff']
        command('tc', 'filter', 'add', 'dev', args.ingress, 'parent', '1:',
                'protocol', 'ip', 'prio', '1', 'u32', *selector,
                'match', 'u16', '0x0000', '0xfe00', 'at', '2', 'flowid', '1:2')
        command('tc', 'filter', 'add', 'dev', args.ingress, 'parent', '1:',
                'protocol', 'ip', 'prio', '2', 'u32',
                *(selector or ['match', 'u32', '0', '0']), 'flowid', '1:1')
        if protocol != 'all':
            command('tc', 'filter', 'add', 'dev', args.ingress, 'parent', '1:',
                    'protocol', 'ip', 'prio', '3', 'u32', 'match', 'u32', '0', '0', 'flowid', '1:3')
            command('tc', 'qdisc', 'add', 'dev', args.ingress, 'parent', '1:3', 'handle', '30:',
                    'pfifo', 'limit', '100000')
        change(args.state, 0)
        return snapshot(args.state)
    except Exception:
        # Save partial ownership for deliberate cleanup; never hide setup failure.
        save(args.state, state)
        raise


def change(path, loss):
    if isinstance(loss, bool) or not isinstance(loss, (int, float)) or not math.isfinite(loss) or not 0 <= loss <= 100:
        raise ValueError('loss must be a percentage from 0 through 100')
    state = checked_state(path)
    checked_line_graph(state, check_classes=True)
    # Updating a netem qdisc in place preserves counters. Explicitly delete
    # only our children, then recreate them to reset counters/queued packets,
    # matching upstream line.sh's step replacement. Keep classifiers intact.
    current = command('tc', '-j', 'qdisc', 'show', 'dev', state['ingress'], as_json=True)
    children = [qdisc for qdisc in current if qdisc.get('parent') in ('1:1', '1:2')]
    for child in children:
        expected = '10:' if child['parent'] == '1:1' else '20:'
        if child.get('kind') != 'netem' or child.get('handle') != expected:
            raise ValueError('refusing to replace a foreign child qdisc')
    for child in children:
        command('tc', 'qdisc', 'del', 'dev', state['ingress'],
                'parent', child['parent'], 'handle', child['handle'])
    command('tc', 'qdisc', 'add', 'dev', state['ingress'], 'parent', '1:1', 'handle', '10:',
            'netem', 'delay', '1ms', 'reorder', '70%', 'loss', f'{loss:g}%', 'limit', '100000')
    command('tc', 'qdisc', 'add', 'dev', state['ingress'], 'parent', '1:2', 'handle', '20:',
            'netem', 'loss', f'{loss:g}%', 'limit', '100000')
    state.update(loss_percent=loss, changed_utc=now())
    save(path, state)
    return snapshot(path)


def netem_counters(qdiscs):
    result = {}
    for qdisc in qdiscs:
        if qdisc.get('kind') != 'netem' or qdisc.get('handle') not in ('10:', '20:'):
            continue
        sent, drops = qdisc.get('packets'), qdisc.get('drops')
        total = sent + drops if isinstance(sent, int) and isinstance(drops, int) else None
        result['large' if qdisc['handle'] == '10:' else 'small'] = {
            'transmitted_packets': sent, 'dropped_packets': drops,
            'backlog_packets': qdisc.get('qlen'), 'transmitted_bytes': qdisc.get('bytes'),
            'drop_fraction_completed_attempts': drops / total if total else None,
            'scope': 'all netem drops; excludes pending packets; not inferred random-loss-only',
        }
    return result


def bypass_counters(state, qdiscs):
    selected = loss_protocol(state) != 'all'
    rows = [q for q in qdiscs if q.get('parent') == '1:3']
    qdisc = rows[0] if selected and len(rows) == 1 else {}
    return dict(enabled=selected, classid='1:3' if selected else None,
                qdisc_kind=qdisc.get('kind'), transmitted_packets=qdisc.get('packets'),
                dropped_packets=qdisc.get('drops'), backlog_packets=qdisc.get('qlen'),
                transmitted_bytes=qdisc.get('bytes'),
                scope='nonselected IPv4 from VM; clean FIFO, no netem loss/delay/reorder' if selected
                else 'not used; all IPv4 from VM is selected by the legacy method')


def snapshot(path):
    state = checked_state(path)
    checked_line_graph(state, check_classes=True)
    links = {role: command('ip', '-j', '-s', 'link', 'show', 'dev', state[role], as_json=True)
             for role in ('interface', 'ingress', 'receive')}
    addresses = command('ip', '-j', 'address', 'show', 'dev', state['receive'], as_json=True)
    if any(addr.get('family') == 'inet' for link in addresses for addr in link.get('addr_info', [])):
        raise ValueError('receive endpoint must not own an IPv4 address')
    if links['receive'][0].get('address') != links['interface'][0].get('address'):
        raise ValueError('receive endpoint MAC no longer matches ordinary interface')
    qdiscs = command('tc', '-j', '-s', 'qdisc', 'show', 'dev', state['ingress'], as_json=True)
    return {'schema_version': 2, 'utc': now(), 'state': state, 'links': links,
            'loss_protocol': loss_protocol(state), 'protocol_scope': protocol_scope(state),
            'receive_addresses': addresses,
            'interface_qdiscs': command('tc', '-j', '-s', 'qdisc', 'show', 'dev', state['interface'], as_json=True),
            'ingress_filter': command('tc', '-j', '-s', 'filter', 'show', 'dev', state['interface'], 'parent', 'ffff:', as_json=True),
            'classification_filters': command('tc', '-j', '-s', 'filter', 'show', 'dev', state['ingress'], 'parent', '1:', as_json=True),
            'netem_qdiscs': qdiscs, 'netem_counters': netem_counters(qdiscs),
            'bypass_counters': bypass_counters(state, qdiscs),
            'rp_filter': {name: rp_path(name).read_text().strip()
                          for name in ('all', state['interface'], state['receive'])}}


def off(path):
    # Full ownership is required even for cleanup. Partial setup failures retain
    # the state and must be inspected instead of deleting arbitrary interfaces.
    state = checked_state(path)
    checked_line_graph(state, check_classes=True)
    before = snapshot(path)
    if state.get('installed_ingress'):
        command('tc', 'qdisc', 'del', 'dev', state['interface'], 'ingress')
    command('ip', 'link', 'del', 'dev', state['ingress'])
    for name, value in state['original_rp_filter'].items():
        set_rp_filter(name, value)
    command('ip', 'link', 'set', 'dev', state['interface'], 'mtu', str(state['original_interface_mtu']))
    Path(path).unlink()
    return dict(schema_version=1, utc=now(), outcome='REMOVED', before=before,
                restored_rp_filter=state['original_rp_filter'],
                restored_interface_mtu=state['original_interface_mtu'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    sub = parser.add_subparsers(dest='command', required=True)
    installer = sub.add_parser('setup')
    installer.add_argument('--vm-ip', required=True)
    installer.add_argument('--interface', default='eth0')
    installer.add_argument('--ingress', default='bench_ing')
    installer.add_argument('--receive', default='bench_rx')
    installer.add_argument('--loss-protocol', choices=('all', 'tcp', 'udp'), default='all',
                           help='IPv4 protocol selected for the entire netem treatment; others bypass cleanly')
    changer = sub.add_parser('change')
    changer.add_argument('--loss', type=float, required=True)
    sub.add_parser('snapshot'); sub.add_parser('off')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run as root in an isolated benchmark network namespace')
    if args.command == 'setup': result = setup(args)
    elif args.command == 'change': result = change(args.state, args.loss)
    elif args.command == 'snapshot': result = snapshot(args.state)
    else: result = off(args.state)
    if args.output: save(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
