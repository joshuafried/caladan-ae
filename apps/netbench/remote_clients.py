import paramiko
import select
import time
import numpy as np

BASE_DIR = "/users/inhocho"
APP_DIR = "shenango/apps/netbench"

NUMCONN = 1000
SERVER_IP = "192.168.1.103"
SERVICE_TIME = 100

command = "sudo /users/inhocho/shenango/apps/netbench/netbench "\
"/users/inhocho/shenango/apps/netbench/client.config "\
"client 1000 192.168.1.103 100"

clients = ["hp179", "hp162", "hp038", "hp172", "hp161", "hp199", "hp164",
        "hp200", "hp188", "hp178"]
num_clients = len(clients)

sshs = []

throughputs = []
cpus = []
wsizes = []
mins = []
means = []
p50s = []
p90s = []
p99s = []
p999s = []
p9999s = []
maxs = []
wins = []
queues = []

def parse_output(response):
    lines = response.split('\n');
    for line in lines:
        if "{:d},".format(NUMCONN) in line:
            numbers = line.split(",")
            throughputs.append(float(numbers[2]))
            cpus.append(float(numbers[3]))
            wsizes.append(int(numbers[4]))
            mins.append(float(numbers[5]))
            means.append(float(numbers[6]))
            p50s.append(float(numbers[7]))
            p90s.append(float(numbers[8]))
            p99s.append(float(numbers[9]))
            p999s.append(float(numbers[10]))
            p9999s.append(float(numbers[11]))
            maxs.append(float(numbers[12]))
            wins.append(float(numbers[13]))
            queues.append(float(numbers[14]))
        
# set private key
pk = paramiko.RSAKey.from_private_key_file("{}/inhocho.pem".format(BASE_DIR));

# connect to the clients
for client in clients:
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect("{}.utah.cloudlab.us".format(client), username="inhocho", pkey=pk)
    sshs.append(ssh)

loads = range(50000, 2000001, 50000)
loads += range(2500000, 8000001, 500000)

for load in loads:
    # construct command
    command = "sudo {}/{}/netbench {}/{}/client.config client "\
            "{:d} {} {:d} {:d}"\
            .format(BASE_DIR, APP_DIR, BASE_DIR, APP_DIR, NUMCONN,
                    SERVER_IP, SERVICE_TIME, load/num_clients)
    stdouts = []
    throughputs = []
    cpus = []
    wsizes = []
    mins = []
    means = []
    p50s = []
    p90s = []
    p99s = []
    p999s = []
    p9999s = []
    maxs = []
    wins = []
    queues = []

    # issue command
    for ssh in sshs:
        stdin, stdout, stderr = ssh.exec_command(command);
        stdouts.append(stdout)
    
    # wait for output
    for stdout in stdouts:
        while not stdout.channel.exit_status_ready():
            time.sleep(1)
    
        if stdout.channel.recv_ready():
            rl, wl, xl = select.select([stdout.channel], [], [], 0.0)
            if len(rl) > 0:
                output = stdout.channel.recv(2048)
                parse_output(output)
    
    interpolation = 'lower'
    # aggregate results
    throughput = np.sum(throughputs)
    cpu = np.mean(cpus)
    wsize = np.sum(wsizes)
    mmin = np.min(mins)
    mean = np.mean(means)
    p50 = np.percentile(p50s, 50, interpolation=interpolation)
    p90 = np.percentile(p90s, 90, interpolation=interpolation)
    p99 = np.percentile(p99s, 99, interpolation=interpolation)
    p999 = np.percentile(p999s, 99.9, interpolation=interpolation)
    p9999 = np.percentile(p9999s, 99.99, interpolation=interpolation)
    mmax = np.max(maxs)
    win = np.mean(wins)
    queue = np.mean(queues)
    
    print("{:d},{:.04f},{:f},{:f},{:d},{:f},{:f},{:f},{:f},{:f},{:f},{:f},{:f},{:f},{:f}"\
            .format(NUMCONN, load, throughput, cpu, wsize, mmin, mean,
                p50, p90, p99, p999, p9999, mmax, win, queue))

# close the connection
for ssh in sshs:
    ssh.close()
