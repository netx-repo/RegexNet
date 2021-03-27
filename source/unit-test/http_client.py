import requests

def main():
    url = 'http://127.0.0.1:8888/'
    r = requests.get(url, headers = {'If-None-Match': 'x' + ' ' * 10000 + 'x'})
    print (r.status_code)

if __name__ == "__main__":
    main()
