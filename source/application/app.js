const express = require('express');
const path = require('path');
const logger = require('morgan');
const cookieParser = require('cookie-parser');
const bodyParser = require('body-parser');
const session = require('express-session');
const moment = require('moment');
const MongoStore = require('connect-mongodb-session')(session);
const MongoClient = require('mongodb').MongoClient;
const numeral = require('numeral');
const helmet = require('helmet');
const colors = require('colors');
const common = require('./lib/common');
const mongodbUri = require('mongodb-uri');
let handlebars = require('express-handlebars');

// Validate our settings schema
const Ajv = require('ajv');
const ajv = new Ajv({useDefaults: true});

const baseConfig = ajv.validate(require('./config/baseSchema'), require('./config/settings.json'));
if(baseConfig === false){
    console.log(colors.red(`settings.json incorrect: ${ajv.errorsText()}`));
    process.exit(2);
}

// get config
let config = common.getConfig();

// Validate the payment gateway config
if(config.paymentGateway === 'paypal'){
    const paypalConfig = ajv.validate(require('./config/paypalSchema'), require('./config/paypal.json'));
    if(paypalConfig === false){
        console.log(colors.red(`PayPal config is incorrect: ${ajv.errorsText()}`));
        process.exit(2);
    }
}
if(config.paymentGateway === 'stripe'){
    const stripeConfig = ajv.validate(require('./config/stripeSchema'), require('./config/stripe.json'));
    if(stripeConfig === false){
        console.log(colors.red(`Stripe config is incorrect: ${ajv.errorsText()}`));
        process.exit(2);
    }
}
if(config.paymentGateway === 'authorizenet'){
    const authorizenetConfig = ajv.validate(require('./config/authorizenetSchema'), require('./config/authorizenet.json'));
    if(authorizenetConfig === false){
        console.log(colors.red(`Authorizenet config is incorrect: ${ajv.errorsText()}`));
        process.exit(2);
    }
}

// require the routes
const index = require('./routes/index');
const admin = require('./routes/admin');
const product = require('./routes/product');
const customer = require('./routes/customer');
const order = require('./routes/order');
const user = require('./routes/user');
const paypal = require('./routes/payments/paypal');
const stripe = require('./routes/payments/stripe');
const authorizenet = require('./routes/payments/authorizenet');

const app = express();

// view engine setup
app.set('views', path.join(__dirname, '/views'));
app.engine('hbs', handlebars({
    extname: 'hbs',
    layoutsDir: path.join(__dirname, 'views', 'layouts'),
    defaultLayout: 'layout.hbs',
    partialsDir: [ path.join(__dirname, 'views') ]
}));
app.set('view engine', 'hbs');

// helpers for the handlebar templating platform
handlebars = handlebars.create({
    helpers: {
        perRowClass: function(numProducts){
            if(parseInt(numProducts) === 1){
                return'col-md-12 col-xl-12 col m12 xl12 product-item';
            }
            if(parseInt(numProducts) === 2){
                return'col-md-6 col-xl-6 col m6 xl6 product-item';
            }
            if(parseInt(numProducts) === 3){
                return'col-md-4 col-xl-4 col m4 xl4 product-item';
            }
            if(parseInt(numProducts) === 4){
                return'col-md-3 col-xl-3 col m3 xl3 product-item';
            }

            return'col-md-6 col-xl-6 col m6 xl6 product-item';
        },
        menuMatch: function(title, search){
            if(!title || !search){
                return'';
            }
            if(title.toLowerCase().startsWith(search.toLowerCase())){
                return'class="navActive"';
            }
            return'';
        },
        getTheme: function(view){
            return`themes/${config.theme}/${view}`;
        },
        formatAmount: function(amt){
            if(amt){
                return numeral(amt).format('0.00');
            }
            return'0.00';
        },
        amountNoDecimal: function(amt){
            if(amt){
                return handlebars.helpers.formatAmount(amt).replace('.', '');
            }
            return handlebars.helpers.formatAmount(amt);
        },
        getStatusColor: function (status){
            switch(status){
            case'Paid':
                return'success';
            case'Approved':
                return'success';
            case'Approved - Processing':
                return'success';
            case'Failed':
                return'danger';
            case'Completed':
                return'success';
            case'Shipped':
                return'success';
            case'Pending':
                return'warning';
            default:
                return'danger';
            }
        },
        checkProductOptions: function (opts){
            if(opts){
                return'true';
            }
            return'false';
        },
        currencySymbol: function(value){
            if(typeof value === 'undefined' || value === ''){
                return'$';
            }
            return value;
        },
        objectLength: function(obj){
            if(obj){
                return Object.keys(obj).length;
            }
            return 0;
        },
        checkedState: function (state){
            if(state === 'true' || state === true){
                return'checked';
            }
            return'';
        },
        selectState: function (state, value){
            if(state === value){
                return'selected';
            }
            return'';
        },
        isNull: function (value, options){
            if(typeof value === 'undefined' || value === ''){
                return options.fn(this);
            }
            return options.inverse(this);
        },
        toLower: function (value){
            if(value){
                return value.toLowerCase();
            }
            return null;
        },
        formatDate: function (date, format){
            return moment(date).format(format);
        },
        ifCond: function (v1, operator, v2, options){
            switch(operator){
            case'==':
                return(v1 === v2) ? options.fn(this) : options.inverse(this);
            case'!=':
                return(v1 !== v2) ? options.fn(this) : options.inverse(this);
            case'===':
                return(v1 === v2) ? options.fn(this) : options.inverse(this);
            case'<':
                return(v1 < v2) ? options.fn(this) : options.inverse(this);
            case'<=':
                return(v1 <= v2) ? options.fn(this) : options.inverse(this);
            case'>':
                return(v1 > v2) ? options.fn(this) : options.inverse(this);
            case'>=':
                return(v1 >= v2) ? options.fn(this) : options.inverse(this);
            case'&&':
                return(v1 && v2) ? options.fn(this) : options.inverse(this);
            case'||':
                return(v1 || v2) ? options.fn(this) : options.inverse(this);
            default:
                return options.inverse(this);
            }
        },
        isAnAdmin: function (value, options){
            if(value === 'true' || value === true){
                return options.fn(this);
            }
            return options.inverse(this);
        }
    }
});

