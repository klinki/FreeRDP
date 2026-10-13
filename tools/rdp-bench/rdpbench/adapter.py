"""Declared private-adapter semantics; never alter upstream transport policy."""
LEGACY_GEOMETRY=dict(own_log_window=7,peer_log_window=7,local_protocol_capacity_packets=127,
    peer_protocol_capacity_packets=127,sent_storage_slots=128,usable_storage_packets=127,
    queue_limit_bytes=131072,wire_mtu=1232,send_window_policy='legacy-peer-capacity')
PR_GEOMETRY=dict(own_log_window=15,peer_log_window=15,local_protocol_capacity_packets=32767,
    peer_protocol_capacity_packets=32767,sent_storage_slots=64,usable_storage_packets=64,
    queue_limit_bytes=131072,wire_mtu=1232,send_window_policy='upstream-v3-native-64')
PR_ADAPTER=dict(id='pr13608-rdpudp',backend=2,stats_semantics='upstream-unified-state',
    expected_geometry=PR_GEOMETRY,peer_advertisement_gates_send=False,
    available_native_counters=['sent_datagrams','received_datagrams','retransmissions',
        'sent_window_entries','receive_buffer_bytes','transport_failed'],
    unavailable_native_counters=['loss_reports','send_failed','receive_failed'],
    available_driver_diagnostics=['driver_error'],
    policy='Upstream PR retry/RTO/ACK and receive policy unchanged; compatibility recovery_mode environment is ignored')

PR_NATIVE_SOURCE_NAMES=('rdpudp.c','rdpudp.h','rdpudp_bench_private.inc','rdpudp_bench_observer.inc','rdpudp_bench_types.inc')
LEGACY_NATIVE_SOURCE_NAMES=('rdpeudp.c','rdpeudp.h','rdp_bench_private.inc','rdp_bench_queue_private.inc','rdp_bench_loss_private.inc')
def native_source_files(core,backend=None):
    from pathlib import Path
    core=Path(core).resolve()
    names=PR_NATIVE_SOURCE_NAMES if backend==2 or (core/'rdpudp_bench_private.inc').is_file() else LEGACY_NATIVE_SOURCE_NAMES
    return {name:core/name for name in names}

def native_source_receipt(core,metadata=None):
    from .common import sha256
    backend=((metadata or {}).get('native_adapter') or {}).get('backend')
    paths=native_source_files(core,backend);missing=[name for name,path in paths.items() if not path.is_file()]
    if not missing:
        return dict(basis='checkout-files',files={'libfreerdp/core/'+name:sha256(path) for name,path in paths.items()})
    recorded=(metadata or {}).get('native_source_sha256')
    if isinstance(recorded,dict) and recorded:
        return dict(basis='declared-build-metadata; local native source absent',files=recorded,missing=missing)
    return dict(basis='unavailable',files=None,missing=missing)
