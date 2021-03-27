#! /bin/bash

function download_nodejs() {
    wget https://nodejs.org/download/release/v8.10.0/node-v8.10.0-linux-x64.tar.gz
    tar -xzvf node-v8.10.0-linux-x64.tar.gz
    mv node-v8.10.0-linux-x64 node
}

function clean_nodejs() {
    rm -rf node node-*
}

case "$1" in
    download)
        download_nodejs
        ;;
    clean)
        clean_nodejs
        ;;
    *)
        echo "Error: Unrecognized command $1"
        exit 1
esac