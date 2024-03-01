
text/x-generic app.js ( HTML document, ASCII text )
var http = require('http');
var url = require('url');
var fs = require('fs');
//var swig = require('swig');

var server = http.createServer(function(req, res) {
    var q = url.parse(req.url, true);   // true to get query as object
    var qdata = q.query;                // retrun an object

    // check app id and action is update
    if (qdata.appid === 'app-id' && qdata.action === 'update') {

        var total = Number(qdata.total);
        if (total < 40) {
            var html = buildHtml(Number(qdata.total), qdata.time, Number(qdata.usedtoday), qdata.hotwater, qdata.battery);
        
            fs.writeFileSync('solar.html',html,{encoding:'utf8',flag:'w'});
            res.end('Web page updated...');
        }
    } else {
        res.end('No update performed');
    }
});
server.listen();

function buildHtml(total, time, usedtoday, hotwater, battery) {
    var head = '<div style="width: 550px; margin: 0 auto"><head></head><body style="background: black; color: white;  font-size: x-large;"><br><br><div style="text-align: center;">'
    
//    var body = '<h1>Solar Today: ' + total + ' kWh</h1> <h2>@' + time + '</h2>' + '<h2>Hot Water Status</h2> 
    var body = '<h1>@ ' + time + '</h1> <h1>Solar</h1><h2>Generated Today: ' + total + ' kWh</h2>' + '<h1>iBoost</h1> <h2>Saved Today: ' + usedtoday.toLocaleString() + ' Wh</h2> <h2>Water Tank: ' + hotwater + '</h2><h2>Sender Battery: ' + battery + '</h2>' 

    var tail = '</div></body></div></html>'
    
    return '<!DOCTYPE html>' + head + body + tail
};

