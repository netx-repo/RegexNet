import socket
import threading
import queue
import string
import time
import os
import math
import sys
import struct

PORT_DETECTOR = 9001
PORT_WARNING = 9002
BATCH_SIZE = 32
MAX_LENGTH = 100000
model_path = '/home/ubuntu/regexnet/build/model.bin'
flag_path  = '/home/ubuntu/regexnet/build/flag.txt'

import torch
import torch.nn as nn
import torch.nn.functional as F

import model_cnn
import data as data_module
import test as test_module

def classify(model, sample_list):
    batch_raw = []
    for line in sample_list:
        category_tensor = torch.tensor([0], dtype=torch.long)
        line_padding, line_tensor = data_module.lineToTensor(line)
        length = len(line_padding)
        length_tensor = torch.tensor([length], dtype=torch.long)
        batch_raw.append((category_tensor, line_tensor, length_tensor))

    batch = data_module.polishBatch(batch_raw)

    before_infer = time.time()
    results = test_module.test_one_step(model, batch)
    after_infer = time.time()

    guess = results['guess']
    _, line_tensor, _ = batch
    return guess, line_tensor, after_infer - before_infer

task_q = queue.Queue()
warning_q = queue.Queue()

def http_get_unique_id(data):
    begin = data.find('X-Unique-ID: ') + len('X-Unique-ID: ')
    if begin < 0:
        return -1
    end = data.find('\r', begin)
    return int(data[begin: end])

def http_get_server(data):
    begin = data.find('X-Server: ') + len('X-Server: ')
    if begin < 0:
        return -1
    end = data.find('\r', begin)
    return data[begin: end]

def send_warning(server, id):
    # print ("Time: %f" % time.time())
    # print ("Server: " + server)
    # print ("Malicious ID: " + str(id))
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(None)
    
    try:
        s.connect((server, PORT_WARNING))
        s.send(bytes(str(id), 'ascii'))
        s.close()
        print ('Malicious, %f, %d, Signal' % (time.time(), id))
    except:
        # print ('Connect error')
        print ('Malicious, %f, %d, Miss' % (time.time(), id))
        pass

def handle_request(model_path, flag_path):
    print ('Wait for training complete...')
    while not os.path.isfile(flag_path):
        task_q.get()
    print ('Start')

    count = 0
    flag_mdate = None
    while True:
        if not task_q.empty():
            if flag_mdate != os.stat(flag_path)[8]:
                model = torch.load(model_path)
                flag_mdate = os.stat(flag_path)[8]
                print ('Input model')

            sample_list = []
            for i in range(BATCH_SIZE):
                if not task_q.empty():
                    line = task_q.get()
                    sample_list.append(line)
                else:
                    break

            # Classify
            guess, line_tensor, latency = classify(model, sample_list)
            for i in range(len(guess)):
                if guess[i] == 1:
                    warning_q.put(line_tensor[i])
                    # print ('Latency: %f' % latency)
            count = count + len(guess)
            # print (count)

def handle_warning():
    while True:
        line = data_module.tensorToLine(warning_q.get(block=True).cpu())
        # print ('suspicious length: %d' % len(line))
        id = http_get_unique_id(line)
        server = http_get_server(line)
        send_warning(server, id)


def main():
    # Warmup CUDA
    torch.randn(1024, device='cuda').sum()

    global model_path
    global flag_path
    worker = threading.Thread(target=handle_request, args=(model_path, flag_path))
    worker.start()

    worker_warning = threading.Thread(target=handle_warning)
    worker_warning.start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('0.0.0.0', PORT_DETECTOR))
    sock.listen(1)

    seqno = -1
    conn, addr = sock.accept()
    print (addr)
    while True:
        SIZE_INTEGER = 4
        length_b = conn.recv(SIZE_INTEGER, socket.MSG_WAITALL)
        length = struct.unpack(">i", length_b)[0]
        data_b = conn.recv(length, socket.MSG_WAITALL)
        data = data_b.decode()

        #id = http_get_unique_id(data)
        # if length > 1000:
        #     print ('Receive potential malicious request: %f, %d\n' % (time.time(), length))

        #if id <= seqno:
        #    continue
        #seqno = id
        task_q.put(data)

if __name__ == "__main__":
    main()
