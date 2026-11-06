#!/usr/bin/env python3
"""Improv Wi-Fi over serial from the command line: what the web installer
does after flashing, without a browser. Also the QEMU self test's client.

  tools/improv.py PORT info              firmware, version, state, page URL
  tools/improv.py PORT scan              the networks the board sees
  tools/improv.py PORT wifi SSID         joins a network (asks for the password,
                                         or IMPROV_PASSWORD)

PORT: a serial port (/dev/ttyUSB0, /dev/cu.usbserial-...; needs pyserial),
or tcp:HOST:PORT (QEMU's serial chardev). The board's log goes by on the same
port and is ignored; packets start a line with "IMPROV".
"""
import getpass
import os
import socket
import sys
import time

TYPE_STATE, TYPE_ERROR, TYPE_RPC, TYPE_RESULT = 1, 2, 3, 4
CMD_WIFI, CMD_STATE, CMD_INFO, CMD_SCAN = 1, 2, 3, 4
STATES = {0: 'stopped', 2: 'ready (no network)', 3: 'joining', 4: 'on the network'}
ERRORS = {0: 'none', 1: 'invalid packet', 2: 'unknown command', 3: 'unable to connect', 0xff: 'unknown error'}


class Port:
    def __init__(self, name):
        if name.startswith('tcp:'):
            host, port = name[4:].rsplit(':', 1)
            self.sock = socket.create_connection((host, int(port)), timeout=10)
            self.sock.settimeout(0.2)
            self.ser = None
        else:
            import serial     # pyserial (PlatformIO's Python has it)
            # no DTR/RTS: on most boards they drive reset and BOOT
            self.ser = serial.Serial()
            self.ser.port, self.ser.baudrate, self.ser.timeout = name, 115200, 0.2
            self.ser.dtr = self.ser.rts = False
            self.ser.open()
        self.buf = b''

    def write(self, data):
        self.sock.sendall(data) if self.ser is None else self.ser.write(data)

    def read(self):
        if self.ser is not None:
            return self.ser.read(256)
        try:
            return self.sock.recv(256)
        except socket.timeout:
            return b''

    def packets(self, timeout):
        """Yields (type, data) for each valid packet until timeout seconds pass."""
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self.read()
            while True:
                i = self.buf.find(b'IMPROV')
                if i < 0:
                    self.buf = self.buf[-5:]
                    break
                p = self.buf[i:]
                if len(p) >= 7 and p[6] != 1:     # "IMPROV" in a log line
                    self.buf = p[1:]
                    continue
                if len(p) < 9 or len(p) < 10 + p[8]:
                    self.buf = p
                    break
                n = p[8]
                if sum(p[:9 + n]) & 0xff != p[9 + n]:
                    self.buf = p[1:]
                    continue
                self.buf = p[10 + n:]
                yield p[7], p[9:9 + n]


def packet(type_, data):
    p = b'IMPROV' + bytes([1, type_, len(data)]) + bytes(data)
    return p + bytes([sum(p) & 0xff]) + b'\n'


def rpc(cmd, data=b''):
    return packet(TYPE_RPC, bytes([cmd, len(data)]) + data)


def strings(data):
    out, i = [], 2
    while i < 2 + data[1]:
        out.append(data[i + 1:i + 1 + data[i]].decode('utf-8', 'replace'))
        i += 1 + data[i]
    return out


def request(port, cmd, data=b'', timeout=5, until_result=True, many=False):
    """Sends an RPC; returns (state, error, results) seen until its answer."""
    port.write(rpc(cmd, data))
    state, error, results = None, None, []
    for t, d in port.packets(timeout):
        if t == TYPE_STATE:
            state = d[0]
            if not until_result or (cmd == CMD_STATE and state != 4):
                break
        elif t == TYPE_ERROR:
            error = d[0]
            if error:
                break
        elif t == TYPE_RESULT and d[0] == cmd:
            s = [x for x in strings(d) if x != ''] if many else strings(d)
            if many and s:
                results.append(s)
                continue
            if not many:
                results = s
            break
    return state, error, results


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in ('info', 'scan', 'wifi') or (sys.argv[2] == 'wifi' and len(sys.argv) < 4):
        sys.exit(__doc__)
    port = Port(sys.argv[1])
    what = sys.argv[2]
    # the firmware may still be starting: ask the state until it answers
    for _ in range(15):
        state, error, res = request(port, CMD_STATE, timeout=1)
        if state is not None:
            break
    else:
        sys.exit('no answer: not this firmware, or the wrong port (use the COM port, not "USB")')
    if what == 'info':
        _, _, info = request(port, CMD_INFO)
        print('firmware: %s' % ' '.join(info[:2]) if len(info) >= 2 else 'firmware: ?')
        if len(info) >= 4:
            print('chip:     %s\nname:     %s' % (info[2], info[3]))
        print('state:    %s' % STATES.get(state, state))
        if res:
            print('page:     %s' % res[0])
    elif what == 'scan':
        _, error, nets = request(port, CMD_SCAN, timeout=20, many=True)
        if error:
            sys.exit('scan failed: %s' % ERRORS.get(error, error))
        for name, rssi, secured in nets:
            print('%5s dBm  %s  %s' % (rssi, 'secured' if secured == 'YES' else 'open   ', name))
    else:
        ssid = sys.argv[3].encode()
        pw = os.environ.get('IMPROV_PASSWORD')
        if pw is None:
            pw = getpass.getpass('password for %s (empty: open network): ' % sys.argv[3])
        pw = pw.encode()
        state, error, res = request(port, CMD_WIFI, bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw, timeout=45)
        if error:
            sys.exit('failed: %s' % ERRORS.get(error, error))
        print('joined; the page: %s' % (res[0] if res else '?'))


if __name__ == '__main__':
    main()
