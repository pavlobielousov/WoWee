#!/usr/bin/env python3
"""Run arm-vita-eabi-gdb against Vita3K's gdbstub through a pty and report what it sees at a breakpoint.

Runs inside the VitaSDK image (see gdb_at_pc.sh). usage:
  gdb_drive.py <elf> <host:port> <break-address> <ignore-count> <timeout-seconds>

Why a pty: Vita3K's stub does not report crashes, so the session breaks at the crash PC instead, and a
plain `gdb -batch` run does not behave like an interactive one. Exit 3 if gdb could not connect
(the caller retries), 4 if the breakpoint was never hit.
"""
import os, pty, select, signal, sys, time

elf, target, addr, ignore, timeout = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5])

pid, fd = pty.fork()
if pid == 0:
    os.execvp("arm-vita-eabi-gdb", ["arm-vita-eabi-gdb", "-nx", "-q", elf])

out = []


def pump(seconds, until=None):
    """Collect output for `seconds`, or until `until` appears in it; returns True if it appeared."""
    end = time.time() + seconds
    while time.time() < end:
        if until and until in "".join(out):
            return True
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            out.append(data.decode(errors="replace"))
    return bool(until) and until in "".join(out)


def send(line, wait=1.5):
    os.write(fd, (line + "\n").encode())
    pump(wait)


def finish(code):
    try:
        os.write(fd, b"quit\n")
        pump(1)
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    sys.stdout.write("".join(out))
    sys.exit(code)


pump(2)
for line in ("set pagination off", "set confirm off", "set width 0"):
    send(line, 0.3)
send("target remote " + target, 3)
if "could not connect" in "".join(out) or "Remote debugging using" not in "".join(out):
    finish(3)

send("break *" + addr)
if ignore:
    send("ignore 1 %d" % ignore)
os.write(fd, b"continue\n")
if not pump(timeout, until="hit Breakpoint"):
    out.append("\n[gdb_drive] breakpoint at %s was not hit within %ds\n" % (addr, timeout))
    finish(4)

pump(1)
for cmd in ("info threads", "bt full", "info registers", "x/4i $pc", "x/32wx $sp"):
    send(cmd, 2)
finish(0)