// session store
let store = new MongoStore({
    uri: config.databaseConnectionString,
    collection: 'sessions'
});

app.enable('trust proxy');
app.use(helmet());
app.set('port', process.env.PORT || 8080);
app.use(logger('dev'));
app.use(bodyParser.json());
app.use(bodyParser.urlencoded({extended: false}));
app.use(cookieParser('5TOCyfH3HuszKGzFZntk'));
app.use(session({
    resave: true,
    saveUninitialized: true,
    secret: 'pAgGxo8Hzg7PFlv1HpO8Eg0Y6xtP7zYx',
    cookie: {
        path: '/',
        httpOnly: true,
        maxAge: 3600000 * 24
    },
    store: store
}));

// serving static content
app.use(express.static(path.join(__dirname, 'public')));
app.use(express.static(path.join(__dirname, 'views', 'themes')));

// Make stuff accessible to our router
// var client = require('redis').createClient(6379, "127.0.0.1"); // Connect to redis at netx7
// var fresh = require('fresh');
// var charset = require('charset');
// var content = require('content');
// var forwarded = require('forwarded');
// var MobileDetect = require('mobile-detect');
// var platform = require('platform')
// var ToughCookie = require('tough-cookie').Cookie;
// NOTE: Already required. var moment = require('moment');
// var marked = require('marked');
// var UAParser = require('ua-parser-js');
// var ms = require('ms');
// var uglify = require('uglify-js');
// var useragent = require('useragent');
app.use((req, res, next) => {
    // Trigger Stored Attack
    // stored_id = req.headers["stored_id"];
    // client.get(stored_id, function (error, result) {
    //     if (error) {
    //         console.log(error);
    //         throw error;
    //     }

    //     // Trigger fresh
    //     // var reqHeaders = { 'if-none-match': result };
    //     // var resHeaders = { 'etag': '"foo"' };
    //     // fresh(reqHeaders, resHeaders);

    //     // Trigger charset
    //     // charset(result);

    //     // Trigger content
    //     // content.type(result);

    //     // Trigger forwarded
    //     // forwarded({
    //     //    "headers": {
    //     //        "x-forwarded-for": result
    //     //    },
    //     //    "connection": {
    //     //        "remoteAddress": "0.0.0.0"
    //     //    }
    //     // });


    //     // Trigger mobile-detect
    //     // var md = new MobileDetect(result);
    //     // md.phone();

    //     //Trigger platform
    //     // platform.parse(result);

    //     // Trigger useragent
    //     // var agent = useragent.parse(result);

    //     // Trigger tough-cookie
    //     // ToughCookie.parse(result);
        
    //     //Trigger moment
    //     // moment(result, "MMM");

    //     // Trigger marked
    //     // marked(result);

    //     // Trigger ua-parser-js
    //     // UAParser(result);

    //     // Trigger ms
    //     // ms(result);

    //     // Trigger uglify-js
    //     // try {
    //     //     uglify.parse(result);
    //     // }
    //     // catch (ex) {
    //     //     ;
    //     // };
    // });

    // Trigger fresh, which is in the express

    // Trigger charset
    // if (req.headers["content-type"])
    //     charset(req.headers["content-type"]);

    // Trigger content
    // if (req.headers["content-type"])
    //     content.type(req.headers["content-type"]);

    // Trigger forwarded
    /*forwarded(req);*/

    // Trigger mobile-detect
    /*if (req.headers['user-agent']) {
        var md = new MobileDetect(req.headers['user-agent']);
        md.phone();
    }*/

    // Trigger platform
    /*if (req.headers['user-agent']) {
        platform.parse(req.headers['user-agent']);
    }*/

    // Trigger tough-cookie
    /*if (req.headers['tough-cookie']) {
        ToughCookie.parse(req.headers['tough-cookie']);
    }*/
        
    // Trigger moment
    /*if (req.headers['moment'])
        moment(req.headers['moment'], "MMM");*/

    // Trigger marked
    /*if (req.headers['marked'])
        marked(req.headers['marked']);*/

    // Trigger ua-parser-js
    // if (req.headers['ua-parser-js'])
    //     UAParser(req.headers['ua-parser-js']);

    // Trigger ms
    /*if (req.headers['ms'])
        ms(req.headers['ms']);*/

    // Trigger uglify-js
    /*if (req.headers['uglify-js'])
        uglify.parse(req.headers['uglify-js']);*/

    // Trigger useragent
    /*if (req.headers['user-agent'])
        var agent = useragent.parse(req.headers['user-agent']);*/

    req.handlebars = handlebars;
    next();
});

