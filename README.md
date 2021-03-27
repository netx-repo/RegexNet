# regexnet
This is open source for our project, RegexNet.
This branch includes codes for experiments of end-to-end performance of baseline (Fig. 6, Fig. 9 and Fig. 12).

# Directory
- source: Codes for the core functions of RegexNet
- scripts: Scripts to help to run RegexNet
- experiments: Scripts to run some experiments.

# How to run experiments
## End-to-end performance
0. Select servers to run the system
    - For exmaple, you can use one CPU server.
    - CPU server: `MongoDB`, `redis` (optional), `node.js` application, `load_balancer`.
1. Modify IP addresses and absolute paths.
    - `node.js` application: Change the address of the `MongoDB` at `source/application/config/setting.json::databaseConnectionString`. Change the address of the `redis` at `source/application/app.js` for stored attacks (optional).
    - `haproxy`: Change the address of the `node.js` application at `source/haproxy/config/my_proxy.cfg`. A trick is that the name of the server is the same as the IP address of the server. 
    - `attacker`: All codes are in `source/attacker`. For the inteded attacker, change the value of 'X-Server' field in HTTP header to the IP address of the backend.
2. Modify vulnerable modules in `node.js` application.
    - Uncommet codes for required vulnerable modules in `source/application/app.js`. If you want to run experiments for stored attacks, you will also need to uncomment codes for connecting `redis` and setting the address to the `redis` correctly.
3. Compile codes.
    - `bash scripts/build.sh`
4. Run components
    - Start `mongodb`
    - Start `redis` for stored attacks. Insert the malicious content to some vulnerable module into the redis server with key `malicious_id`.
    - Start `node.js` application: `bash scripts/run.sh application`
    - Start load balancer: `bash scripts/run.sh haproxy`
    - Start background throughput: For reflected dattacks, use `ab -c 32 -n 10000000 http://127.0.0.1:8080/`. Here the URL is the address to the load balancer. For stored attacks, use `ab -c 32 -n 10000000 -H"stored_id:benign_id" http://127.0.0.1:8080/`.
5. Start attacking the system
    - To warm up the system, you need to wait for about 10s after starting the background throughput. Then you can launch attacks. For example, for `fresh` module, you can use `bash scripts/run.sh attacker fresh http://127.0.0.1:8080/ 60 30000`. Here `60` is the frequency of the attack in the unit of requests/minute, and `30000` is the length of the malicious content. The parameters might be a bit different for different attacks. You can refer to codes in `source/attacker`.
6. Observe the result.
    - The printed information of `load_balancer` is the throughput in the unit of request/second. Note that there will be no output if there is no packets.

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