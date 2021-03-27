#! /bin/bash

function generate_dataset() {
    DATASET_NAME=$1
    DATSET_NAME_DETAIL=$2
    LENGTH_LIMIT=$3
    NUM_MALICIOUS=$4
    NUM_BENIGN=$5
    
    # Generate the dataset
    ./scripts/run.sh dataset $DATASET_NAME $DATSET_NAME_DETAIL \
        --dataset_folder=output/dataset/$DATASET_NAME/$DATSET_NAME_DETAIL \
        --num_malicious=$NUM_MALICIOUS \
        --num_benign=$NUM_BENIGN \
        --length_limit=$LENGTH_LIMIT
}

TEST_STEPS='0 1 2 3 4 5 6 7 8 9 10'
function experiment() {
    DATASET_NAME=$1
    LENGTH_LIMIT=$2
    DATSET_NAME_DETAIL=$3
    

    # Train and test
    ./scripts/run.sh train_and_test $DATASET_NAME/$DATSET_NAME_DETAIL $DATASET_NAME/test --test_steps $TEST_STEPS ${@:4}
}

./scripts/clean.sh build
./scripts/clean.sh run
./scripts/build.sh all


DATASET_NAME=$1
LENGTH_LIMIT=$2
NUM_MALICIOUS_LIST='32'
NUM_BENIGN_LIST='1024'

generate_dataset $DATASET_NAME test $LENGTH_LIMIT 100 100

for NUM_MALICIOUS in $NUM_MALICIOUS_LIST
do
    for NUM_BENIGN in $NUM_BENIGN_LIST
    do
        generate_dataset $DATASET_NAME $NUM_MALICIOUS-$NUM_BENIGN $LENGTH_LIMIT $NUM_MALICIOUS $NUM_BENIGN
        experiment $DATASET_NAME $LENGTH_LIMIT $NUM_MALICIOUS-$NUM_BENIGN ${@:5}
    done
done