// update config when modified
app.use((req, res, next) => {
    next();
    if(res.configDirty){
        config = common.getConfig();
        app.config = config;
    }
});

// Ran on all routes
app.use((req, res, next) => {
    res.setHeader('Cache-Control', 'no-cache, no-store');
    next();
});

// setup the routes
app.use('/', index);
app.use('/', customer);
app.use('/', product);
app.use('/', order);
app.use('/', user);
app.use('/', admin);
app.use('/paypal', paypal);
app.use('/stripe', stripe);
app.use('/authorizenet', authorizenet);

// catch 404 and forward to error handler
app.use((req, res, next) => {
    let err = new Error('Not Found');
    err.status = 404;
    next(err);
});

// error handlers

// development error handler
// will print stacktrace
if(app.get('env') === 'development'){
    app.use((err, req, res, next) => {
        console.error(colors.red(err.stack));
        res.status(err.status || 500);
        res.render('error', {
            message: err.message,
            error: err,
            helpers: handlebars.helpers
        });
    });
}

// production error handler
// no stacktraces leaked to user
app.use((err, req, res, next) => {
    console.error(colors.red(err.stack));
    res.status(err.status || 500);
    res.render('error', {
        message: err.message,
        error: {},
        helpers: handlebars.helpers
    });
});

// Nodejs version check
const nodeVersionMajor = parseInt(process.version.split('.')[0].replace('v', ''));
if(nodeVersionMajor < 7){
    console.log(colors.red(`Please use Node.js version 7.x or above. Current version: ${nodeVersionMajor}`));
    process.exit(2);
}

app.on('uncaughtException', (err) => {
    console.error(colors.red(err.stack));
    process.exit(2);
});

MongoClient.connect(config.databaseConnectionString, {}, (err, client) => {
    // On connection error we display then exit
    if(err){
        console.log(colors.red('Error connecting to MongoDB: ' + err));
        process.exit(2);
    }

    // select DB
    const dbUriObj = mongodbUri.parse(config.databaseConnectionString);
    let db;
    // if in testing, set the testing DB
    if(process.env.NODE_ENV === 'test'){
        db = client.db('testingdb');
    }else{
        db = client.db(dbUriObj.database);
    }

    // setup the collections
    db.users = db.collection('users');
    db.products = db.collection('products');
    db.orders = db.collection('orders');
    db.pages = db.collection('pages');
    db.menu = db.collection('menu');
    db.customers = db.collection('customers');

    // add db to app for routes
    app.dbClient = client;
    app.db = db;
    app.config = config;
    app.port = app.get('port');

    // run indexing
    common.runIndexing(app)
    .then(app.listen(app.get('port')))
    .then(() => {
        // lift the app
        app.emit('appStarted');
        console.log(colors.green('expressCart running on host: http://localhost:' + app.get('port')));
    })
    .catch((err) => {
        console.error(colors.red('Error setting up indexes:' + err));
        process.exit(2);
    });
});

module.exports = app;
