#!/usr/bin/env python3
"""Create a small diagnostic summary without copying packet/ACK ledgers to Git.

The large source analysis stays local/in the ZIP. Captured active-attempt ACKs,
historical AoA-retired references and native-only observations remain separate.
"""
from __future__ import annotations
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path


def sha256(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):
            digest.update(chunk)
    return digest.hexdigest()


def integer(value):
    return isinstance(value,int) and not isinstance(value,bool)


def unix_ns(value):
    if not isinstance(value,str):return None
    try:
        stamp=datetime.fromisoformat(value.replace('Z','+00:00'))
        if stamp.tzinfo is None:return None
        delta=stamp.astimezone(timezone.utc)-datetime(1970,1,1,tzinfo=timezone.utc)
        return (delta.days*86400+delta.seconds)*1_000_000_000+delta.microseconds*1000
    except ValueError:
        return None


def profile_from_campaign(campaign, reset=None):
    if not isinstance(campaign,dict):raise ValueError('campaign must be an object')
    windows=[]
    for step in campaign.get('steps',[]):
        if not isinstance(step,dict):raise ValueError('campaign step must be an object')
        windows.append(dict(id=step.get('id'),loss_percent=step.get('loss_percent'),
                            start_unix_ns=unix_ns(step.get('start_utc')),
                            end_unix_ns=unix_ns(step.get('end_utc')),
                            outcome=step.get('outcome'),requested_s=step.get('requested_s')))
    state=reset.get('state',{}) if isinstance(reset,dict) else {}
    zero=unix_ns(state.get('changed_utc')) if isinstance(state,dict) and state.get('loss_percent')==0 else None
    return dict(measurement_steps=windows,loss_reset_zero_receipt_unix_ns=zero,
                reset_receipt_scope='changed_utc is after zero loss was applied; an upper bound, not the exact mutation instant')


def within(value, bounds):
    return bool(integer(value) and integer(bounds.get('first_time_ns')) and integer(bounds.get('last_time_ns'))
                and bounds['first_time_ns']<=value<=bounds['last_time_ns'])


