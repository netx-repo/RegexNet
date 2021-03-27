import string
import os
import sys
import random
import socket
import time

# os.system('python -m pip install requests')
import requests

import torch
import torch.nn as nn
import torch.nn.functional as F
gpu = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
cpu = torch.device("cpu")

import model_rnn
import model_cnn
import data as data_module
import test as test_module

PORT_ATTACKER = 9100
PORT_MODEL_READY = 9101

class Model_Attack(model_cnn.Model):
# class Model_Attack(model_rnn.Model):
    def encode(self, x):
        x = self.embeddings(x)
        return x

    def get_vectors(self):
        _, all_letters_tensor = data_module.lineToTensor(data_module.all_letters)
        all_vectors = self.encode(all_letters_tensor)
        return all_vectors.view(-1, 32)[:100]

    def get_vector_index(self, vector):
        all_vectors = self.get_vectors()
        min_dist = 1e8
        min_index = 0
        for i in range(all_vectors.size()[0]):
            dist = torch.norm(vector - all_vectors[i])
            if dist < min_dist:
                min_dist = dist
                min_index = i
        return min_index


    def forward_after_encode(self, x_after_encode):
        x = x_after_encode.transpose(1, 2)
        # x = F.relu(self.conv1(x)).transpose(1, 2).transpose(0, 1)
        # _, x = self.gru(x)
        # x = x[0]
        # x = self.h2o(x)
        # x = self.softmax(x)

        x = torch.tanh(self.conv1(x))
        x = self.spp(x)
        x = self.linear(x)
        x = self.softmax(x)

        return x

def get_index(data, start, msg):
    length = data.size()[0]
    for i in range(start, length):
        flag = True
        for j in range(len(msg)):
            if i + j >= length or data[i + j] != data_module.all_letters.find(msg[j]):
                flag = False
                break
        if flag:
            return i
    return -1

def get_mask(data, header_name):
    mask = []
    for i in range(data.size()[0]):
        begin_left = get_index(data[i], 0, '%s: ' % header_name) + len('%s: ' % header_name)
        begin_right = begin_left
        while data[i][begin_right + 1] != data_module.all_letters.find(' '):
            begin_right = begin_right + 1

        end_right = get_index(data[i], begin_left, '\n') - 1
        end_left = end_right
        while data[i][end_left - 1] != data_module.all_letters.find(' '):
            end_left = end_left - 1
        
        mask_begin = [j for j in range(begin_left, begin_right + 1)]
        mask_end = [j for j in range(end_left, end_right + 1)]
        mask.append(mask_begin + mask_end)
    return mask

def get_attack_content(http_request_str):
    header_name = 'if-none-match'
    
    begin_left = http_request_str.find('%s: ' % header_name, 0) + len('%s: ' % header_name)
    end_right = http_request_str.find('\n', begin_left) - 1
    
    return http_request_str[begin_left: end_right]

def fgsm(model, perturbed_data_encoded, mask, target):
    model.eval()
    
    perturbed_data_encoded.requires_grad = True
    output = model.forward_after_encode(perturbed_data_encoded)
    loss = nn.NLLLoss()(output, target)
    
    if perturbed_data_encoded.grad is not None:
        perturbed_data_encoded.grad.data.zero_()
    loss.backward()
    
    all_vectors = model.get_vectors()
    with torch.no_grad():
        for i in range(perturbed_data_encoded.size()[0]):
            # Update vector at step i
            for step in mask[i]:
                if random.randint(0, 16) != 0:
                    continue

                sign_data_grad = perturbed_data_encoded.grad[i][step].data.sign()
                closest_vectors = []
                closest_dist = 1e8
                for j in range(all_vectors.size()[0]):
                    word_vector = all_vectors[j]
                    if torch.norm(perturbed_data_encoded[i][step] - word_vector) < 1e-6:
                        continue

                    word = data_module.all_letters[j]
                    if word not in (string.ascii_letters + string.digits):
                        continue


                    sign_vector = (perturbed_data_encoded[i][step] - word_vector).data.sign()
                    diff = sign_vector - sign_data_grad
                    if torch.norm(diff) < closest_dist:
                        closest_vectors = []
                        closest_dist = torch.norm(diff)

                    if torch.abs(torch.norm(diff) - closest_dist) < 1e-6:
                        closest_vectors.append(word_vector)

                closest_vector = closest_vectors[random.randint(0, len(closest_vectors) - 1)]
                perturbed_data_encoded[i][step] = closest_vector

    return perturbed_data_encoded

