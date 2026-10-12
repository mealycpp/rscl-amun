#!/usr/bin/env python3
"""Independent GPIO witness (NOT part of F'): reads the four neuron comparators directly from
/dev/gpiomem, read-only, and logs every change with a timestamp. It never writes a register,
so it cannot disturb the FPAAs or F'. Usage:  python3 gpio_watch.py [seconds] [logfile]
POS level = comparator level that means 'membrane positive' (0 on all four chips, from calibration).
"""
import mmap, os, sys, time, struct, json

PINS = {1: 16, 2: 18, 3: 20, 4: 12}
POS = {1: 0, 2: 0, 3: 0, 4: 0}
GPLEV0 = 0x34
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
logf = sys.argv[2] if len(sys.argv) > 2 else time.strftime('watch_%Y%m%d_%H%M%S.jsonl')

fd = os.open('/dev/gpiomem', os.O_RDONLY | os.O_SYNC)
mem = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ)
def levels():
    v = struct.unpack_from('<I', mem, GPLEV0)[0]
    return {k: (v >> p) & 1 for k, p in PINS.items()}

t0 = time.time(); last = None
with open(logf, 'a') as out:
    while time.time() - t0 < dur:
        lv = levels()
        fired = {k: int(lv[k] == POS[k]) for k in PINS}
        if fired != last:
            rec = {'t': round(time.time(), 6), 'fired': fired}
            out.write(json.dumps(rec) + '\n'); out.flush()
            print(time.strftime('%H:%M:%S'), 'fired (membrane > threshold):', fired, flush=True)
            last = fired
        time.sleep(0.001)
print('log:', logf)
