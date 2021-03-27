#! /bin/bash

# Copy dataset generator
function build_generator() {
    cp -r source/dataset build/
    echo "Copy dataset generator"
}

# Copy classifier
function build_classifier() {
    cp -r source/classifier build/
    echo "Copy classifier"
}

# Compile HAProxy and move to build file
function build_haproxy() {
    cd source/haproxy-with
    make TARGET=linux2628
    mkdir $WORK_DIR/build/haproxy
    cp haproxy $WORK_DIR/build/haproxy/
    cp config/* $WORK_DIR/build/haproxy/
    make clean
    cd $WORK_DIR
    echo "Compile HAProxy"
}

# Compile data_collector
function build_collector() {
    mkdir build/data_collector
    g++ -Isource \
        -o build/data_collector/data_collector \
        source/data_collector/data_collector.cpp
}

# Copy data_manager
function build_manager() {
    cp -r source/data_manager build/
    cp -r source/classifier/* build/data_manager/
    echo "Copy data_manager"
}

# Compile http_proxy
function build_proxy() {
    mkdir build/http_proxy
    g++ -Isource \
        -std=c++11 \
        -o build/http_proxy/http_proxy \
        source/http_proxy/http_proxy.cpp
}

# Copy detector
function build_detector() {
    cp -r source/detector build/
    cp -r source/classifier/model_cnn.py build/detector/
    cp -r source/classifier/spp.py build/detector/
    cp -r source/classifier/data.py build/detector/
    cp -r source/classifier/test.py build/detector/
    echo "Copy detector"
}

# Copy adversary
function build_adversary() {
    cp -r source/adversary build/
    cp -r source/classifier/model_cnn.py build/adversary/
    cp -r source/classifier/spp.py build/adversary/
    cp -r source/classifier/data.py build/adversary/
    cp -r source/classifier/test.py build/adversary/
    echo "Copy adversary"
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
        build_classifier
        build_haproxy
        build_collector
        build_manager
        build_proxy
        build_detector
        build_unit_test
        build_attacker
        build_nodejs
        build_application
        build_vulnerability
        ;;
    generator)
        build_generator
        ;;
    classifier)
        build_classifier
        ;;
    haproxy)
        build_haproxy
        ;;
    collector)
        build_collector
        ;;
    manager)
        build_manager
        ;;
    http_proxy)
        build_proxy
        ;;
    detector)
        build_detector
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