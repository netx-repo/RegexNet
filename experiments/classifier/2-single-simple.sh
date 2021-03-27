#! /bin/bash

DATASET_NAME=2-single-simple
LENGTH_LIMIT=40000

# for VARIABLE in {1..10}
# do
	./experiments/classifier/utils.sh $DATASET_NAME $LENGTH_LIMIT --batch_size=16
# done