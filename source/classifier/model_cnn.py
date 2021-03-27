import torch
import torch.nn as nn
import spp


class Model(nn.Module):
    def __init__(self, input_size, hidden_size, output_size, spp_num_level):
        super(Model, self).__init__()

        self.input_size = input_size
        self.hidden_size = hidden_size
        self.output_size = output_size

        self.embeddings = nn.Embedding(self.input_size, 32)
        self.conv1 = nn.Conv1d(32, 16, 64, stride=32)

        self.softmax = nn.LogSoftmax(dim=1)

        self.spp_num_level = spp_num_level
        self.spp = spp.SPPLayer(spp_num_level)
        self.num_grid = self.spp.cal_num_grids(spp_num_level)
        self.linear = nn.Linear(self.num_grid * 16, output_size)

    def forward(self, x, lengths):
        device = next(self.parameters()).device

        x = self.embeddings(x).transpose(1, 2)
        x = torch.tanh(self.conv1(x))
        x = self.spp(x)
        x = self.linear(x)
        x = self.softmax(x)
        return x
