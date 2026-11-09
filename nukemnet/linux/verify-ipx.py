#!/usr/bin/env python3
"""Exercise a portable build's IPX server with a full-size guest packet."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=Path, help='path to the package bin/dosbox-x')
parser.add_argument('--qemu', help='QEMU user emulator for a different host architecture')
parser.add_argument('--loader', help='bundled dynamic loader name (required with --qemu)')
parser.add_argument('--temp-dir', help='directory for temporary configuration and logs')
args = parser.parse_args()
if bool(args.qemu) != bool(args.loader):
    parser.error('--qemu and --loader must be used together')

binary = args.binary.resolve()
package = binary.parent.parent
with tempfile.TemporaryDirectory(prefix='nn-dosbox-ipx-', dir=args.temp_dir) as temp:
    directory = Path(temp)
    config = directory / 'test.conf'
    config.write_text('[sdl]\noutput=surface\n[dosbox]\nmachine=svga_s3\n'
                      '[cpu]\ncore=normal\ncycles=3000\n[ipx]\nipx=true\n')
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
    probe.close()
    command = [str(binary)]
    if args.qemu:
        command = [args.qemu, str(package / 'lib' / args.loader), '--library-path',
                   str(package / 'lib'), str(binary)]
    command += ['-conf', str(config), '-noautoexec', '-fastlaunch',
                '-c', f'IPXNET STARTSERVER {port}', '-c', 'pause']
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'HOME': temp}
    with (directory / 'dosbox.log').open('w+') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            peers = []
            for index in range(2):
                peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                peer.bind(('127.0.0.1', 0))
                peer.settimeout(.2)
                mac = bytes([2, 0, 0, 0, 0, index + 1])
                registration = bytearray(36)
                registration[:2] = b'\xff\xff'
                registration[2:4] = struct.pack('!H', len(registration))
                registration[4] = ord('M')
                registration[16:18] = registration[28:30] = b'\x00\x02'
                registration[30:36] = mac
                deadline = time.monotonic() + 120
                while True:
                    if process.poll() is not None:
                        log.flush()
                        raise RuntimeError('DOSBox-X exited: ' + (directory / 'dosbox.log').read_text(errors='replace')[-3000:])
                    if time.monotonic() > deadline:
                        raise RuntimeError('IPX server registration timed out')
                    peer.sendto(registration, ('127.0.0.1', port))
                    try:
                        reply = peer.recv(2048)
                        if len(reply) >= 30 and reply[4] == ord('M'):
                            break
                    except (TimeoutError, socket.timeout):  # distinct before Python 3.10
                        time.sleep(.1)
                peers.append((peer, mac))
            packet = bytearray(1470)
            packet[:2] = b'\xff\xff'
            packet[2:4] = struct.pack('!H', len(packet))
            packet[10:16] = b'\xff' * 6
            packet[16:18] = packet[28:30] = b'\x45\x52'
            packet[22:28] = peers[0][1]
            packet[30:] = bytes(index % 251 for index in range(len(packet) - 30))
            peers[1][0].settimeout(10)
            peers[0][0].sendto(packet, ('127.0.0.1', port))
            received = peers[1][0].recv(2048)
            if received != packet:
                raise RuntimeError(f'IPX packet mismatch: sent {len(packet)}, received {len(received)} bytes')
            print(f'IPX server forwarded {len(received)} bytes without truncation')
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
