#!/usr/bin/env python3
"""Audit offered/post-loss UDP2 captures and an optional receiver recovery ledger."""
from __future__ import annotations
import argparse
from pathlib import Path
from rdpbench.common import read_json, write_json
from rdpbench.packet_trace import audit_channel, audit_receiver_trace, read_pcap


def channel_number(value):
    try:
        result = int(value, 0)
    except ValueError:
        result = int(value, 16)
    if not 0 <= result <= 65535:
        raise argparse.ArgumentTypeError('channel must be a 16-bit integer')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offered', type=Path, required=True)
    parser.add_argument('--received', type=Path, required=True)
    parser.add_argument('--vm-ip', required=True)
    parser.add_argument('--channel', type=channel_number, required=True,
                        help='decimal, 0x-prefixed, or hexadecimal channel label')
    parser.add_argument('--client-port', type=int)
    parser.add_argument('--capture-health', type=Path,
                        help='JSON offered/received objects with complete and kernel_drops')
    parser.add_argument('--trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    targets = [args.output.with_suffix(suffix).resolve() for suffix in ('.json', '.md')]
    inputs = [path.resolve() for path in (args.offered, args.received, args.trace, args.capture_health) if path]
    if any(target in inputs for target in targets):
        parser.error('output must not overwrite capture/trace inputs')
    if args.client_port is not None and not 1 <= args.client_port <= 65535:
        parser.error('client port must be from 1 through 65535')
    offered, received = read_pcap(args.offered), read_pcap(args.received)
    health = read_json(args.capture_health) if args.capture_health else None
    report = audit_channel(offered, received, args.vm_ip, args.channel,
                           client_port=args.client_port, capture_health=health)
    report['captures'] = {name: {key: capture[key] for key in
                         ('path', 'sha256', 'record_count', 'snaplen', 'linktype', 'errors',
                          'non_udp_ipv4', 'udp2_unclassified')}
                          for name, capture in (('offered', offered), ('received', received))}
    report['receiver_trace'] = audit_receiver_trace(args.trace, args.channel,
        target_sequences=[row['data_seq'] for row in report['attempts']]) if args.trace else None
    write_json(args.output.with_suffix('.json'), report)
    trace = report['receiver_trace']
    lines = ['# UDP2 missing-channel packet audit', '',
             f"Target channel `{args.channel:04x}`: {report['offered_channel_packets']} offered "
             f"and {report['received_channel_packets']} post-loss packets observed.", '',
             '| DataSeq | Offered packet | Post-loss matches | Captured ACK candidates |',
             '|---|---:|---:|---:|']
    lines += [f"| `{row['data_seq']:04x}` | {row['offered_packet']} | {row['received_match_count']} | "
              f"{len(row['captured_ack_coverage'])} |" for row in report['attempts']]
    if trace:
        lines += ['', f"Receiver trace valid: {trace['valid']}; unseen auto-slides: "
                  f"{len(trace['unseen_auto_slides'])}; subsequent feedback references: "
                  f"{len(trace['feedback_after_unseen_slide'])}."]
    lines += ['', 'Capture absence is not proof of packet loss or sender abandonment. '
              'ACK candidates require sequence-epoch and AoA-retirement validation.', '']
    lines += ['- ' + note for note in report['notes']]
    args.output.with_suffix('.md').write_text('\n'.join(lines) + '\n')
    print({'offered': report['offered_channel_packets'], 'received': report['received_channel_packets'],
           'trace_valid': trace['valid'] if trace else None})
    return 1 if offered['errors'] or received['errors'] or trace and not trace['valid'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
