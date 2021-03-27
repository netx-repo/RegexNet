#! /bin/bash

DATASET_NAME=5-single-rescue
LENGTH_LIMIT=128

for VARIABLE in {1..100}
do
./experiments/classifier/utils.sh $DATASET_NAME $LENGTH_LIMIT --batch_size=64
done