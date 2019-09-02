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

lx = []
ly = []

with open('agt.out') as agt_out:
    plots = csv.reader(agt_out, delimiter=',')
    for row in plots:
        tx.append(float(row[0]))
        ty.append(float(row[1])*1000.0)

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

with open('p99.out') as lat_out:
    plots = csv.reader(lat_out, delimiter=',')
    for row in plots:
        lx.append(float(row[0]))
        ly.append(float(row[1]))

plt.gcf().set_size_inches(10, 12)

plt.subplot(4,1,1)
plt.xticks(np.arange(0.0, 21.0, 2.0))
plt.gca().set_ylim(bottom=0, top = 1500)
plt.plot(tx, ty, label='throughput')
plt.xlabel('Time (s)')
plt.ylabel('Throughput (kRPS)')

plt.subplot(4,1,2)
plt.xticks(np.arange(0.0, 21.0, 2.0))
plt.gca().set_ylim(bottom=0, top = 2000)
plt.plot(qx, qy, label='queueing delay')
plt.xlabel('Time (s)')
plt.ylabel('Queueing delay')

plt.subplot(4,1,3)
plt.xticks(np.arange(0.0, 21.0, 2.0))
plt.gca().set_ylim(bottom=0, top = 10)
plt.plot(wx, wy, 'o-', markersize=3, label='cwnd')
plt.xlabel('Time (s)')
plt.ylabel('cwnd')

plt.subplot(4,1,4)
plt.xticks(np.arange(0.0, 21.0, 2.0))
plt.gca().set_ylim(bottom=0, top = 4000)
plt.plot(lx,ly, label='latency')
plt.xlabel('Time (s)')
plt.ylabel('99p Latency')

plt.savefig("out.png")
