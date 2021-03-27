#! /bin/bash

clear

./scripts/clean.sh build

./scripts/build.sh

TEST_STEPS='1 2 3 4 5 6 7 8 9 10 20 30 40 50 60 70 80 90 100 200 300 400 500 600 700 800 900 1000'
./scripts/run.sh train_and_test 1-single-constant/1-100 --test_steps $TEST_STEPS