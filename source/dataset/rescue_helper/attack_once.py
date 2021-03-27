import os
import subprocess

SUCCESS_MESSAGE = 'Attack success, attack string is:\n'

def attack_once(input):
    reg_file, length_limit = input
    try:
        # output = subprocess.check_output('timeout 60 java -jar ReScue.jar -q -sl %d -pz 1000 -g 200 -mp 10 -cp 10 < %s' % (length_limit, reg_file), shell=True)
        # output = output.decode()
        # print (output)
        os.system('java -jar ReScue.jar -q -sl %d -pz 1000 -g 200 -mp 10 -cp 10 < %s' % (length_limit, reg_file))

        # if output.find(SUCCESS_MESSAGE) >= 0:
        #     location = output.find(SUCCESS_MESSAGE) + len(SUCCESS_MESSAGE)
        #     return output[location:].strip('\n')
        # else:
        #     return None
        return None
    except:
        return None

if __name__ == "__main__":
    print ('Test')
    regex = '/ *, */'
    length_limit = 100000

    reg_file = 'tmp.txt'
    with open(reg_file, 'w') as f:
        f.write(regex)

    print ('Regular expression file: ' + reg_file)
    print ('Length limit: ' + str(length_limit))

    ret = attack_once((reg_file, length_limit))
    # if ret is None:
    #     print ('Attack failed')
    # else:
    #     print ('Attck result: ' + '<' + ret + '>')