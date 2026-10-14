#!/usr/bin/env python3
"""Bounded, independent Xvfb content-progress evidence; no FPS/latency/goodput claim."""
from __future__ import annotations
import argparse
import ctypes as C
import ctypes.util
from datetime import datetime, timezone
import hashlib
import json
import math
import multiprocessing
from pathlib import Path
import signal
import sys
import time

MAX_PIXELS = 1_048_576
MAX_IMAGE_BYTES = 8 * MAX_PIXELS
STOP_SIGNAL = None
SCOPE = ('Low-frequency central-content pixel changes only; not client FPS, input/display '
         'latency, decrypted payload integrity or application goodput. Observer health '
         'and native session/tunnel health are separate.')


def validate_config(seconds, interval, region, capture_timeout, change_fraction):
    values = (seconds, interval, capture_timeout, change_fraction)
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in values):
        raise ValueError('sampling limits must be finite numbers')
    if not .1 <= seconds <= 600 or not .5 <= interval <= 30 or not .1 <= capture_timeout <= 5:
        raise ValueError('seconds=.1..600, interval=.5..30 and capture timeout=.1..5 required')
    if not 0 < change_fraction <= 1:
        raise ValueError('change fraction must be in (0,1]')
    if len(region) != 4 or any(isinstance(v, bool) or not isinstance(v, int) for v in region):
        raise ValueError('region must have four integer coordinates/dimensions')
    x, y, width, height = region
    if min(x, y) < 0 or min(width, height) <= 0 or max(x, y, width, height) > 8192 or width * height > MAX_PIXELS:
        raise ValueError('region must be positive, bounded and at most 1048576 pixels')
    return math.ceil(seconds / interval)


