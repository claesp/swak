#!/usr/bin/env python3
"""Serve bounded CoT XML for development tests. Python 3.9 or later is required."""
import argparse
import asyncio
import datetime
import math
import ssl
from xml.sax.saxutils import quoteattr


def message(index, contacts, lifetime, motion):
    now = datetime.datetime.now(datetime.timezone.utc)
    stamp = lambda value: value.isoformat(timespec="milliseconds").replace("+00:00", "Z")
    longitude = 18.0 + (math.sin(index / 100) * 0.01 if motion else 0)
    if motion and (index // 1000) % 2:
        longitude += 20
    return (f'<event version="2.0" uid="harness-{index % contacts}" type="a-f-G-U" how="m-g" '
            f'time={quoteattr(stamp(now))} start={quoteattr(stamp(now))} '
            f'stale={quoteattr(stamp(now + datetime.timedelta(seconds=lifetime)))}>'
            f'<point lat="59.33" lon="{longitude:.8f}" hae="10" ce="10" le="10"/>'
            f'<detail><contact callsign="Contact {index % contacts}"/>'
            '<__group name="Cyan" role="Team Member"/></detail></event>').encode()


async def run(args):
    tls = None
    if args.cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.cert, args.key)
        if args.client_ca:
            tls.load_verify_locations(args.client_ca)
            tls.verify_mode = ssl.CERT_REQUIRED
    clients = set()

    async def receive(reader):
        # Keep the input buffer bounded without treating received XML as trusted.
        while await reader.read(16384):
            pass

    async def connected(reader, writer):
        if len(clients) >= 16:
            writer.close()
            return
        clients.add(writer)
        task = asyncio.create_task(receive(reader))
        try:
            for first in range(0, args.count, args.batch):
                data = bytearray()
                for index in range(first, min(first + args.batch, args.count)):
                    data.extend(b'<event/>' if args.bad_every and index % args.bad_every == 0
                                else message(index, args.contacts, args.lifetime, args.motion))
                if args.fragment:
                    for offset in range(0, len(data), args.fragment):
                        writer.write(data[offset:offset + args.fragment])
                        await writer.drain()
                        await asyncio.sleep(0)
                else:
                    writer.write(data)
                    await writer.drain()
                await asyncio.sleep(args.batch / args.rate)
            await asyncio.sleep(args.lifetime + 2)
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            clients.discard(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    server = await asyncio.start_server(connected, args.host, args.port, ssl=tls, limit=16384)
    print(f"CoT {'TLS' if tls else 'TCP'} harness on {server.sockets[0].getsockname()}", flush=True)
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8088)
    parser.add_argument('--rate', type=int, default=1000, help='Messages per second, per client')
    parser.add_argument('--count', type=int, default=12000)
    parser.add_argument('--contacts', type=int, default=500)
    parser.add_argument('--batch', type=int, default=50)
    parser.add_argument('--fragment', type=int, default=0)
    parser.add_argument('--bad-every', type=int, default=0)
    parser.add_argument('--lifetime', type=int, default=15)
    parser.add_argument('--motion', action='store_true')
    parser.add_argument('--cert')
    parser.add_argument('--key')
    parser.add_argument('--client-ca')
    args = parser.parse_args()
    if not (1 <= args.port <= 65535 and 1 <= args.rate <= 100000 and 1 <= args.count <= 10000000
            and 1 <= args.contacts <= 4096 and 1 <= args.batch <= 100 and 0 <= args.fragment <= 16384
            and 0 <= args.bad_every <= 100000 and 1 <= args.lifetime <= 86400):
        parser.error('A numeric argument is outside its allowed range.')
    if bool(args.cert) != bool(args.key) or (args.client_ca and not args.cert):
        parser.error('TLS requires --cert and --key. --client-ca also requires TLS.')
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
