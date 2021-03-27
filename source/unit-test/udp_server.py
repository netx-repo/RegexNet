import socket
import sys

def main():
    UDP_IP = sys.argv[1]
    UDP_PORT = int (sys.argv[2])

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))

    data_b, addr = sock.recvfrom(2048)
    data = data_b.decode()
    print (str(addr))
    print ('--------------------')
    print (data)

if __name__ == "__main__":
    print ('==========Start==========')
    main()
    print ('==========End==========')