def validate_header(header, width, height):
    if header['width'] != width or header['height'] != height or header['format'] != 2 or header['xoffset'] != 0:
        raise ValueError('XImage dimensions/format differ from the requested region')
    bits = header['bits_per_pixel']
    if bits not in (16, 24, 32) or header['byte_order'] not in (0, 1) or not 0 < header['depth'] <= bits:
        raise ValueError('unsupported XImage pixel/depth/byte-order layout')
    stride = header['bytes_per_line']
    if stride < width * (bits // 8) or stride * height > MAX_IMAGE_BYTES:
        raise ValueError('invalid/unbounded XImage stride')
    masks = [header[name] for name in ('red_mask', 'green_mask', 'blue_mask')]
    if any(not isinstance(m, int) or m <= 0 or m >= 1 << bits for m in masks):
        raise ValueError('invalid XImage colour masks')
    if any(a & b for i, a in enumerate(masks) for b in masks[i + 1:]):
        raise ValueError('overlapping XImage colour masks')
    for mask in masks:
        low = (mask & -mask).bit_length() - 1
        shifted = mask >> low
        if shifted & (shifted + 1):
            raise ValueError('noncontiguous XImage colour mask')
    return stride * height


def canonical_rgb(raw, header):
    """Exclude row padding/unused alpha bits so static content cannot falsely change."""
    width, height = header['width'], header['height']
    required = validate_header(header, width, height)
    if len(raw) != required:
        raise ValueError('XImage byte count differs from its header')
    size = header['bits_per_pixel'] // 8
    stride = header['bytes_per_line']
    packed = b''.join(raw[y * stride:y * stride + width * size] for y in range(height))
    masks = [header[k] for k in ('red_mask', 'green_mask', 'blue_mask')]
    shifts = [(m & -m).bit_length() - 1 for m in masks]
    limits = [m >> s for m, s in zip(masks, shifts)]
    rgb = bytearray(width * height * 3)
    if all(m == 255 and s % 8 == 0 for m, s in zip(limits, shifts)):
        for channel, shift in enumerate(shifts):
            offset = shift // 8 if header['byte_order'] == 0 else size - 1 - shift // 8
            rgb[channel::3] = packed[offset::size]
    else:
        order = 'little' if header['byte_order'] == 0 else 'big'
        for pixel in range(width * height):
            value = int.from_bytes(packed[pixel * size:(pixel + 1) * size], order)
            for channel, (mask, shift, limit) in enumerate(zip(masks, shifts, limits)):
                rgb[pixel * 3 + channel] = ((value & mask) >> shift) * 255 // limit
    return bytes(rgb)


def changed_pixels(previous, current):
    if len(previous) != len(current) or len(current) % 3:
        raise ValueError('RGB comparison dimensions differ')
    if previous == current:
        return 0
    return sum(a != d or b != e or c != f for a, b, c, d, e, f in
               zip(previous[0::3], previous[1::3], previous[2::3],
                   current[0::3], current[1::3], current[2::3]))


class XImage(C.Structure):
    _fields_ = [('width', C.c_int), ('height', C.c_int), ('xoffset', C.c_int), ('format', C.c_int),
                ('data', C.c_void_p), ('byte_order', C.c_int), ('bitmap_unit', C.c_int),
                ('bitmap_bit_order', C.c_int), ('bitmap_pad', C.c_int), ('depth', C.c_int),
                ('bytes_per_line', C.c_int), ('bits_per_pixel', C.c_int),
                ('red_mask', C.c_ulong), ('green_mask', C.c_ulong), ('blue_mask', C.c_ulong),
                ('obdata', C.c_void_p)]


class XErrorEvent(C.Structure):
    _fields_ = [('type', C.c_int), ('display', C.c_void_p), ('resourceid', C.c_ulong),
                ('serial', C.c_ulong), ('error_code', C.c_ubyte), ('request_code', C.c_ubyte),
                ('minor_code', C.c_ubyte)]


def capture_worker(connection, display_name, region, expected_root):
    """Only this owned process calls Xlib; a stuck X server cannot block the collector."""
    display = None
    library = None
    try:
        library = C.CDLL(C.util.find_library('X11') or 'libX11.so.6')
        library.XOpenDisplay.argtypes = [C.c_char_p]; library.XOpenDisplay.restype = C.c_void_p
        library.XCloseDisplay.argtypes = [C.c_void_p]; library.XCloseDisplay.restype = C.c_int
        library.XDefaultRootWindow.argtypes = [C.c_void_p]; library.XDefaultRootWindow.restype = C.c_ulong
        library.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
            C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
        library.XGetGeometry.restype = C.c_int
        library.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint,
                                      C.c_uint, C.c_ulong, C.c_int]
        library.XGetImage.restype = C.POINTER(XImage)
        library.XDestroyImage.argtypes = [C.POINTER(XImage)]; library.XDestroyImage.restype = C.c_int
        library.XSync.argtypes = [C.c_void_p, C.c_int]; library.XSync.restype = C.c_int
        errors = []
        callback_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(XErrorEvent))

        @callback_type
        def on_error(_display, event):
            if len(errors) < 8:
                errors.append(dict(code=event.contents.error_code, request=event.contents.request_code,
                                   minor=event.contents.minor_code))
            return 0

        library.XSetErrorHandler.argtypes = [callback_type]; library.XSetErrorHandler.restype = C.c_void_p
        library.XSetErrorHandler(on_error)
        display = library.XOpenDisplay(display_name.encode())
        if not display:
            raise RuntimeError('XOpenDisplay failed')
        root = library.XDefaultRootWindow(display)
        returned_root = C.c_ulong(); x = C.c_int(); y = C.c_int()
        width = C.c_uint(); height = C.c_uint(); border = C.c_uint(); depth = C.c_uint()
        if not library.XGetGeometry(display, root, C.byref(returned_root), C.byref(x), C.byref(y),
                                    C.byref(width), C.byref(height), C.byref(border), C.byref(depth)):
            raise RuntimeError('XGetGeometry failed')
        rx, ry, rw, rh = region
        if [width.value, height.value] != expected_root or rx + rw > width.value or ry + rh > height.value:
            raise ValueError('Xvfb geometry differs or content region is outside the root')
        connection.send(dict(type='ready',root_width=width.value,root_height=height.value,root_depth=depth.value))
        while connection.recv() == 'capture':
            errors.clear()
            started = time.monotonic_ns()
            image = library.XGetImage(display, root, rx, ry, rw, rh, C.c_ulong(-1).value, 2)
            try:
                library.XSync(display, 0)
                if errors or not image:
                    raise RuntimeError('XGetImage failed: ' + repr(errors))
                obj = image.contents
                names = ('width','height','xoffset','format','byte_order','depth','bytes_per_line',
                         'bits_per_pixel','red_mask','green_mask','blue_mask')
                header = {name:int(getattr(obj,name)) for name in names}
                length = validate_header(header, rw, rh)
                if not obj.data:
                    raise RuntimeError('XImage data pointer is null')
                rgb = canonical_rgb(C.string_at(obj.data, length), header)
                ended = time.monotonic_ns()
                connection.send(dict(type='image',rgb=rgb,header=header,
                                     capture_start_ns=started,capture_end_ns=ended))
            finally:
                if image:
                    library.XDestroyImage(image)
    except (Exception, EOFError) as error:
        try:
            connection.send(dict(type='error',error=repr(error)))
        except (BrokenPipeError, EOFError, OSError):
            pass
    finally:
        if display and library:
            library.XCloseDisplay(display)
        connection.close()


