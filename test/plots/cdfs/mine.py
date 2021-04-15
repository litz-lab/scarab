import sys
from matplotlib import pyplot as plt
from matplotlib.figure import figaspect
from matplotlib import rc

rc('font',**{'size':'21','family':'serif','serif':['Palatino']})
rc('text', usetex=True)

linestyles = ['-', '--', '-.', ':']
colors=['#5BB8D7','#57A86B','#A8A857','#6E4587','#ADEBCC','#EBDCAD']

def is_number(s):
    try:
        float(s)
        return True
    except ValueError:
        return False

filename="trace-size.txt"
if len(sys.argv) > 1:
  filename = sys.argv[1]

data = [[],{}]
with open(filename, 'r') as data_file:
  is_first = True
  for line in data_file:
    if line[0] == '#':
        continue
    arr = line.strip().split()
    if is_first:
      for i in arr:
        data[0].append(i)
        data[1][i]=[]
      is_first = False
    else:
      for i in range(len(arr)):
        pushed_data=arr[i]
        if is_number(pushed_data):
          pushed_data=float(pushed_data)
        data[1][data[0][i]].append(pushed_data)

w, h = figaspect(0.4/0.9)
fig, ax = plt.subplots(figsize=(w,h))

for j in range(1,len(data[0])):
    item=data[0][j]
    ax.plot(data[1][data[0][0]], data[1][item], label=item, linestyle=linestyles[j%len(linestyles)], color=colors[j%len(colors)])

ax.set_ylim(ymin=0,ymax=100)
ax.set_xlim(xmin=0,xmax=48)
ax.set_ylabel(r'CDF (\%)')
ax.set_xlabel(r'\#-of-bits required to store the target offset')
ax.legend(ncol=3, prop={'size': 16}, columnspacing=0.25)
# ax.legend(ncol=5,prop={'size': 18},loc=9) #,columnspacing=0.25)
ax.grid(linestyle='--',zorder=0)
plt.axhline(0, color='gray')
plt.tight_layout()
fig.savefig("branch-target-offset.pdf",bbox_inches='tight',pad_inches=0.1)