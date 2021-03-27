#! /bin/bash

# Copy dataset generator
function build_generator() {
    cp -r source/dataset build/
    echo "Copy dataset generator"
}

# Compile HAProxy and move to build file
function build_haproxy() {
    cd source/haproxy
    make TARGET=linux2628
    mkdir $WORK_DIR/build/haproxy
    cp haproxy $WORK_DIR/build/haproxy/
    cp config/* $WORK_DIR/build/haproxy/
    make clean
    cd $WORK_DIR
    echo "Compile HAProxy"
}

# Copy unit tests
function build_unit_test() {
    cp -r source/unit-test build/
    echo "Copy unit tests"
}

# Copy attackers
function build_attacker() {
    cp -r source/attacker build/
    echo "Copy attackers"
}

# Downlaod nodejs and move to build file
function build_nodejs() {
    cd third-party
    bash nodejs.sh download
    cp -r node $WORK_DIR/build/node
    bash nodejs.sh clean
    cd $WORK_DIR
    echo "Download nodejs"
}

# Copy application
function build_application() {
    cp -r source/application build/
    cd build/application
    PATH=$WORK_DIR/build/node/bin/:$PATH npm install
    cd $WORK_DIR
    echo "Copy application"
}

# Copy vulnerabilities
function build_vulnerability() {
    cp -r source/redos_vulnerabilities build/
    cd build/redos_vulnerabilities
    PATH=$WORK_DIR/build/node/bin/:$PATH npm install
}

# Save the path to the work directory
WORK_DIR=$(pwd)
echo "Work derectory: ${WORK_DIR}"

# Clean the build folder
scripts/clean.sh build

# Create the build folder
if [ -d "build" ]; then
    echo "Error: build folder already exists. Please clear it before building."
    exit
fi
mkdir build
echo "Create the build folder"

case "$1" in
    all)
        build_generator
        build_haproxy
        build_unit_test
        build_attacker
        build_nodejs
        build_application
        build_vulnerability
        ;;
    generator)
        build_generator
        ;;
    haproxy)
        build_haproxy
        ;;
    application)
        build_application
        ;;
    unit_test)
        build_unit_test
        ;;
    attacker)
        build_attacker
        ;;
    nodejs)
        build_nodejs
        ;;
    vulnerability)
        build_vulnerability
        ;;
    *)
        echo "Error: Unrecognized command $1"
        exit 1
esac