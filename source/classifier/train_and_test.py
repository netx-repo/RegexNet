import random
import string
import os
import sys
import time
import argparse

import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim

import model_cnn
import data
import train
import test

parser = argparse.ArgumentParser(description='Process some parameters.')
parser.add_argument(
    '--dataset_train_folder',
    required=True
)
parser.add_argument(
    '--dataset_test_folder',
    required=True
)
parser.add_argument(
    '--model_folder',
    required=True
)
parser.add_argument(
    '--log_folder',
    required=True
)
parser.add_argument(
    '--test_steps',
    nargs='+',
    type=int,
    required=True,
)
parser.add_argument(
    '--batch_size',
    type=int,
    default=64
)
parser.add_argument(
    '--n_iters',
    type=int,
    default=100
)
parser.add_argument(
    '--init_model_path',
    default=""
)

file_name = './task3_cnn.csv'


def train_and_test(params):
    dataset_train = params['dataset_train']
    dataset_test = params['dataset_test']
    model = params['model']
    model_folder = params['model_folder']
    log_folder = params['log_folder']
    test_steps = params['test_steps']
    batch_size = params['batch_size']
    n_iters = params['n_iters']

    start = time.time()

    def timeSince():
        now = time.time()
        elapsed = now - start
        minute = int(elapsed / 60)
        second = int(elapsed % 60)
        return '%4dm %2ds' % (minute, second)

    optimizer = optim.Adam(model.parameters(), lr=0.01, weight_decay=0.0005)

    test_batch_list = dataset_test.getTestBatchList(batch_size)
    for i in range(len(test_batch_list)):
        test_batch_list[i] = data.polishBatch(test_batch_list[i])

    train_history = []
    test_history = []
    best_accuracy = 0.0

    def summary(history, label, duration=0):
        return sum([results[label] for results in history[-duration:]])

    # print (model_folder)
    # print ('Test step, Accuracy, Loss, Benign, Malicious')
    output_list = [model_folder]

    if 0 in test_steps:
        iter = 0
        model_path = '%s/model-%d.bin' % (model_folder, 0)
        # torch.save(model, model_path)
        results_list = []
        for batch in test_batch_list:
            results = test.test_one_step(model, batch)
            results_list.append(results)

        loss = summary(results_list, 'loss')
        positive_correct = summary(results_list, 'positive_correct')
        positive_total = summary(results_list, 'positive_total')
        negative_correct = summary(results_list, 'negative_correct')
        negative_total = summary(results_list, 'negative_total')
        accuracy = (positive_correct + negative_correct) / (positive_total + negative_total)

        # print('%d, %.4f, %.4f, %4d/%4d, %4d/%4d' % \
        #         (iter, \
        #         accuracy, \
        #         loss, \
        #         positive_correct, positive_total, \
        #         negative_correct, negative_total))
        output_list.append(str(accuracy))

        # with open(file_name, 'a') as f:
        #     f.write(str(iter) + ',' + str(accuracy) + '\n')

    for iter in range(1, n_iters + 1):
        batch_raw = dataset_train.getBalancedBatch(batch_size)
        batch = data.polishBatch(batch_raw)

        results = train.train_one_step(model, batch, optimizer)
        train_history.append(results)

        if iter in test_steps:
            model_path = '%s/model-%d.bin' % (model_folder, iter)
            # torch.save(model, model_path)

            results_list = []
            for batch in test_batch_list:
                results = test.test_one_step(model, batch)
                results_list.append(results)

            loss = summary(results_list, 'loss')
            positive_correct = summary(results_list, 'positive_correct')
            positive_total = summary(results_list, 'positive_total')
            negative_correct = summary(results_list, 'negative_correct')
            negative_total = summary(results_list, 'negative_total')
            accuracy = (positive_correct + negative_correct) / (positive_total + negative_total)

            # print('%d, %.4f, %.4f, %4d/%4d, %4d/%4d' % \
            #       (iter, \
            #        accuracy, \
            #        loss, \
            #        positive_correct, positive_total, \
            #        negative_correct, negative_total))
            output_list.append(str(accuracy))

            # with open(file_name, 'a') as f:
            #     f.write(str(iter) + ',' + str(accuracy) + '\n')

            test_history.append({
                'time': time.time(),
                'step': iter,
                'loss': loss,
                'positive_correct': positive_correct,
                'positive_total': positive_total,
                'negative_correct': negative_correct,
                'negative_total': negative_total
            })
    # print ('\n\n')

    # log_index = len(os.listdir(log_folder))
    # log_name = '%s/log-%d.txt' % (log_folder, log_index)
    # with open(log_name, 'w') as f:
    #     for results in test_history:
    #         results_str = ','.join(map(lambda x: '='.join(map(str, x)), results.items()))
    #         f.write(results_str)
    #         f.write('\n')
    print (', '.join(output_list))


def main():
    args = parser.parse_args()

    dataset_train_folder = args.dataset_train_folder
    dataset_test_folder = args.dataset_test_folder
    model_folder = args.model_folder
    log_folder = args.log_folder
    test_steps = set(args.test_steps)
    # test_step = args.test_steps
    batch_size = args.batch_size
    n_iters = args.n_iters
    init_model_path = args.init_model_path
    # test_steps = [i for i in range(0, n_iters+1, test_step)]

    dataset_train = data.OfflineDataset(dataset_train_folder)
    # print('Input dataset_train complete')
    dataset_test = data.OfflineDataset(dataset_test_folder)
    # print('Input dataset_test complete')

    if init_model_path == "":
        n_letters = data.n_letters
        n_categories = data.n_categories
        n_hidden = 32
        n_spp_num_level = 3
        model = model_cnn.Model(n_letters, n_hidden, n_categories, n_spp_num_level)
        # print('Create the model')
    else:
        model = torch.load(init_model_path)
        # print('Load the model')

    with open(file_name, 'a') as f:
        f.write(dataset_train_folder + '\n')

    params = {
        'dataset_train': dataset_train,
        'dataset_test': dataset_test,
        'model': model,
        'model_folder': model_folder,
        'log_folder': log_folder,
        'test_steps': test_steps,
        'batch_size': batch_size,
        'n_iters': n_iters,
    }
    train_and_test(params)


if __name__ == "__main__":
    main()
