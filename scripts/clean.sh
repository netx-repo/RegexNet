#! /bin/bash

# Clear the build folder
function clean_build() {
    if [ -d "build" ]; then
        rm -r build
    fi
}

# Clear the output folder
function clean_run() {
    if [ -d "output" ]; then
        rm -r output
    fi
}

# Clear the log folder
function clean_log() {
    if [ -d "log" ]; then
        rm -r log
    fi
}

# Clear the model
function clean_model() {
    if [ -f "model.bin" ]; then
        rm model.bin
    fi
    if [ -f "flag.txt" ]; then
        rm flag.txt
    fi
}

case "$1" in
    all)
        clean_build
        clean_run
        clean_log
        clean_model
        ;;
    build)
        clean_build
        ;;
    run)
        clean_run
        ;;
    log)
        clean_log
        ;;
    model)
        clean_model
        ;;
    *)
        echo "Error: Unrecognized command $1"
        exit 1
esac