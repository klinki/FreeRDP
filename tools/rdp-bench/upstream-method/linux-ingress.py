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
import os
from pathlib import Path
import re
import subprocess
import uuid


def command(*args, as_json=False):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(' '.join(args) + ': ' + result.stderr.strip())
    return json.loads(result.stdout or '[]') if as_json else result.stdout


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
        leaves = [f for f in filters if f.get('options', {}).get('match') is not None
                  or f.get('options', {}).get('actions')]
        if len(leaves) != 1:
            raise ValueError('owned ingress source filter was replaced or augmented')
        leaf = leaves[0]; options = leaf.get('options', {}); match = options.get('match', {})
        actions = options.get('actions', [])
        try:
            source_matches = int(str(match.get('value')), 16) == int(ipaddress.IPv4Address(state['vm_ip']))
            mask_matches = int(str(match.get('mask')), 16) == 0xffffffff
        except (TypeError, ValueError):
            source_matches = mask_matches = False
        if (leaf.get('kind') != 'u32' or leaf.get('protocol') != 'ip' or leaf.get('pref') != 100
                or not source_matches or not mask_matches or match.get('off') != 12
                or len(actions) != 1 or actions[0].get('kind') != 'mirred'
                or actions[0].get('mirred_action') != 'redirect' or actions[0].get('direction') != 'egress'
                or actions[0].get('to_dev') != state['ingress']):
            raise ValueError('owned source redirect no longer matches the recorded VM/interface')
    if check_classes:
        qdiscs = command('tc', '-j', 'qdisc', 'show', 'dev', state['ingress'], as_json=True)
        roots = [q for q in qdiscs if q.get('root')]
        if len(roots) != 1 or roots[0].get('kind') != 'prio' or roots[0].get('handle') != '1:':
            raise ValueError('owned priority qdisc was replaced')
        filters = command('tc', '-j', 'filter', 'show', 'dev', state['ingress'], 'parent', '1:', as_json=True)
        leaves = [f for f in filters if f.get('options', {}).get('match') is not None
                  or f.get('options', {}).get('actions')]
        if len(leaves) != 2 or {(f.get('pref'), f.get('options', {}).get('flowid')) for f in leaves} != {(1, '1:2'), (2, '1:1')}:
            raise ValueError('owned packet-size classifiers were replaced or augmented')
        if any(f.get('kind') != 'u32' or f.get('protocol') != 'ip' for f in leaves):
            raise ValueError('owned packet-size classifier kind changed')
        for qdisc in qdiscs:
            if qdisc.get('parent') and qdisc.get('parent') not in ('1:1', '1:2'):
                raise ValueError('unexpected child qdisc on owned priority queue')


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
    state = dict(schema_version=1, created_utc=now(), netns=netns(), vm_ip=vm_ip,
                 interface=args.interface, ingress=args.ingress, receive=args.receive,
                 mac=device['address'], pair_mtu=1500, original_interface_mtu=device['mtu'],
                 ifindices={'interface': device['ifindex']},
                 owner='freerdp-pr13608:' + uuid.uuid4().hex,
                 original_rp_filter={name: rp_path(name).read_text().strip()
                                     for name in ('all', args.interface)},
                 installed_ingress=False, veth_created=False, loss_percent=None,
                 method='upstream ingress loss via veth, not IFB', rate_cap_mbps=None)
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
        command('tc', 'filter', 'add', 'dev', args.ingress, 'parent', '1:',
                'protocol', 'ip', 'prio', '1', 'u32', 'match', 'u16', '0x0000', '0xfe00',
                'at', '2', 'flowid', '1:2')
        command('tc', 'filter', 'add', 'dev', args.ingress, 'parent', '1:',
                'protocol', 'ip', 'prio', '2', 'u32', 'match', 'u32', '0', '0',
                'flowid', '1:1')
        change(args.state, 0)
        return snapshot(args.state)
    except Exception:
        # Save partial ownership for deliberate cleanup; never hide setup failure.
        save(args.state, state)
        raise


def change(path, loss):
    if isinstance(loss, bool) or not isinstance(loss, (int, float)) or not 0 <= loss <= 100:
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


def snapshot(path):
    state = checked_state(path)
    links = {role: command('ip', '-j', '-s', 'link', 'show', 'dev', state[role], as_json=True)
             for role in ('interface', 'ingress', 'receive')}
    addresses = command('ip', '-j', 'address', 'show', 'dev', state['receive'], as_json=True)
    if any(addr.get('family') == 'inet' for link in addresses for addr in link.get('addr_info', [])):
        raise ValueError('receive endpoint must not own an IPv4 address')
    if links['receive'][0].get('address') != links['interface'][0].get('address'):
        raise ValueError('receive endpoint MAC no longer matches ordinary interface')
    qdiscs = command('tc', '-j', '-s', 'qdisc', 'show', 'dev', state['ingress'], as_json=True)
    return {'schema_version': 1, 'utc': now(), 'state': state, 'links': links,
            'receive_addresses': addresses,
            'interface_qdiscs': command('tc', '-j', '-s', 'qdisc', 'show', 'dev', state['interface'], as_json=True),
            'ingress_filter': command('tc', '-j', '-s', 'filter', 'show', 'dev', state['interface'], 'parent', 'ffff:', as_json=True),
            'classification_filters': command('tc', '-j', '-s', 'filter', 'show', 'dev', state['ingress'], 'parent', '1:', as_json=True),
            'netem_qdiscs': qdiscs, 'netem_counters': netem_counters(qdiscs),
            'rp_filter': {name: rp_path(name).read_text().strip()
                          for name in ('all', state['interface'], state['receive'])}}


def off(path):
    # Full ownership is required even for cleanup. Partial setup failures retain
    # the state and must be inspected instead of deleting arbitrary interfaces.
    state = checked_state(path)
    checked_line_graph(state)
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
