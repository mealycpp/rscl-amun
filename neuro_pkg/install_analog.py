#!/usr/bin/env python3
"""Install the AnalogCompute component into the amun-fsw F' project (idempotent).

usage: python3 install_analog.py <project_root>   e.g. ~/amun/fprime/amun-fsw
Run from the folder that contains this script and the AnalogCompute/ folder.
"""
import os, re, shutil, sys

root = os.path.abspath(os.path.expanduser(sys.argv[1]))
here = os.path.dirname(os.path.abspath(__file__))
comp_dst = os.path.join(root, 'Amun', 'Components', 'AnalogCompute')
top = os.path.join(root, 'AmunDeployment', 'Top')

def edit(path, fn):
    s = open(path).read()
    t = fn(s)
    if t != s:
        shutil.copy(path, path + '.bak')
        open(path, 'w').write(t)
        print('edited  ', path)
    else:
        print('unchanged', path)

# 1. component sources
os.makedirs(comp_dst, exist_ok=True)
for f in ('AnalogCompute.fpp', 'AnalogCompute.hpp', 'AnalogCompute.cpp', 'CMakeLists.txt'):
    shutil.copy(os.path.join(here, 'AnalogCompute', f), os.path.join(comp_dst, f))
print('copied   ', comp_dst)

# 2. register the component
line = 'add_fprime_subdirectory("${CMAKE_CURRENT_LIST_DIR}/AnalogCompute")\n'
edit(os.path.join(root, 'Amun', 'Components', 'CMakeLists.txt'),
     lambda s: s if 'AnalogCompute' in s else s.rstrip('\n') + '\n' + line)

# 3. instance (active: own thread + queue)
inst = ('\n  # PIKA HAT analog compute manager\n'
        '  instance analog: Amun.AnalogCompute base id 0x10015000 \\\n'
        '    queue size Default.QUEUE_SIZE \\\n'
        '    stack size Default.STACK_SIZE \\\n'
        '    priority 50\n')
def add_instance(s):
    if 'Amun.AnalogCompute' in s:
        return s
    i = s.rstrip().rfind('}')
    return s[:i].rstrip('\n') + '\n' + inst + '\n}\n'
edit(os.path.join(top, 'instances.fpp'), add_instance)

# 4. topology: use the instance + 1 Hz housekeeping
def add_topo(s):
    if 'instance analog' not in s:
        s = re.sub(r'(\n(\s*)instance cmdSeq\n)', r'\1\2instance analog\n', s, count=1)
    if 'analog.run' not in s:
        used = [int(m) for m in re.findall(r'rateGroup_1Hz\.RateGroupMemberOut\[(\d+)\]', s)]
        k = max(used) + 1
        s = re.sub(r'(\n(\s*)rateGroup_1Hz\.RateGroupMemberOut\[%d\][^\n]*\n)' % max(used),
                   r'\1\2rateGroup_1Hz.RateGroupMemberOut[%d] -> analog.run\n' % k, s, count=1)
    return s
edit(os.path.join(top, 'topology.fpp'), add_topo)
print('done')
