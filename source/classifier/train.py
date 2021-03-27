import random
import string
import sys
import time
import argparse

import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim

import model_cnn
import data

parser = argparse.ArgumentParser(description='Process some parameters.')
parser.add_argument(
    '--dataset_folder',
    required=True
)
parser.add_argument(
    '--model_folder',
    required=True
)
parser.add_argument(
    '--batch_size',
    type=int,
    default=32
)
parser.add_argument(
    '--n_iters',
    type=int,
    default=1000
)
parser.add_argument(
    '--print_every',
    type=int,
    default=10
)

def batchAccuracy(output_tensor, category_tensor):
    positive_correct = 0
    positive_total = 0
    negative_correct = 0
    negative_total = 0

    guess = output_tensor.max(dim=1)[1]
    for i in range(len(category_tensor)):
        if (category_tensor[i] == 0):
            positive_total = positive_total + 1
            if (guess[i] == 0):
                positive_correct = positive_correct + 1
        else:
            negative_total = negative_total + 1
            if (guess[i] == 1):
                negative_correct = negative_correct + 1

    results = {}
    results['positive_correct'] = positive_correct
    results['positive_total'] = positive_total
    results['negative_correct'] = negative_correct
    results['negative_total'] = negative_total
    return results

def train_one_step(model, batch, optimizer):
    model.train()
    device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
    
    model.to(device)
    model.zero_grad()
    
    category_tensor, line_tensor, length_tensor = batch
    category_tensor = category_tensor.to(device)
    line_tensor = line_tensor.to(device)
    length_tensor = length_tensor.to(device)
    
    output = model(line_tensor, length_tensor)
    loss = nn.NLLLoss()(output, category_tensor)
    if not torch.isnan(loss):
        loss.backward()
        optimizer.step()
    
    results = batchAccuracy(output, category_tensor)
    results['output'] = output
    results['loss'] = loss.item()
    
    return results

def train(params):
    dataset = params['dataset']
    model = params['model']
    batch_size = params['batch_size']
    n_iters = params['n_iters']
    print_every = params['print_every']

    start = time.time()
    def timeSince():
        now = time.time()
        elapsed = now - start
        minute = int (elapsed / 60)
        second = int (elapsed % 60)
        return '%4dm %2ds' % (minute, second)

    optimizer = optim.Adam(model.parameters(), lr=0.01, weight_decay=0.0005)
    results_history = []
    for iter in range(1, n_iters + 1):
        batch_raw = dataset.getBalancedBatch(batch_size)
        batch = data.polishBatch(batch_raw)

        results = train_one_step(model, batch, optimizer)
        results_history.append(results)
        
        if iter % print_every == 0:
            def summary(label, duration):
                return sum([results[label] for results in results_history[-duration:]])
            loss = summary('loss', print_every)
            positive_correct = summary('positive_correct', print_every)
            positive_total = summary('positive_total', print_every)
            negative_correct = summary('negative_correct', print_every)
            negative_total = summary('negative_total', print_every)

            print('%6d %2d%% (%s) Loss %.4f, Benign %4d/%4d, Malicious %4d/%4d' % \
                (iter, int(iter / n_iters * 100), \
                timeSince(), \
                loss, \
                positive_correct, positive_total, \
                negative_correct, negative_total))

def main():
    args = parser.parse_args()

    dataset_folder = args.dataset_folder
    model_folder = args.model_folder
    batch_size = args.batch_size
    n_iters = args.n_iters
    print_every = args.print_every
    
    dataset_train = data.OfflineDataset(dataset_folder)
    print ('Input dataset complete')
    
    n_letters = data.n_letters
    n_categories = data.n_categories
    n_hidden = 32
    n_spp_num_level = 3
    model = model_cnn.Model(n_letters, n_hidden, n_categories, n_spp_num_level)
    print ('Create the model')

    params = {
        'dataset': dataset_train,
        'model': model,
        'batch_size': batch_size,
        'n_iters': n_iters,
        'print_every': print_every,
    }
    train(params)

    model_path = '%s/model.bin' % model_folder
    torch.save(model, model_path)

if __name__ == "__main__":
    main()