import os
from multiprocessing import Pool

from attack_once import attack_once

POOL_SIZE = 1
BATCH_SIZE = 1
TARGET_NUMBER = 1

def get_target_list(regex_filename, length_filename):
    regex_list = []
    with open(regex_filename) as f:
        for line in f.readlines():
            regex_list.append(line.strip())
    regex_length_list = []
    with open(length_filename) as f:
        for line in f.readlines():
            regex_length_list.append(int(line.strip()))
    return zip(regex_list, regex_length_list)

def generate_attacks(regex):
    with Pool(POOL_SIZE) as p:
        ret = p.map(attack_once, [regex] * BATCH_SIZE)
    return ret

def filter_none(attacks):
    return [attack for attack in attacks if attack is not None]

def main():
    regex_filename = 'regex.txt'
    length_filename = 'regex_length.txt'
    target_list = get_target_list(regex_filename, length_filename)

    for index, target in enumerate(target_list):
        print ('Attack', index)
        regex, length_limit = target
        attack_set = set()
        repeat = 0

        folder = 'output_%d' % index
        if not os.path.exists(folder):
            os.mkdir(folder)

        reg_file = folder + '/reg.txt'
        with open(reg_file, 'w') as f:
            f.write(regex)        

        log_file = folder + '/log.txt'
        with open(log_file, 'w') as flog:
            print (folder, reg_file, length_limit, file=flog)
            while len(attack_set) < TARGET_NUMBER and repeat < 6:
                attacks = filter_none(generate_attacks((reg_file, length_limit)))
                attack_set |= set(attacks)
                repeat += 1
                print ('' + str(len(attack_set)) + '(+' + str(len(attacks)) + ')', repeat, file=flog)
                if (len(attacks) == 0):
                    break
            for attack_count, attack in enumerate(attack_set):
                output_file = folder + ('/%d.txt' % attack_count)
                with open(output_file, 'w') as fatt:
                    fatt.write(attack + '\n')
            print (file=flog)

if __name__ == "__main__":
    main()