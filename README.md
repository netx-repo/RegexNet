# regexnet
This is open source for our project, RegexNet.
This branch includes codes for experiments of end-to-end performance of RegexNet (Fig. 6, Fig. 9 and Fig. 12) as well as accuracy of the classifier (Fig. 13 and Fig. 14).

# Directory
- source: Codes for the core functions of RegexNet
- scripts: Scripts to help to run RegexNet
- experiments: Scripts to run some experiments.

# How to run experiments
## End-to-end performance
0. Select servers to run the system
    - For exmaple, you can use one CPU server and one GPU server.
    - CPU server: `MongoDB`, `redis` (optional), `sandbox`, `node.js` application, `backend`, `load_balancer`, `data_collector`.
    - GPU server: `data_manager`, `detector`.
1. Modify IP addresses and absolute paths.
    - `node.js` application: Change the address of the `MongoDB` at `source/application/config/setting.json::databaseConnectionString`. Change the address of the `redis` at `source/application/app.js` for stored attacks (optional).
    - `backend`: All codes are in `source/http_proxy/http_proxy.cpp`. Change the address of the `data_collector`. Change the address of the `sandbox`. Change the path to the `node.js` application, including the `node.js` path and `app.js` path.
    - `haproxy`: Change the address to the `detector` at `source/haproxy-with/include/customize.h`. Change the address of the `backend` at `source/haproxy-with/config/my_proxy.cfg`. A trick is that the name of the server is the same as the IP address of the server. 
    - `data_collector`: All codes are in `source/data_collector/data_collector.cpp`. Change the address to the `data_manager`.
    - `data_manager`: All codes are in `source/data_manager/data_manager.py`. Change the path to the model file, the flag file and the folder for samples.
    - `detector`: All codes are in `source/detector/detector.py`. Change the path to the model file and the flag file.
    - `attacker`: All codes are in `source/attacker`. For the inteded attacker, change the value of 'X-Server' field in HTTP header to the IP address of the backend.
2. Modify vulnerable modules in `node.js` application.
    - Uncommet codes for required vulnerable modules in `source/application/app.js`. If you want to run experiments for stored attacks, you will also need to uncomment codes for connecting `redis` and setting the address to the `redis` correctly.
3. Compile codes.
    - `bash scripts/build.sh`
4. Run components
    - Start `mongodb`
    - Start `redis` for stored attacks. Insert the malicious content to some vulnerable module into the redis server with key `malicious_id`.
    - Start sandbox: `bash scripts/run.sh application`
    - Start backend: `bash scripts/run.sh backend`
    - Start load balancer: `bash scripts/run.sh haproxy`
    - Start data collector: `bash scripts/run.sh collector`
    - Before start the data manager and the detector, clean the stale files: `rm -rf build/model.bin build/flag.txt`
    - Start data manager: `bash scripts/run.sh data_manager`
    - Start detector: `bash scripts/run.sh detector`
    - Start background throughput: For reflected dattacks, use `ab -c 32 -n 10000000 http://127.0.0.1:8080/`. Here the URL is the address to the load balancer. For stored attacks, use `ab -c 32 -n 10000000 -H"stored_id:benign_id" http://127.0.0.1:8080/`.
5. Start attacking the system
    - To warm up the system, you need to wait for about 30s after starting the background throughput. Then you can launch attacks. For example, for `fresh` module, you can use `bash scripts/run.sh attacker fresh http://127.0.0.1:8080/ 60 30000`. Here `60` is the frequency of the attack in the unit of requests/minute, and `30000` is the length of the malicious content. The parameters might be a bit different for different attacks. You can refer to codes in `source/attacker`.
6. Observe the result.
    - The printed information of `load_balancer` is the throughput in the unit of request/second. Note that there will be no output if there is no packets.

## Accuracy of the classifier
0. Select a GPU server to run experiments for the classifier.
1. Compile codes. `bash scripts/build.sh all` You can comment unnecessary items for faster compilation.
2. Run the requried scripts. For example, `bash experiments/classifier/3-single-complex.sh`. You can modify psettings in `experiments/classifier/3-single-complex.sh` or `experiments/classifier/util.sh`.

Note that, to run `5-single-rescue`, you need to generate malicious content with `ReScue` ([source code](https://2bdenny.github.io/ReScue/)) in advance. You can copy codes in `source/dataset/rescue_helper` to `ReScue/release` to do so.

## Compare with Rampart
Refer to `docs/rampart` to set up Rampart ([Code](https://github.com/cuhk-seclab/rampart)). To simulate distribtued ReDoS attacks, run Rampart in a docker container and send attacks from different docker containers.

<!-- # Dataset Overview
Each case has two datasets, one for train and one for test. The are generated in the same way, and the ratio of malicious requests and benign requests are the same

## Dataset: Single Constant
This dataset is to show that the classifier is able to detect malicious messages with constant malicious content on single header field.

To be more specific, in the malicious messages, a specfic header field has a specific constant string. This case often happens in stored ReDoS, as attackers need to read the value for a specific key in the database.

A sample in the dataset consists of a set of header fields. For benign requests, the content of all the fields are random strings. For malicious requests, the content of the field triggering ReDoS is the specific value, and the content of other fields are random strings.

## Dataset: Single Simple
This dataset is to show that the classifier is able to detect malicious messages against a simple regular expression on single header field.

To be more specific, in the malicious messages, a specfic header field has a string following some pattern like repeated blanks.

A sample in the dataset consists of a set of header fields. For benign requests, the content of all the fields are random strings. For malicious requests, the content of the field triggering ReDoS is the string following the pattern, and the content of other fields are random strings. -->