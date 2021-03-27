import socket
import sys

def main():
    TCP_IP = sys.argv[1]
    TCP_PORT = int(sys.argv[2])
    MESSAGE = sys.argv[3]

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((TCP_IP, TCP_PORT))
    s.send(MESSAGE.encode())
    s.close()

if __name__ == "__main__":
    print ('==========Start==========')
    main()
    print ('==========End==========')