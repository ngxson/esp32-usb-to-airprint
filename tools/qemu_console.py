"""QEMU's console for tools/qemu_selftest.sh: QEMU waits (chardev socket,
wait=on) until this connects, so nothing is lost. Everything the board writes
is appended to LOG, and passed on to the clients connected to RELAY_PORT,
whose bytes go to the board (tools/improv.py tcp:localhost:RELAY_PORT).
Exits when QEMU closes the console.

  python3 tools/qemu_console.py QEMU_PORT LOG RELAY_PORT
"""
import select
import socket
import sys
import time

qemu_port, log_path, relay_port = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
for _ in range(100):
    try:
        q = socket.create_connection(('127.0.0.1', qemu_port))
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit('qemu_console: no QEMU on port %d' % qemu_port)
log = open(log_path, 'ab', buffering=0)
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', relay_port))
srv.listen()
clients = []
while True:
    ready, _, _ = select.select([q, srv] + clients, [], [])
    for s in ready:
        if s is q:
            data = q.recv(4096)
            if not data:
                sys.exit(0)
            log.write(data)
            for c in clients[:]:
                try:
                    c.sendall(data)
                except OSError:
                    clients.remove(c)
        elif s is srv:
            clients.append(srv.accept()[0])
        else:
            data = s.recv(4096)
            if data:
                q.sendall(data)
            else:
                clients.remove(s)
                s.close()
