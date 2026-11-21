#!/usr/bin/env python3
# Boot kernel8.img in QEMU (raspi3b) and drive the serial console from a script.
# usage: tools/qemu-drive.py IMG|- step...
#   step: text to type (Python escapes, e.g. 'ls\r'), @SECS to wait,
#         !CMD;CMD to send QEMU monitor commands (e.g. '!sendkey a')
# env: QEMU_EXTRA (extra QEMU args, e.g. "-device usb-kbd"), SHOT=file.ppm (screendump at end)
# Prints everything the console printed. Waits for the first "$ " prompt.
import codecs, os, select, socket, subprocess, sys, tempfile, time

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
img = sys.argv[1]
mon = os.path.join(tempfile.mkdtemp(), "mon.sock")
args = ["qemu-system-aarch64", "-M", "raspi3b", "-kernel", os.path.join(root, "kernel8.img"),
        "-serial", "stdio", "-display", "none", "-monitor", "unix:%s,server,nowait" % mon]
args += os.environ.get("QEMU_EXTRA", "").split()
if img != "-":
    args += ["-drive", "file=%s,if=sd,format=raw" % img]
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""

def pump(t):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.05)
        if r:
            out += os.read(p.stdout.fileno(), 65536)

def monitor(cmds):
    m = socket.socket(socket.AF_UNIX)
    m.connect(mon)
    for c in cmds:
        m.sendall((c + "\n").encode())
        time.sleep(0.15)
    time.sleep(0.5)
    m.close()

end = time.time() + 90
while time.time() < end and not out.endswith(b"$ ") and p.poll() is None:
    pump(0.1)
for st in sys.argv[2:]:
    if st.startswith("@"):
        pump(float(st[1:]))
    elif st.startswith("!"):
        monitor(st[1:].split(";"))
        pump(0.5)
    else:
        for ch in codecs.decode(st, "unicode_escape").encode("latin1"):
            p.stdin.write(bytes([ch]))
            p.stdin.flush()
            time.sleep(0.01)
        pump(0.6)
pump(1)
if os.environ.get("SHOT"):
    monitor(["screendump %s" % os.environ["SHOT"]])
    time.sleep(1)
p.kill()
sys.stdout.buffer.write(out)
