import torch

import random
import string
import os
import sys
import math
import time

all_letters = string.printable + '\0'
n_letters = len(all_letters)
all_categories = ['good', 'bad']
n_categories = len(all_categories)

def letterToIndex(letter):
    return all_letters.find(letter)

def lineToTensor(line):
    length = int(math.ceil(len(line) / 32)) * 32
    line = line + '\0' * (length - len(line))
    return line, torch.tensor([[letterToIndex(letter) for letter in line]], dtype=torch.long)

def tensorToLine(tensor):
    narray = tensor.numpy().reshape([-1])
    line = ''.join([all_letters[i] for i in narray])
    return line.rstrip('\0')

class Dataset(object):
    def __init__(self, records, classes):
        self.records = records
        self.classes = classes

    def insertItem(self, category, line):
        category_tensor = torch.tensor([category], dtype=torch.long)
        if category not in self.classes:
            self.classes[category] = []

        line_pading, line_tensor = lineToTensor(line)
        length = len(line_pading)
        length_tensor = torch.tensor([length], dtype=torch.long)
        
        self.records.append((category_tensor, line_tensor, length_tensor))
        self.classes[category].append(len(self.records) - 1)

    def getRandomBatch(self, batch_size):
        sample_ids = [random.randint(0, len(self.records) - 1) for _ in range(batch_size)]
        return [self.records[id] for id in sample_ids]

    def getBalancedBatch(self, batch_size):
        avg_num = batch_size // len(self.classes)
        sample_ids = []
        for class_id in self.classes:
            samples = self.classes[class_id]
            for _ in range(avg_num):
                sample_ids.append(samples[random.randint(0, len(samples) - 1)])
        return [self.records[id] for id in sample_ids]

    def getTestBatchList(self, batch_size):
        def chunks(l, n):
            for i in range(0, len(l), n):
                yield l[i: min(len(l), i + n)]
        return list(chunks(self.records, batch_size))

class OfflineDataset(Dataset):
    def __init__(self, folder):
        super().__init__(
            records=[], 
            classes={}
        )

        for filename in os.listdir(folder):
            category = int(filename.split('.')[0].split('-')[1])
            with open(folder + '/' + filename) as f:
                line = f.read()
            self.insertItem(category, line)

class OnlineDataset(Dataset):
    def __init__(self, benign, malicious):
        super().__init__(
            records=[], 
            classes={}
        )

        for line in benign:
            self.insertItem(0, line)
        for line in malicious:
            self.insertItem(1, line)

def padding(line_tensor_1_n, length):
    if line_tensor_1_n.size()[1] == length:
        return line_tensor_1_n
    zeros = torch.zeros((1, length - line_tensor_1_n.size()[1]), dtype=torch.long)
    return torch.cat([line_tensor_1_n, zeros], dim=1)

def polishBatch(sample_list):
    sample_list.sort(key=lambda tup: tup[2].item(), reverse=True)
    max_length = sample_list[0][2].item()

    category_tensors = []
    line_tensors = []
    length_tensors = []
    for i in range(len(sample_list)):
        category_tensors.append(sample_list[i][0])
        line_tensors.append(padding(sample_list[i][1], max_length))
        length_tensors.append(sample_list[i][2])
    return torch.cat(category_tensors), torch.cat(line_tensors), torch.cat(length_tensors)