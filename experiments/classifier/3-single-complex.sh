#! /bin/bash

DATASET_NAME=3-single-complex
LENGTH_LIMIT=40000

for VARIABLE in {1..100}
do
./experiments/classifier/utils.sh $DATASET_NAME $LENGTH_LIMIT --batch_size=32
done