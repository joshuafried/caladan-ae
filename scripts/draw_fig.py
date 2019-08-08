import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import csv

tx = []
ty = []

qx = []
qy = []

wx = []
wy = []

with open('agt.out') as agt_out:
    plots = csv.reader(agt_out, delimiter=',')
    for row in plots:
        tx.append(float(row[0]))
        ty.append(float(row[1]))

with open('q.out') as q_out:
    plots = csv.reader(q_out, delimiter=',')
    for row in plots:
        qx.append(float(row[0]))
        qy.append(float(row[1]))

with open('cwnd.out') as cwnd_out:
    plots = csv.reader(cwnd_out, delimiter=',')
    for row in plots:
        wx.append(float(row[0]))
        wy.append(float(row[1]))

plt.subplot(3,1,1)
plt.gca().set_ylim(bottom=0, top = 2)
plt.plot(tx, ty, label='throughput')
plt.xlabel('Time (ms)')
plt.ylabel('Throughput (MRPS)')

plt.subplot(3,1,2)
plt.gca().set_ylim(bottom=0, top = 10)
plt.plot(qx, qy, label='standing queue')
plt.xlabel('Time (ms)')
plt.ylabel('Standing QLen')

plt.subplot(3,1,3)
plt.gca().set_ylim(bottom=0, top = 5)
plt.plot(wx, wy, 'o-', markersize=3, label='cwnd')
plt.xlabel('Time (ms)')
plt.ylabel('cwnd')

plt.savefig("out.png")
