import matplotlib.pyplot as plt
import sys
import numpy as np
from matplotlib import rc
from matplotlib.figure import figaspect

rc('font', **{'size': '21', 'family': 'serif', 'serif': ['Palatino']})
rc('text', usetex=True)


add_average = True
xlabel_rotation = 35


def is_number(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def add_subplot(filename, ax, ytitle, show_legend=True, logy=False):  # , loc):
    data = [[], {}] # [[column names], {col_name: [data for col]}]

    file_ytitle = ""
    with open(filename, 'r') as data_file:
        is_first = True
        is_second = False
        for line in data_file:
            if line[0] == '#': # ignore comments
                continue
            if line.strip() == "":
                is_first, is_second = is_second, is_first # swap first and second
                continue
            arr = line.strip().split()
            if is_first:
                file_ytitle += line.strip() + '\n'
            elif is_second:
                # parse header
                for i in arr:
                    data[0].append(i) # column names
                    data[1][i] = [] # data per column
                is_second = False
            else:
                for i in range(len(arr)):
                    pushed_data = arr[i]
                    if is_number(pushed_data):
                        pushed_data = float(pushed_data)
                    data[1][data[0][i]].append(pushed_data) #data[0][i] is the name for this column

    if ytitle == "":
        ytitle = file_ytitle.strip() # not overridden by command line
    if add_average:
        for i in range(len(data[0])): #for each column
            key = data[0][i]
            if i == 0:
                val = r"{\bf Avg}" #since first column is typically "Workload" or some non-statistic one, add this as a "row" name
            else:
                val = sum(data[1][key])/len(data[1][key])
                print(val)
            data[1][key].append(val)

    dim = len(data[0]) - 1
    w = 0.75
    dimw = w / dim

    hatches = ['/', '.', 'x', '\\', '+']
    colors = ['#5BB8D7', '#57A86B', '#A8A857', '#6E4587', '#ADEBCC', '#EBDCAD']
    density = 5
    for i in range(len(hatches)):
        hatches[i] = hatches[i]*density

    all_bars = []
    index = 0
    x = np.arange(len(data[1][data[0][0]])) #number of data rows
    for i in range(1, len(data[0])):
        y = data[1][data[0][i]]
        if i == 1:
            prior = []
        b = ax.bar(x + (i-1) * dimw, y, dimw,
                   label=data[0][i], zorder=3,
                   fill=False,
                   hatch=hatches[index%len(hatches)],
                   edgecolor=colors[index%len(colors)])
        index += 1
        all_bars.append(b)
        prior.extend(y)

    #ax.set_xticks(x + dimw / 2, data[1][data[0][0]])
    ax.set_xticks(x+w/2-dimw/2)
    ax.set_xticklabels(data[1][data[0][0]], rotation=xlabel_rotation)

    if dim == 2 and False:
        bar_modified = all_bars[0]
        bar_original = all_bars[1]
        for i in range(len(bar_modified)):
            # /bar_original[i].get_height())
            difference = (
                (bar_original[i].get_height() - bar_modified[i].get_height()))
            ax.text((bar_modified[i].get_x())+0.35, bar_modified[i].get_height()+0.0, ''+str(round(difference, 1))+'', ha='center',
                    va='bottom', rotation=0)  # (bar_original[i].get_height() + bar_modified[i].get_height())/2 #,fontsize=17

    ax.text(6.75, 50*1.25, "{0:.0f}".format(data[1]['twig'][7]))
    ax.text(7.5, 50*1.25, "{0:.0f}".format(data[1]['ideal'][7]))
    # ax.set_xlabel('Applications')
    if logy:
        ax.set_yscale('symlog') # only way to get log and negative values
    # ax.set_adjustable('datalim')
    ax.set_ylim(ymax=50*1.2)
    if ytitle != "":
        ax.set_ylabel(ytitle)
    if show_legend:
        # ax.legend(ncol=4)
        ax.legend(ncol=4, columnspacing=0.5, fontsize='x-small')
    #ax.legend(ncol=3, columnspacing=0.25, title=title, loc=loc)
    ax.grid(linestyle='--', zorder=0)


w, h = figaspect(0.4/0.9)
fig, axs = plt.subplots(1, 1, figsize=(w, h))
filename = 'no-prefetch'
logy=False
if len(sys.argv) > 1:
    filename = sys.argv[1]
if len(sys.argv) > 2:
    logy=True
add_subplot(filename+'.txt', axs, "", logy=logy)

plt.axhline(0, color='gray')
plt.tight_layout()
fig.savefig(filename+".pdf", bbox_inches='tight', pad_inches=0)