def generate_adversary(model_path, http_request_str, adversary_path, attack_message_index):
    # Input data
    if http_request_str == None:
        data_adversary_str = 'x' * 1000 + ' ' * 30000 + 'x' * 1000
        attack_message_index += 1
        filename = '%s/attack_%d.txt' % (adversary_path, attack_message_index)
        with open(filename, 'w') as f:
            f.write(data_adversary_str)
        print ('Attack content path: %s' % filename)
        return attack_message_index

    label = torch.tensor([1], dtype=torch.long)
    _, data = data_module.lineToTensor(http_request_str)

    # Input network
    os.system('scp 172.31.38.81:~/regexnet/build/model.bin ./')
    model = torch.load(model_path)

    # Define network for attack
    # model_attack = Model_Attack(data_module.n_letters, 64, data_module.n_categories)
    model_attack = Model_Attack(data_module.n_letters, 64, data_module.n_categories, 3)
    model_attack.load_state_dict(model.state_dict())
    
    data_encoded = model_attack.encode(data)
    mask = get_mask(data, 'if-none-match')
    print ('Length of mask: %d' % len(mask[0]))

    target = 1 - label
    label_adversary = label
    data_adversary_encoded = data_encoded

    benign_probability = 0.0
    while label_adversary.item() == label.item():
    #while benign_probability < 0.6:
        data_adversary_encoded = data_adversary_encoded.clone().detach()
        data_adversary_encoded = fgsm(model_attack, data_adversary_encoded, mask, target)

        with torch.no_grad():
            output = model_attack.forward_after_encode(data_adversary_encoded)
            label_adversary = output.max(dim=1)[1]

        benign_probability = output.exp()[0][0].item()
        print (target, output.exp(), torch.norm(data_adversary_encoded - data_encoded).item())
    print ('Adaptive attack finish')

    #data_adversary = []
    #for j in range(data.size()[1]):
    #    data_adversary.append(model_attack.get_vector_index(data_adversary_encoded[0][j]))
    modified_data = []
    for j in mask[0]:
        modified_data.append((j, model_attack.get_vector_index(data_adversary_encoded[0][j])))
    
    data_adversary = data.clone()
    data_adversary_char_list = [c for c in http_request_str]
    for j, letter_index in modified_data:
        data_adversary[0][j] = letter_index
        data_adversary_char_list[j] = data_module.all_letters[letter_index]
    print ('Convert adaptive attack result to string')

    print ('Difference %d / %d' % ((data_adversary != data).sum().item(), data.size()[1]))

    data_adversary_str = ''.join(data_adversary_char_list)
    attack_content = get_attack_content(data_adversary_str)
    attack_message_index += 1
    filename = '%s/attack_%d.txt' % (adversary_path, attack_message_index)
    print ('Attack content path: %s' % filename)
    sys.stdout.flush()
    with open(filename, 'w+') as f:
        f.write(attack_content)
    return attack_message_index


def send_notification(attack_message_index):
    print ('Update')
    message = '%4d' % attack_message_index
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', PORT_ATTACKER))
    s.send(message.encode())
    s.close()

def recv_notification(conn):
    length_str_b = conn.recv(8, socket.MSG_WAITALL)
    length_str = length_str_b.decode()
    length = int(length_str)
    data_b = conn.recv(length, socket.MSG_WAITALL)
    data = data_b.decode()
    return data

if __name__ == "__main__":
    # Arguments
    model_path = 'model.bin'
    adversary_path = sys.argv[1]

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('0.0.0.0', PORT_MODEL_READY))
    sock.listen(1)

    attack_message_index = 0
    http_request_str = None
    while True:
        time_1 = time.time()
        attack_message_index = generate_adversary(
            model_path,
            http_request_str,
            adversary_path, 
            attack_message_index
        )
        time_2 = time.time()
        latency = (time_2 - time_1)
        print ("Latency for adaptive attack: %f s" % latency)
        
        send_notification(attack_message_index)

        conn, _ = sock.accept()
        http_request_str = recv_notification(conn)
        conn.close()
