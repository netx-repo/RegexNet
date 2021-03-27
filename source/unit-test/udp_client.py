import socket
import sys

def main():
    UDP_IP = sys.argv[1]
    UDP_PORT = int(sys.argv[2])
    MESSAGE = sys.argv[3]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(MESSAGE.encode(), (UDP_IP, UDP_PORT))

if __name__ == "__main__":
    print ('==========Start==========')
    main()
    print ('==========End==========')