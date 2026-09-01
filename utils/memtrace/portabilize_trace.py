# Portabilize trace by copying binary dependencies to a local directory
# Usage: python portabilize_trace.py [trace directory, e.g. ~/drmemtrace.trace1.1234.2134.dir/]
#

from shutil import copy
from os import mkdir, remove
from os import path
import sys


if len(sys.argv) < 2:
    print('Usage: python portabilize_trace.py [trace directory, e.g. ~/drmemtrace.trace1.1234.2134.dir/]')
    exit();

traceDir = sys.argv[1]

binPath = traceDir + '/bin/'
if not path.exists(binPath):
    mkdir(binPath)

data = []
copied = set()
with open(traceDir + '/bin/modules.log', 'r') as infile :
    separator = ', '
    first = 1
    col = 99
    for line in infile:
        s = line.split(separator)
        if first:
            ss = s[0].split(' ')
            
            first = 0
            if ss[2] != 'version':
                print('Corrupt file format'+s[2])
                exit()
            else:
                #version == 5
                if ss[3] == '5':
                    col = 8
                #earlier versions
                elif ss[3] < '5':
                    col = 7
                else:
                    print('new file format, please add support')
                    exit()
                    
# Skip over but preserve lines that don't describe libraries
        if len(s) < col+1 or s[col][0] != '/':
            data.append(line);
            continue;
        libPath = s[col].strip()
        # Modify the path to the library to point to new, relative path
        libName = path.basename(libPath)
        newLibPath = path.abspath(binPath + libName)
        # DynamoRIO emits one modules.log line per ELF segment, so the same module
        # appears several times with an identical path. Copy each module only once
        # per run (but remove leftovers from earlier runs) to avoid redundant copies.
        if libPath not in copied:
            if path.exists(newLibPath):
                remove(newLibPath)
            copy(libPath, newLibPath)
            copied.add(libPath)
        s[col] = newLibPath + '\n'
        
        data.append(separator.join(s))

with open(traceDir + '/bin/modules.log', 'w') as outfile:
    for wline in data:
        outfile.write(wline)



