#! /bin/bash

DATASET_NAME=1-single-constant
LENGTH_LIMIT=1000

for VARIABLE in {1..10}
do
    ./experiments/utils.sh $DATASET_NAME $LENGTH_LIMIT --batch_size=16
done