def receive_bounded(connection, process, timeout):
    end = time.monotonic() + timeout
    while STOP_SIGNAL is None and time.monotonic() < end:
        if connection.poll(min(.1,max(0,end-time.monotonic()))):
            return connection.recv()
        if not process.is_alive():
            raise RuntimeError('Xlib worker exited without a result: ' + str(process.exitcode))
    if STOP_SIGNAL is not None:
        raise InterruptedError('collector stopped by signal')
    raise TimeoutError('bounded Xlib worker wait exceeded')


def write_ppm(path, width, height, rgb):
    if len(rgb) != width * height * 3:
        raise ValueError('PPM RGB byte count differs from dimensions')
    with Path(path).open('xb') as stream:
        stream.write(f'P6\n{width} {height}\n255\n'.encode() + rgb)


def run(args):
    planned = validate_config(args.seconds,args.interval,args.region,args.capture_timeout,args.change_fraction)
    if len(args.expect_root) != 2 or any(not isinstance(v,int) or v <= 0 or v > 8192 for v in args.expect_root):
        raise ValueError('expected root must have two positive bounded dimensions')
    if args.start_ns is not None and (args.start_ns <= 0 or args.start_ns < time.monotonic_ns()-1_000_000_000):
        raise ValueError('start-ns must be positive and not more than one second in the past')
    output = args.output.resolve(); summary_path = output.with_suffix('.summary.json')
    first_path = output.with_suffix('.first.ppm'); last_path = output.with_suffix('.last.ppm')
    for path in (output,summary_path,*((first_path,last_path) if args.save_ppm else ())):
        if path.exists():
            raise ValueError('preserve existing observer artifact: ' + str(path))
    output.parent.mkdir(parents=True,exist_ok=True)
    process = connection = None
    start = end = None
    samples = changes = substantial = missed = 0
    previous = first = last = None
    first_at = last_at = None
    errors = []; stop_reason = 'planned_end'; worker_cleanup = None
    with output.open('x',encoding='utf-8',buffering=1) as stream:
        def emit(kind, **fields):
            before = time.monotonic_ns()
            epoch = time.time_ns()
            after = time.monotonic_ns()
            offset = epoch-(before+after)//2
            for name in ('capture_start_ns','capture_end_ns'):
                if name in fields:
                    fields[name.replace('_ns','_utc_ns_estimate')] = fields[name]+offset
            stream.write(json.dumps(dict(type=kind,monotonic_ns=after,utc_ns=epoch,
                utc_minus_monotonic_ns=offset,utc_pair_uncertainty_ns=(after-before+1)//2,
                utc=datetime.fromtimestamp(epoch/1e9,timezone.utc).isoformat(),**fields),allow_nan=False)+'\n')
        emit('metadata',schema='freerdp.xvfb_content_progress',version=1,scope=SCOPE,
             display=args.display,region=args.region,expected_root=args.expect_root,interval_s=args.interval,
             seconds=args.seconds,planned_samples=planned,substantial_change_fraction=args.change_fraction,
             requested_start_ns=args.start_ns,ppm_enabled=args.save_ppm)
        try:
            context = multiprocessing.get_context('spawn')
            connection, child = context.Pipe()
            process = context.Process(target=capture_worker,args=(child,args.display,args.region,args.expect_root),daemon=True)
            process.start(); child.close()
            ready = receive_bounded(connection,process,5)
            if ready.get('type') != 'ready':
                raise RuntimeError('Xlib startup failed: ' + repr(ready))
            emit('ready',worker_pid=process.pid,geometry=ready)
            start = args.start_ns if args.start_ns is not None else time.monotonic_ns()
            end = start + int(args.seconds*1e9)
            interval = int(args.interval*1e9)
            index = 0
            while index < planned and STOP_SIGNAL is None:
                scheduled = start + index*interval
                while STOP_SIGNAL is None and time.monotonic_ns() < scheduled:
                    time.sleep(max(0,min(.05,(scheduled-time.monotonic_ns())/1e9)))
                if STOP_SIGNAL is not None:
                    break
                now = time.monotonic_ns()
                if now >= end:
                    missed += planned-index
                    break
                if now-scheduled >= interval:
                    skipped = min((now-scheduled)//interval,planned-index)
                    emit('missed_sample',first_index=index,count=skipped,reason='observer_schedule_lateness')
                    missed += skipped; index += skipped
                    continue
                connection.send('capture')
                value = receive_bounded(connection,process,args.capture_timeout)
                if value.get('type') != 'image':
                    raise RuntimeError('Xlib capture failed: ' + repr(value))
                rgb = value['rgb']; finished = value['capture_end_ns']
                inside = start <= value['capture_start_ns'] <= finished < end
                if not inside:
                    missed += 1
                    emit('excluded_sample',index=index,reason='capture_crossed_measurement_boundary',
                         capture_start_ns=value['capture_start_ns'],capture_end_ns=finished)
                    index += 1
                    continue
                count = changed_pixels(previous,rgb) if previous is not None else None
                ratio = count/(args.region[2]*args.region[3]) if count is not None else None
                if count is not None:
                    changes += count > 0
                    substantial += ratio >= args.change_fraction
                emit('sample',index=index,scheduled_ns=scheduled,capture_start_ns=value['capture_start_ns'],
                     capture_end_ns=finished,capture_duration_ms=(finished-value['capture_start_ns'])/1e6,
                     rgb_sha256=hashlib.sha256(rgb).hexdigest(),pixels=args.region[2]*args.region[3],
                     changed_pixels=count,changed_fraction=ratio,
                     substantial_change=ratio >= args.change_fraction if ratio is not None else None,
                     compared_previous_capture_end_ns=last_at,header=value['header'])
                if first is None:
                    first = rgb; first_at = finished
                last = previous = rgb; last_at = finished; samples += 1; index += 1
            # Samples are taken at [start,end); remain alive through the exact
            # end rather than declaring a120-second observer done at second119.
            while STOP_SIGNAL is None and end is not None and time.monotonic_ns() < end:
                time.sleep(max(0,min(.05,(end-time.monotonic_ns())/1e9)))
        except InterruptedError:
            stop_reason = 'signal'
        except Exception as error:
            errors.append(repr(error)); stop_reason = 'observer_error'
            emit('error',error=repr(error))
        finally:
            if STOP_SIGNAL is not None:
                stop_reason = 'signal'
            if process:
                if process.is_alive():
                    try: connection.send('stop')
                    except (BrokenPipeError,EOFError,OSError): pass
                process.join(.2)
                if process.is_alive():
                    process.terminate(); process.join(.5); worker_cleanup = 'terminated_owned_worker'
                if process.is_alive():
                    process.kill(); process.join(.5); worker_cleanup = 'killed_owned_worker'
                if worker_cleanup is None:
                    worker_cleanup = 'owned_worker_exited'
                if process.is_alive():
                    errors.append('owned Xlib worker did not exit within cleanup bounds')
            if connection: connection.close()
            if args.save_ppm and first is not None:
                try:
                    write_ppm(first_path,args.region[2],args.region[3],first)
                    write_ppm(last_path,args.region[2],args.region[3],last)
                except Exception as error:
                    errors.append('PPM export: '+repr(error))
            ended = time.monotonic_ns()
            result = dict(schema='freerdp.xvfb_content_progress',version=1,scope=SCOPE,
                observer_valid=not errors and samples >= 2,samples=samples,planned_samples=planned,
                completed_measurement_window=start is not None and end is not None and
                    ended >= end and stop_reason == 'planned_end',
                sampling_complete=not errors and samples == planned and
                    start is not None and end is not None and ended >= end and stop_reason == 'planned_end',
                missed_samples=planned-samples,missed_due_to_schedule_or_boundary=missed,
                unsampled_after_stop_or_capture_failure=max(0,planned-samples-missed),
                comparison_intervals=max(0,samples-1),changed_intervals=changes,
                substantial_change_intervals=substantial,substantial_change_fraction=args.change_fraction,
                region=args.region,scheduled_start_ns=start,scheduled_end_ns=end,finished_ns=ended,
                first_capture_end_ns=first_at,last_capture_end_ns=last_at,
                coverage_s=(last_at-first_at)/1e9 if samples >= 2 else None,
                stop_reason=stop_reason,stop_signal=STOP_SIGNAL,worker_cleanup=worker_cleanup,errors=errors,
                first_ppm=str(first_path) if args.save_ppm and first is not None else None,
                last_ppm=str(last_path) if args.save_ppm and last is not None else None,
                qualification='Pixel hashes are canonical RGB; unused bits/padding are excluded. '
                    'Substantial change is a conservative content diagnostic, not an automatic native pass.')
            emit('footer',result=result)
            with summary_path.open('x',encoding='utf-8') as target:
                json.dump(result,target,indent=2,allow_nan=False);target.write('\n')
    print(json.dumps(dict(summary=str(summary_path),samples=samples,observer_valid=result['observer_valid'],errors=errors)))
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--seconds',type=float,default=120)
    parser.add_argument('--interval',type=float,default=1)
    parser.add_argument('--region',type=int,nargs=4,default=[448,280,384,240],metavar=('X','Y','W','H'))
    parser.add_argument('--display',default=':99')
    parser.add_argument('--expect-root',type=int,nargs=2,default=[1280,800],metavar=('W','H'))
    parser.add_argument('--start-ns',type=int)
    parser.add_argument('--capture-timeout',type=float,default=2)
    parser.add_argument('--change-fraction',type=float,default=.05)
    parser.add_argument('--save-ppm',action='store_true')
    args = parser.parse_args()
    def stopped(signum,_frame):
        global STOP_SIGNAL
        STOP_SIGNAL = signum
    signal.signal(signal.SIGTERM,stopped);signal.signal(signal.SIGINT,stopped)
    try: return run(args)
    except (ValueError,OSError) as error:
        print('image observer configuration/output error: '+str(error),file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
