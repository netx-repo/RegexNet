import threading
import socket
import random
import time

import socket
import threading
import queue
import string
import time
import os
import math
import sys

ADVERSARY_ADDR = '172.31.70.125'

PORT_DETECTOR = 9001
PORT_WARNING = 9002
MAX_LENGTH = 100000
BATCH_SIZE = 64

PORT_REPORT = 9004
PORT_QUERY = 9005

batch_size = 4
model_path = '/home/ubuntu/regexnet/build/model.bin'
flag_path  = '/home/ubuntu/regexnet/build/flag.txt'
train_data_folder = '/home/ubuntu/regexnet/build/train_data/'

import torch
import torch.nn as nn
import torch.nn.functional as F
print ('Torch ' + torch.__version__)

import model_cnn
import data as data_module
import train as train_module
import test as test_module

gpu = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
cpu = torch.device("cpu")
print ("GPU availability: %s" % str(torch.cuda.is_available()))

def train_batch(model, optimizer, benign, malicious):
    lock.acquire()
    dataset = data_module.OnlineDataset(benign, malicious)
    batch = data_module.polishBatch(dataset.getBalancedBatch(batch_size))
    lock.release()

    results = train_module.train_one_step(model, batch, optimizer)

    print ('\tTraining loss: %f' % results['loss'])

def test_batch(model, benign, malicious):
    batch_raw = []
    lock.acquire()
    for i in range(min(8, len(malicious))):
        category = 1
        line = malicious[-(i + 1)]
        batch_raw.append((category, line))
    
    for i in range(min(16 - min(8, len(malicious)), len(benign))):    
        category = 0
        line = benign[-(i + 1)]
        batch_raw.append((category, line))
    lock.release()
    print ([len(line) for category, line in batch_raw])

    batch = []
    for category, line in batch_raw:
        category_tensor = torch.tensor([category], dtype=torch.long)
        line_pading, line_tensor = data_module.lineToTensor(line)
        length = len(line_pading)
        length_tensor = torch.tensor([length], dtype=torch.long)
        batch.append((category_tensor, line_tensor, length_tensor))
    
    batch = data_module.polishBatch(batch)
    results = test_module.test_one_step(model, batch)

    positive_correct = results['positive_correct']
    positive_total = results['positive_total']
    negative_correct = results['negative_correct']
    negative_total = results['negative_total']

    n_correct = results['positive_correct'] + results['negative_correct']
    n_total = results['positive_total'] + results['negative_total']
    accuracy = n_correct / (n_total + 0.0)
    print ('\tpositive_correct: %f, positive_total: %f, negative_correct: %f, negative_total: %f' % (results['positive_correct'],results['positive_total'],results['negative_correct'],results['negative_total']))
    print ('\tTest accuracy: %f' % accuracy)
    return accuracy

lock = threading.Lock()
data_benign = []
data_malicious = []

def is_strange(count, sum, sq_sum, value):
    average = sum / count
    variant = (sq_sum - 2 * average * sum + count * average * average) / count
    stdev = math.sqrt(variant)
    if stdev < 1.0:
        stdev = 1.0

    if value < average - 3 * stdev or value > average + 3 * stdev:
        return True
    else:
        return False

def handle_report():
    global lock
    global data_benign
    global data_malicious

    if not os.path.exists(train_data_folder):
        os.mkdir(train_data_folder)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('0.0.0.0', PORT_REPORT))
    sock.listen(16)

    total = 0

    length_sum = 0
    length_sq_sum = 0

    latency_sum = 0
    latency_sq_sum = 0

    cnt = 0

    while True:
        conn, _ = sock.accept()

        metadata = conn.recv(128)
        metadata = metadata.decode('UTF-8')
        id = int(metadata.split(';')[0])
        latency = int(metadata.split(';')[1])

        data = b''
        while len(data) < 1 or data[-1] != 0xa:
            data = data + conn.recv(MAX_LENGTH)
        conn.close()

        if len(data) < 1000:
            lock.acquire()

            file_name = train_data_folder + str(cnt) + "-0.txt"
            with open(file_name,"a+") as f:
                f.write(str(data.decode()))

            data_benign.append(data.decode())
            # if len(data_benign) > 2000:
            #     data_benign = data_benign[-1000:]
            lock.release()
        else:
            lock.acquire()
            print ('Receive malicious sample %d: %d, %d' % (id, len(data), latency))
            data_malicious.append(data.decode())
            lock.release()

def send_notification(data):
    print ('Notify')
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((ADVERSARY_ADDR, PORT_MODEL_READY))
    length = len(data)
    length_str = '%8d' % length
    legnth_str_b = length_str.encode()
    s.send(legnth_str_b)
    data_b = data.encode()
    s.send(data_b)
    s.close()

def handle_training():
    global lock
    global data_benign
    global data_malicious

    #define model
    if os.path.isfile(model_path):
        model = torch.load(model_path)
    else:
        model = model_cnn.Model(data_module.n_letters, 64, data_module.n_categories, 3)
    optimizer = torch.optim.Adam(model.parameters(), lr=0.01, weight_decay=0.0005)

    last_malicious_count = 0
    last_benign_count = 0
    while True:
        # if len(data_benign) > 0 and len(data_malicious) > last_malicious_count:
        #     last_malicious_count = len(data_malicious)
        if len(data_benign) > last_benign_count or len(data_malicious) > last_malicious_count:
            last_malicious_count = len(data_malicious)
            last_benign_count = len(data_benign)
            print("data_benign:"+str(len(data_benign)))
            print("data_malicious:"+str(len(data_malicious)))

            flag = False
            while test_batch(model, data_benign, data_malicious) < 0.99:
                flag = True
                train_batch(model, optimizer, data_benign, data_malicious)
                # train_batch(model, optimizer, data_benign, data_malicious)
                # train_batch(model, optimizer, data_benign, data_malicious)

            if flag or not os.path.isfile(flag_path):
                model.to(cpu)
                torch.save(model, model_path)
                os.system('echo finish > %s' % flag_path)
                print ("Save model")
                # transferring model is performed by management process
        else:
            print ('Sleep ' + str(time.time()))
            time.sleep(1.0)

def handle_flush():
    while True:
        time.sleep(1)
        sys.stdout.flush()

def main():
    # Warmup CUDA
    torch.randn(1024, device='cuda').sum()

    s = threading.Thread(target=handle_report)
    t = threading.Thread(target=handle_training)
    u = threading.Thread(target=handle_flush)
    s.start()
    t.start()
    u.start()
    s.join()
    t.join()
    u.join()

if __name__ == "__main__":
    main()
