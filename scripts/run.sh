#! /bin/bash

# Generate dataset
function run_dataset() {
    if [ ! -d "output/dataset" ]; then
        mkdir output/dataset
    fi
    echo "Create the output/dataset folder"

    if [ ! -d "output/dataset/$1" ]; then
        mkdir output/dataset/$1
    fi
    echo "Create the output/dataset/$1 folder"

    if [ ! -d "output/dataset/$1/$2" ]; then
        mkdir output/dataset/$1/$2
    fi
    echo "Create the output/dataset/$1/$2 folder"

    if [ ! -d "output/dataset/$1/$2" ]; then
        mkdir output/dataset/$1/$2
    fi
    echo "Create the output/dataset/$1/$2 folder"

    python3 build/dataset/$1.py ${@:3}
    echo "Generate dataset"
}

function run_train() {
    if [ ! -d "output/dataset/$1/train" ]; then
        echo "Dataset folder output/dataset/$1/train does not exist"
        exit
    fi

    if [ ! -d "output/model" ]; then
        mkdir output/model
    fi
    echo "Create the output/model folder"

    if [ ! -d "output/model/$2" ]; then
        mkdir output/model/$2
    fi
    echo "Create the output/model/$2 folder"

    python3 build/classifier/train.py \
        "--dataset_folder=output/dataset/$1/train" \
        "--model_folder=output/model/$2" \
        ${@:3}
    echo "Train the model complete"
}

function run_test() {
    if [ ! -d "output/dataset/$1/test" ]; then
        echo "Dataset folder output/dataset/$1/test does not exist"
        exit
    fi

    if [ ! -d "output/model/$2" ]; then
        echo "Model folder output/model/$2 does not exist"
        exit
    fi

    python3 build/classifier/test.py \
        "--dataset_folder=output/dataset/$1/test" \
        "--model_folder=output/model/$2" \
        ${@:3}
    echo "Train the model complete"
}


function run_train_and_test() {
    if [ ! -d "output/dataset/$1" ]; then
        echo "Dataset folder output/dataset/$1 does not exist"
        exit
    fi

    if [ ! -d "output/dataset/$2" ]; then
        echo "Dataset folder output/dataset/$2 does not exist"
        exit
    fi

    if [ ! -d "output/model" ]; then
        mkdir -p output/model
    fi
    echo "Create the output/model folder"

    if [ ! -d "output/model/$1" ]; then
        mkdir -p output/model/$1
    fi
    echo "Create the output/model/$1 folder"

    if [ ! -d "log" ]; then
        mkdir log
    fi
    echo "Create the/log folder"

    if [ ! -d "log/$1" ]; then
        mkdir -p log/$1
    fi
    echo "Create the log/$1 folder"

    python3 build/classifier/train_and_test.py \
        "--dataset_train_folder=output/dataset/$1" \
        "--dataset_test_folder=output/dataset/$2" \
        "--model_folder=output/model/$1" \
        "--log_folder=log/$1" \
        ${@:3}
    echo "Train and test the model complete"
}

function run_unit_test_udp_client() {
    python3 build/unit-test/udp_client.py $@
}

function run_unit_test_udp_server() {
    python3 build/unit-test/udp_server.py $@
}

function run_unit_test_tcp_client() {
    python3 build/unit-test/tcp_client.py $@
}

function run_unit_test_tcp_server() {
    python3 build/unit-test/tcp_server.py $@
}

function run_application() {
    cd build/application
    PATH=$WORK_DIR/build/node/bin/:$PATH NODE_ENV=production PORT=8099 node app.js
}

function run_backend() {
    cd build/http_proxy
    PATH=$WORK_DIR/build/node/bin/:$PATH ./http_proxy
}

function run_haproxy() {
    cd build/haproxy
    ./haproxy -f my_proxy.cfg
}

function run_collector() {
    cd build/data_collector
    ./data_collector
}

function run_data_manager() {
    if [ ! -d "output/model" ]; then
        mkdir output/model
    fi
    
    cd build/data_manager
    python data_manager.py
}

function run_detector() {
    if [ ! -d "output/model" ]; then
        mkdir output/model
    fi
    
    cd build/detector
    python detector.py
}

function run_attacker() {
    cd build/attacker
    python attacker_${1}.py ${@:2}
}

function run_redis_insert() {
    cd build/attacker
    PATH=$WORK_DIR/build/node/bin/:$PATH npm install redis
    PATH=$WORK_DIR/build/node/bin/:$PATH node redis-insert.js
}

function run_vulnerability() {
    cd build/redos_vulnerabilities
    PATH=$WORK_DIR/build/node/bin/:$PATH node test-${1}.js
}

# Main function
function create_output_and_log_folder() {
    if [ ! -d "output" ]; then
        mkdir output
    fi
    echo "Create the output folder"

    if [ ! -d "log" ]; then
        mkdir log
    fi
    echo "Create the log folder"
}
function main() {
    create_output_and_log_folder
    case "$1" in
        dataset)
            run_dataset ${@:2}
            ;;
        train)
            run_train ${@:2}
            ;;
        test)
            run_test ${@:2}
            ;;
        train_and_test)
            run_train_and_test ${@:2}
            ;;
        unit_test_udp_client)
            run_unit_test_udp_client ${@:2}
            ;;
        unit_test_udp_server)
            run_unit_test_udp_server ${@:2}
            ;;
        unit_test_tcp_client)
            run_unit_test_tcp_client ${@:2}
            ;;
        unit_test_tcp_server)
            run_unit_test_tcp_server ${@:2}
            ;;
        application)
            run_application ${@:2}
            ;;
        backend)
            run_backend ${@:2}
            ;;
        haproxy)
            run_haproxy ${@:2}
            ;;
        collector)
            run_collector ${@:2}
            ;;
        data_manager)
            run_data_manager ${@:2}
            ;;
        detector)
            run_detector ${@:2}
            ;;
        attacker)
            run_attacker ${@:2}
            ;;
        redis-insert)
            run_redis_insert ${@:2}
            ;;
        vulnerability)
            run_vulnerability ${@:2}
            ;;
        *)
            echo "Error: Unrecognized command $1"
            exit 1
    esac
}

# Entry point
WORK_DIR=$(pwd)
main $@