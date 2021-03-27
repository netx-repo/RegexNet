import os

if __name__ == "__main__":
    # workspace = 'useragent'
    # regex = '/((?:[A-z0-9]+|[A-z\\-]+?)?(?:the)?(?:[Ss][Pp][Ii][Dd][Ee][Rr]|[Ss]crape|[A-Za-z0-9-]*(?:[^C][^Uu])[Bb]ot|[Cc][Rr][Aa][Ww][Ll])[A-z0-9]*)(?:(?:[\\/]|v)(\\d+)(?:\\.(\\d+)(?:\\.(\\d+))?)?)?/'
    # length_limit = 2000
    workspace = 'marked'
    regex= '/^\\b_((?:__|[\\s\\S])+?)_\\b|^\\*((?:\\*\\*|[\\s\\S])+?)\\*(?!\\*)/'
    length_limit = 2000

    count = len(os.listdir(workspace))
    folder = workspace + '/' + str(count)
    os.mkdir(folder)

    reg_file = folder + '/regex.txt'
    with open(reg_file, 'w') as f:
        f.write(regex)

    log_file = folder + '/log.txt'
    cmd = 'java -Xss16M -jar ReScue.jar -q -sl %d -pz 1000 -g 500 -mp 10 -cp 40 <%s >%s 2>&1' % (length_limit, reg_file, log_file)

    cmd_file = folder + '/cmd.txt'
    with open(cmd_file, 'w') as f:
        print (cmd, file=f)

    os.system(cmd)

    complete_file = folder + '/complete.txt'
    with open(complete_file, 'w') as f:
        print ('Complete', file=f)