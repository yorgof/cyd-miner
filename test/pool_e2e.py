#!/usr/bin/env python3
"""End-to-end check: fetch a job from the pool, mine one share with the
firmware's own hashing code (built for the host), submit it, expect accept.

usage: pool_e2e.py <host> <port> <btc_address> <path/to/host_test>
"""
import json
import socket
import subprocess
import sys

host, port, address, binary = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
worker = address + ".cyd-test"

sock = socket.create_connection((host, port), timeout=10)
rfile = sock.makefile("r")


def send(obj):
    sock.sendall((json.dumps(obj) + "\n").encode())


send({"id": 1, "method": "mining.subscribe", "params": ["cyd-miner-test"]})
send({"id": 2, "method": "mining.suggest_difficulty", "params": [0.001]})
send({"id": 3, "method": "mining.authorize", "params": [worker, "x"]})

extranonce1 = en2_size = difficulty = job = None
while job is None or difficulty is None or extranonce1 is None:
    msg = json.loads(rfile.readline())
    if msg.get("id") == 1:
        extranonce1, en2_size = msg["result"][1], msg["result"][2]
    elif msg.get("method") == "mining.set_difficulty":
        difficulty = msg["params"][0]
    elif msg.get("method") == "mining.notify":
        job = msg["params"]

job_id, prevhash, coinb1, coinb2, branches, version, nbits, ntime = job[:8]
print(f"job {job_id} difficulty {difficulty} extranonce1 {extranonce1} en2_size {en2_size}")

out = subprocess.run(
    [binary, "mine", str(difficulty), job_id, prevhash, coinb1, extranonce1, str(en2_size),
     coinb2, version, nbits, ntime, *branches],
    capture_output=True, text=True, check=True).stdout.split()
en2, ntime_hex, nonce, share_diff = out
print(f"found share: nonce {nonce} difficulty {share_diff}")

send({"id": 10, "method": "mining.submit", "params": [worker, job_id, en2, ntime_hex, nonce]})
while True:
    msg = json.loads(rfile.readline())
    if msg.get("id") == 10:
        print("pool reply:", msg)
        sys.exit(0 if msg.get("result") is True else 1)
