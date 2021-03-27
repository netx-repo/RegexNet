#! /bin/bash

DATASET_NAME=4-single-pollution
LENGTH_LIMIT=40000

for VARIABLE in {1..100}
do
./experiments/classifier/utils.sh $DATASET_NAME $LENGTH_LIMIT --batch_size=64
done