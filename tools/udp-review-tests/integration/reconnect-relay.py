"""Bounded VM reconnect diagnostic. Relays TCP+UDP, then breaks only this session.
Run with RDP_TEST_PASSWORD in the environment. Logs are under /tmp.
"""
import asyncio, os, time
from pathlib import Path

HOST = '192.168.64.2'
PORT = 13389
LOG = Path(os.environ.get('RDP_TEST_LOG', '/tmp/rdp-vm-reconnect-client.log'))
OUTAGE = int(os.environ.get('RDP_TEST_OUTAGE', '15'))
RETRIES = int(os.environ.get('RDP_TEST_RETRIES', '10'))
start = time.monotonic()
offline = False
connections = set()
attempts = 0

def report(message):
    print(f'{time.monotonic()-start:6.1f}s {message}', flush=True)

async def tcp(reader, writer):
    global attempts
    attempts += 1
    report(f'TCP attempt {attempts}, offline={offline}')
    if offline:
        writer.close()
        return
    try:
        remote_reader, remote_writer = await asyncio.open_connection(HOST, 3389)
    except OSError as e:
        report(f'upstream failed: {e}')
        writer.close()
        return
    pair = (writer, remote_writer)
    connections.add(pair)
    async def copy(src, dst):
        try:
            while data := await src.read(65536):
                dst.write(data)
                await dst.drain()
        except (OSError, asyncio.CancelledError):
            pass
        finally:
            dst.close()
    try:
        await asyncio.gather(copy(reader, remote_writer), copy(remote_reader, writer))
    finally:
        connections.discard(pair)

class Udp(asyncio.DatagramProtocol):
    def __init__(self, upstream=False):
        self.upstream = upstream
        self.peer = None
        self.other = None
        self.count = 0
    def connection_made(self, transport):
        self.transport = transport
    def datagram_received(self, data, addr):
        self.count += 1
        if offline:
            return
        if self.upstream:
            if self.other.peer:
                self.other.transport.sendto(data, self.other.peer)
        else:
            self.peer = addr
            self.other.transport.sendto(data)

async def main():
    global offline
    loop = asyncio.get_running_loop()
    server = await asyncio.start_server(tcp, '127.0.0.1', PORT)
    local = Udp()
    upstream = Udp(True)
    local.other, upstream.other = upstream, local
    lt, _ = await loop.create_datagram_endpoint(lambda: local, local_addr=('127.0.0.1', PORT))
    ut, _ = await loop.create_datagram_endpoint(lambda: upstream, remote_addr=(HOST, 3389))
    proc = None
    try:
        with LOG.open('w') as log:
            proc = await asyncio.create_subprocess_exec(
                '/tmp/freerdp-build/client/SDL/SDL3/sdl-freerdp',
                f'/v:127.0.0.1:{PORT}', '/u:David', '/p:'+os.environ['RDP_TEST_PASSWORD'],
                '/cert:tofu', '/size:1280x800', '+multitransport',
                '/auto-reconnect', f'/auto-reconnect-max-retries:{RETRIES}', '/log-level:DEBUG',
                stdin=asyncio.subprocess.DEVNULL, stdout=log, stderr=log)
            report(f'client pid={proc.pid}')
            # Wait for real session activation before interrupting it.
            for _ in range(90):
                await asyncio.sleep(1)
                if proc.returncode is not None:
                    report(f'client exited before active: {proc.returncode}')
                    return
                if 'CONNECTION_STATE_ACTIVE' in LOG.read_text(errors='replace'):
                    break
            else:
                report('No active session; no fault injected')
                return
            await asyncio.sleep(10)
            report(f'BREAK for {OUTAGE} seconds; UDP packets client={local.count}, server={upstream.count}')
            offline = True
            for pair in list(connections):
                for writer in pair:
                    writer.transport.abort()
            for elapsed in range(OUTAGE):
                await asyncio.sleep(1)
                if elapsed % 30 == 29:
                    report(f'OUTAGE {elapsed+1}s; attempts={attempts}, clientExit={proc.returncode}')
            offline = False
            report('RESTORED relay')
            await asyncio.sleep(40)
            report(f'END observation; attempts={attempts}, clientExit={proc.returncode}, UDP client={local.count} server={upstream.count}')
    finally:
        if proc and proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 8)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        server.close()
        await server.wait_closed()
        lt.close()
        ut.close()

asyncio.run(main())
