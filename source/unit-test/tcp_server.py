import socket
import sys

def main():
    TCP_IP = sys.argv[1]
    TCP_PORT = int (sys.argv[2])

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind((TCP_IP, TCP_PORT))
    sock.listen(1)

    conn, addr = sock.accept()
    data_b = conn.recv(2048)
    data = data_b.decode()
    print (str(addr))
    print ('--------------------')
    print (data)
    conn.close()

    sock.close()

if __name__ == "__main__":
    print ('==========Start==========')
    main()
    print ('==========End==========')