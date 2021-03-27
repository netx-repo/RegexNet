import os
from random import shuffle
import sys
import shutil
import random
import string
import requests
import argparse
import queue

URL = '10.161.159.209:8080'
HEADER_FIELDS = ['User-Agent', 'X-Server', 'X-Unique-ID']

ATTACK_THRESHOLD = 20000

parser = argparse.ArgumentParser(description='Process some parameters.')
parser.add_argument(
    '--dataset_folder',
    required=True
)
parser.add_argument(
    '--length_limit',
    type=int,
    required=True
)
parser.add_argument(
    '--num_malicious',
    type=int,
    required=True
)
parser.add_argument(
    '--num_benign',
    type=int,
    required=True
)

characters = set(string.ascii_letters + string.digits)
def filter_string(s):
    return ''.join([c for c in s if c in characters])
def import_attack_content(dir):
    attack_list = []
    for filename in os.listdir(dir):
        fullname = dir + '/' + filename
        with open(fullname) as f:
            attack = filter_string(f.read(1024)).strip()
        attack_list.append(attack)

    random.shuffle(attack_list)
    output = queue.Queue()
    for att in attack_list:
        output.put(att)
    return output
attack_contents = import_attack_content('resource/ua-parser-js')

def random_string(length):
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))

def generate_content_normal(header, length_limit):
    if header == 'User-Agent':
        return random_string(length_limit)
    elif header == 'X-Server':
        select = random.randint(0, 10)
        return 'localhost:%d' % (8000 + select)
    elif header == 'X-Unique-ID':
        return str(random.randint(0, 2147483647))
    else:
        print ('Error: Unregistered header field %s' % header)
        exit (1)

def generate_content_attack(header, length_limit):
    if header == 'User-Agent':
        return filter_string(attack_contents.get())
        # return random_string(length_limit)
    elif header == 'X-Server':
        select = random.randint(0, 1000)
        return 'localhost:%d' % (8000 + select)
    elif header == 'X-Unique-ID':
        return str(random.randint(0, 2147483647))
    else:
        print ('Error: Header field %s cannot be attacked' % header)
        exit (1)

def generate_headers_benign(length_limit):
    headers = {}
    for header in HEADER_FIELDS:
        content = generate_content_normal(header, length_limit)
        headers[header] = content
    return headers

def generate_headers_malicious(length_limit):
    headers = generate_headers_benign(length_limit)
    for header in HEADER_FIELDS:
        content = generate_content_attack(header, length_limit)
        headers[header] = content
    return headers

def create_request(headers):
    req = requests.Request('GET', URL, headers = headers)
    prepared = req.prepare()
    req_str = 'GET / HTTP/1.1\r\nHost: %s\r\n%s\r\n\r\n' % (prepared.url, '\r\n'.join('%s: %s' % (k, v) for k, v in prepared.headers.items()))
    return req_str

def generate_sample_malicious(folder, index, length_limit):
    path = '%s/%d-1.txt' % (folder, index)
    headers = generate_headers_malicious(length_limit)
    req_str = create_request(headers)

    with open(path, 'w') as f:
        f.write(req_str)

def generate_sample_benign(folder, index, length_limit):
    path = '%s/%d-0.txt' % (folder, index)
    headers = generate_headers_benign(length_limit)
    req_str = create_request(headers)

    with open(path, 'w') as f:
        f.write(req_str)

def main():
    # Parse parameters
    args = parser.parse_args()
    dataset_folder = args.dataset_folder
    length_limit = args.length_limit
    num_malicious = args.num_malicious
    num_benign = args.num_benign

    # Generate malicious samples
    index_base = 0
    for i in range(num_malicious):
        generate_sample_malicious(dataset_folder, index_base + i, length_limit)
    
    # Generate benign samples
    index_base += num_malicious
    for i in range(num_benign):
        generate_sample_benign(dataset_folder, index_base + i, length_limit)

if __name__ == "__main__":
    main()