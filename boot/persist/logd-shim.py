#!/usr/bin/env python3
import os, socket

path = '/dev/socket/logdw'
os.makedirs('/dev/socket', exist_ok=True)
try:
    os.unlink(path)
except OSError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
os.chmod(path, 0o666)
while True:
    try:
        s.recv(65536)
    except OSError:
        pass
