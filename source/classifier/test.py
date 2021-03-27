import random
import string
import sys
import time
import argparse
import cProfile

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
    results['guess'] = guess
    results['positive_correct'] = positive_correct
    results['positive_total'] = positive_total
    results['negative_correct'] = negative_correct
    results['negative_total'] = negative_total
    return results

def test_one_step(model, batch):
    model.eval()
    device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
    
    model.to(device)
    model.zero_grad()
    
    category_tensor, line_tensor, length_tensor = batch
    category_tensor = category_tensor.to(device)
    line_tensor = line_tensor.to(device)
    length_tensor = length_tensor.to(device)
    
    before_infer = time.time()
    output = model(line_tensor, length_tensor)
    after_infer = time.time()
    latency = (after_infer - before_infer) * 1000.0
    # print ('%d, %f ms' % (length_tensor[0].item(), latency))

    loss = nn.NLLLoss()(output, category_tensor)
    
    results = batchAccuracy(output, category_tensor)
    results['output'] = output
    results['loss'] = loss.item()
    
    return results

def test(params):
    dataset = params['dataset']
    model = params['model']
    batch_size = params['batch_size']

    start = time.time()
    def timeSince():
        now = time.time()
        elapsed = now - start
        minute = int (elapsed / 60)
        second = int (elapsed % 60)
        return '%4dm %2ds' % (minute, second)

    results_list = []
    batch_list = dataset.getTestBatchList(batch_size)
    for iter in range(len(batch_list)):
        batch_raw = batch_list[iter]
        batch = data.polishBatch(batch_raw)

        results = test_one_step(model, batch)
        results_list.append(results)
        
        def summary(label):
            return sum([results[label] for results in results_list])
        loss = summary('loss')
        positive_correct = summary('positive_correct')
        positive_total = summary('positive_total')
        negative_correct = summary('negative_correct')
        negative_total = summary('negative_total')

        """print('%6d %2d%% (%s) Loss %.4f, Benign %4d/%4d, Malicious %4d/%4d' % \
            (iter, int(iter / len(batch_list) * 100), \
            timeSince(), \
            loss, \
            positive_correct, positive_total, \
            negative_correct, negative_total))"""

def main():
    args = parser.parse_args()
    
    dataset_folder = args.dataset_folder
    model_folder = args.model_folder
    batch_size = args.batch_size
    
    dataset_test = data.OfflineDataset(dataset_folder)
    print ('Input dataset complete')
    
    model_path = '%s/model.bin' % model_folder
    model = torch.load(model_path)
    print ('Load the model')

    params = {
        'dataset': dataset_test,
        'model': model,
        'batch_size': batch_size
    }
    cProfile.runctx("test(params)", globals(), locals())
    #test(params)

if __name__ == "__main__":
    main()