def gap_scope(report, profile):
    gap=report.get('focused_gap')
    alignment=report['native_trace'].get('alignment')
    result=dict(channel=report.get('channel'),duration_ms=gap.get('duration_ms') if isinstance(gap,dict) else None,
                max_gap_age_ms=gap.get('max_gap_age_ms') if isinstance(gap,dict) else None,
                max_buffered_chunks=gap.get('max_buffered') if isinstance(gap,dict) else None,
                recovered_in_native_trace=gap.get('recovered') if isinstance(gap,dict) else None,
                started_before_arm=gap.get('started_before_arm') if isinstance(gap,dict) else None,
                peer_age_ms_at_end=gap.get('peer_age_ms_at_end') if isinstance(gap,dict) else None,
                native_start_ns=gap.get('start_ns') if isinstance(gap,dict) else None,
                native_end_ns=gap.get('end_ns') if isinstance(gap,dict) else None,
                wall_start_envelope_ns=None,wall_end_envelope_ns=None,
                capture_observed_bounds=report.get('captures',{}),inside_both_capture_observed_windows=None,
                inside_measured_loss_step=None,recovered_after_zero_loss_receipt=None,
                profile_windows=profile.get('measurement_steps',[]) if profile else [])
    # Keep only capture bounds/identities, never decoder rows or packet errors.
    result['capture_observed_bounds']={name:{key:value.get(key) for key in
        ('first_time_ns','last_time_ns','record_count','snaplen','linktype','error_count')}
        for name,value in report.get('captures',{}).items()}
    if not isinstance(gap,dict) or not isinstance(alignment,dict):return result
    if not all(integer(x) for x in (gap.get('start_ns'),gap.get('end_ns'),alignment.get('offset_min_ns'),alignment.get('offset_max_ns'))):
        return result
    slop=round(report['native_trace'].get('clock_slop_ms',0)*1_000_000)
    start=[gap['start_ns']+alignment['offset_min_ns']-slop,
           gap['start_ns']+alignment['offset_max_ns']+slop]
    end=[gap['end_ns']+alignment['offset_min_ns']-slop,
         gap['end_ns']+alignment['offset_max_ns']+slop]
    result.update(wall_start_envelope_ns=start,wall_end_envelope_ns=end)
    bounds=result['capture_observed_bounds']
    if all(isinstance(bounds.get(name),dict) and integer(bounds[name].get('first_time_ns')) and
           integer(bounds[name].get('last_time_ns')) for name in ('offered','received')):
        result['inside_both_capture_observed_windows']=all(
            start[0]>=bounds[name]['first_time_ns'] and end[1]<=bounds[name]['last_time_ns']
            for name in ('offered','received'))
        result['native_recovery_after_received_last_packet_ms']=(
            (end[0]+end[1])//2-bounds['received']['last_time_ns'])/1e6
    windows=[w for w in result['profile_windows'] if integer(w.get('start_unix_ns')) and integer(w.get('end_unix_ns'))]
    if windows:
        result['inside_measured_loss_step']=any(start[0]>=w['start_unix_ns'] and end[1]<=w['end_unix_ns'] for w in windows)
    zero=profile.get('loss_reset_zero_receipt_unix_ns') if profile else None
    result['loss_reset_zero_receipt_unix_ns']=zero
    if integer(zero):result['recovered_after_zero_loss_receipt']=end[0]>zero
    return result


def compact(report, profile=None):
    if not isinstance(report,dict) or report.get('schema')!='freerdp.pr13608.recovery_audit' or report.get('version')!=1:
        raise ValueError('unsupported recovery analysis schema')
    for key in ('native_trace','native_flow_association','capture_quality','captures','conclusion','inputs'):
        if not isinstance(report.get(key),dict):raise ValueError('invalid analysis '+key)
    if not isinstance(report.get('aliases'),list):raise ValueError('invalid analysis aliases')
    native=report['native_trace'];association=report['native_flow_association']
    native_complete=bool(native.get('retained_valid') is True and native.get('complete_from_arm') is True and
                         not native.get('errors') and not native.get('header_errors') and native.get('rows_truncated') is False)
    healthy=all(report['capture_quality'].get(name,{}).get('absence_qualified') is True for name in ('offered','received'))
    qualification=native_complete and healthy and association.get('verified') is True
    totals=Counter();by_kind=Counter();aliases=[]
    for alias in report['aliases']:
        if not isinstance(alias,dict) or not isinstance(alias.get('feedback'),list):raise ValueError('invalid alias/feedback')
        counts=Counter();kinds=Counter()
        for ack in alias['feedback']:
            if not isinstance(ack,dict):raise ValueError('invalid ACK reference')
            kind=ack.get('kind');kinds[kind]+=1;by_kind[kind]+=1
            positive=ack.get('positive_receipt') is True
            retired=bool(ack.get('peer_aoa_retirement_events'))
            wire=bool(ack.get('wire_capture_matches'))
            in_window=within(ack.get('time_unix_ns'),report['captures'].get('offered',{}))
            epoch=ack.get('sequence_epoch_verified') is True
            active=positive and not retired
            eligible=active and epoch and wire and in_window and qualification and not alias.get('serial_ambiguous')
            received=bool(ack.get('native_received_before_ack') or ack.get('initial_data_receipt_possible'))
            declared=ack.get('qualified_unretained_ack') is True
            counts['positive_references']+=positive
            counts['positive_references_retired_by_peer_aoa']+=positive and retired
            counts['positive_current_attempt_references']+=active
            counts['wire_observed_references']+=wire
            counts['outside_offered_observed_packet_window']+=not in_window
            counts['eligible_captured_active_positive_references']+=eligible
            counts['eligible_current_attempt_receipts_with_native_receipt_evidence']+=eligible and received
            counts['raw_analyzer_qualified_violation_references']+=declared
            counts['eligible_captured_active_violation_references']+=declared and eligible
            counts['native_only_or_otherwise_unqualified_violation_references']+=declared and not eligible
        totals.update(counts)
        rx=alias.get('native_rx') or []
        aliases.append(dict(flow=alias.get('flow'),data_seq=alias.get('data_seq'),data_extended=alias.get('data_extended'),
                            channel_extended=alias.get('channel_extended'),offered_time_ns=alias.get('offered_time_ns'),
                            postloss_matches=len(alias.get('received_packets') or []),
                            bounded_no_postloss_match_qualified=alias.get('no_postloss_match_qualified') is True,
                            serial_ambiguous=alias.get('serial_ambiguous') is True,
                            native_rx_status_counts=dict(Counter(str(e.get('status')) for e in rx)),
                            ack_reference_kinds=dict(kinds),ack_qualification=dict(counts)))
    scope=gap_scope(report,profile)
    resync=[e for e in report.get('peer_aoa_events',[]) if e.get('event')=='rx_resync']
    first_resync=resync[0] if resync else None
    ambiguity=dict(ambiguous_aliases=sum(alias['serial_ambiguous'] for alias in aliases),
                   receiver_resync_count=len(resync),
                   first_receiver_resync={key:first_resync.get(key) for key in
                       ('id','time_ns','data_seq','detail','base','next_channel','buffered','peer_age_ms','gap_age_ms')}
                       if first_resync else None,
                   offered_channel_extended_values=sorted({x['channel_extended'] for x in aliases if integer(x['channel_extended'])}),
                   repeated_alias_data_labels=sum(count>1 for count in Counter((x['flow'],x['data_seq']) for x in aliases).values()),
                   capture_serial_ambiguity={name:quality.get('serial_ambiguous') for name,quality in report['capture_quality'].items()},
                   scope='a receiver resync conservatively invalidates continuous-epoch active-ACK qualification for the focused window; does not imply capture corruption')
    return dict(schema='freerdp.pr13608.recovery_compact',version=1,source_analysis_schema=report['schema'],
                source_analysis_version=report['version'],performance_eligible=False,scope=report.get('scope'),
                channel=report.get('channel'),conclusion=report['conclusion'],gap_scope=scope,
                native_qualification={key:native.get(key) for key in ('retained_valid','complete_from_arm','overwritten_events','rows_truncated','alignment','clock_slop_ms')},
                native_error_counts=dict(trace=len(native.get('errors') or []),headers=len(native.get('header_errors') or [])),
                capture_qualification=report['capture_quality'],
                flow_association={key:association.get(key) for key in ('verified','flow','unique_header_matches','ambiguous_header_matches','required_unique_matches','method','scope')},
                ack_qualification=dict(base_eligibility=qualification,reference_counts=dict(totals),reference_kinds=dict(by_kind)),
                serial_ambiguity=ambiguity,
                alias_count=len(aliases),aliases=aliases,observed_inter_alias_offer_ms=report.get('observed_inter_alias_offer_ms',[]),
                native_channel_rx_status_counts=dict(Counter(str(e.get('status')) for e in report.get('native_rx_for_channel',[]))),
                native_channel_delivery_count=len(report.get('native_channel_deliveries',[])),
                native_slot_collision_count=len(report.get('native_slot_collisions',[])),source_inputs=report['inputs'],
                notes=['This compact summary omits all per-packet/per-ACK arrays; detailed analyses and raw evidence remain local/in the ZIP.',
                       'ACK counts are reference counts per alias, not unique ACK datagrams or global receipt totals.',
                       'Historical AoA-retired attempts are separate from captured active-attempt receipt eligibility.',
                       'Wire observations prove emitted/captured transport headers, not Windows acceptance, TLS plaintext or rendering.',
                       'Capture bounds are first/last observed packet times, not recorder lifecycle timestamps.',
                       'Native recovery beyond the capture or measured profile window cannot be described as captured recovery under that profile.',
                       'The clock-slop envelope is declared; independent drift/clock-step review remains necessary.',
                       'These observer-enabled diagnostics do not replace production benchmark failures.'])


def markdown(summary):
    gap=summary['gap_scope'];acks=summary['ack_qualification']['reference_counts']
    flow=summary['flow_association']
    def display(value):
        return 'unknown' if value is None else str(value)
    lines=['# Header-only recovery audit — compact summary','',
           f"Channel `{summary['channel']:04x}`; native gap **{display(gap['duration_ms'])} ms**; "
           f"peak buffered chunks **{display(gap['max_buffered_chunks'])}**.",'',
           f"Native flow association: **{display(flow.get('verified'))}**, `{display(flow.get('flow'))}`.",'',
           f"Whole gap inside both observed capture windows: **{display(gap['inside_both_capture_observed_windows'])}**.",
           f"Whole gap inside a measured loss step: **{display(gap['inside_measured_loss_step'])}**.",
           f"Native recovery after the zero-loss receipt: **{display(gap['recovered_after_zero_loss_receipt'])}**.",'',
           f"Eligible captured active-attempt positive ACK references: **{acks.get('eligible_captured_active_positive_references',0)}**. "
           f"Eligible violation references: **{acks.get('eligible_captured_active_violation_references',0)}**.",
           f"Historical positive references retired by peer AoA: **{acks.get('positive_references_retired_by_peer_aoa',0)}**.",'',
           f"Serial-ambiguous aliases: **{summary['serial_ambiguity']['ambiguous_aliases']}**; "
           f"receiver resyncs in the focused window: **{summary['serial_ambiguity']['receiver_resync_count']}**.",
           summary['serial_ambiguity']['scope'],'',
           '| Offered DataSeq | Postloss matches | Native RX status counts | Eligible active positive ACK refs | AoA-retired positive refs |',
           '|---|---:|---|---:|---:|']
    for alias in summary['aliases']:
        counts=alias['ack_qualification']
        lines.append(f"| `{alias['data_seq']:04x}` | {alias['postloss_matches']} | "
                     f"{json.dumps(alias['native_rx_status_counts'],sort_keys=True)} | "
                     f"{counts.get('eligible_captured_active_positive_references',0)} | "
                     f"{counts.get('positive_references_retired_by_peer_aoa',0)} |")
    lines+=['','Observed inter-alias offer intervals (ms): '+', '.join(str(x) for x in summary['observed_inter_alias_offer_ms'])+'.','',
            '## Source hashes','',f"Analysis: `{summary.get('analysis_sha256','unknown')}`.",
            f"Compact generator: `{summary.get('generator_sha256','unknown')}`.",'']
    for name,receipt in summary['source_inputs'].items():
        lines.append(f"- {name}: `{receipt.get('sha256','unknown')}`")
    for name,receipt in summary.get('extra_source_receipts',{}).items():
        lines.append(f"- {name}: `{receipt.get('sha256','unknown')}`")
    lines+=['','## Qualification','']+['- '+note for note in summary['notes']]
    return '\n'.join(lines)+'\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--analysis',type=Path,required=True)
    parser.add_argument('--trial',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve()
    if output.suffix!='.json':parser.error('compact output must use a .json suffix')
    campaign=args.trial/'ramp/campaign.json'
    reset=args.trial/'ramp/network-reset-zero.json'
    provenance=args.trial/'provenance.json'
    sources=[args.analysis,campaign,reset,provenance]
    protected=[path.resolve() for path in sources]+[args.analysis.with_suffix('.md').resolve()]
    targets=[output,output.with_suffix('.md')]
    if any(target in protected or any((parent/'SHA256SUMS.json').exists() for parent in target.parents) for target in targets):
        parser.error('output must not overwrite inputs or enter sealed artifact folders')
    report=json.loads(args.analysis.read_text())
    profile=profile_from_campaign(json.loads(campaign.read_text()),json.loads(reset.read_text()) if reset.is_file() else None) if campaign.is_file() else None
    summary=compact(report,profile)
    summary['generator_sha256']=sha256(__file__)
    summary['analysis_sha256']=sha256(args.analysis)
    summary['extra_source_receipts']={path.name:dict(path=str(path.resolve()),sha256=sha256(path)) for path in sources[1:] if path.is_file()}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(summary,indent=2,allow_nan=False)+'\n')
    output.with_suffix('.md').write_text(markdown(summary))
    print(json.dumps(dict(output=str(output),bytes=output.stat().st_size,
                          eligible_active_violations=summary['ack_qualification']['reference_counts'].get('eligible_captured_active_violation_references',0))))


if __name__=='__main__':main()
