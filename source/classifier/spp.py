import torch
import torch.nn as nn
import torch.nn.functional as F
from math import floor, ceil


class SPPLayer(nn.Module):

    def __init__(self, num_levels, pool_type='max_pool'):
        super(SPPLayer, self).__init__()

        self.num_levels = num_levels
        self.pool_type = pool_type

    def cal_num_grids(self, level):
        count = 0
        for i in range(level):
            count += (i + 1)
        return count

    def forward(self, x):
        N, C, H = x.size()
        for i in range(self.num_levels):
            level = i + 1
            kernel_size = ceil(H / level)
            stride = ceil(H / level)
            padding = floor((kernel_size * level - H + 1) / 2)

            if self.pool_type == 'max_pool':
                tensor = (F.max_pool1d(x, kernel_size=kernel_size, stride=stride, padding=padding)).view(N, -1)
            else:
                tensor = (F.avg_pool1d(x, kernel_size=kernel_size, stride=stride, padding=padding)).view(N, -1)
            if i == 0:
                res = tensor
            else:
                res = torch.cat((res, tensor), 1)
        return res
