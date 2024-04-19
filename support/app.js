var https = require('https');
var url = require('url');
var fs = require('fs');

var server = https.createServer(function(req, res) {
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
    var today = 0;
    
    // format usedtoday value
    if (usedtoday > 0) {
        today = usedtoday / 1000;
    }
    
    var head = '<div style="width: 550px; margin: 0 auto"><head></head><body style="background: black; color: white;  font-size: x-large;"><br><br><div style="text-align: center;">'

    var updateTime = '<h1>@ ' + time + '</h1>';
    var solarHeader = '<h1>Solar</h1>';
    var generatedToday = '<h2>Generated Today: ' + total + ' kWh</h2>';
    var iboostHeader = '<h1>iBoost</h1>';
    var savedToday = '<h2>Saved Today: ' + today + ' kWh</h2>';
    var waterTank = '<h2>Water Tank: ' + hotwater + '</h2>';
    
    // If battery is low make text red to highlight
    var senderBattery = '<h2>Sender Battery: ';
    if (battery == 'LOW') {
        senderBattery += '<strong style="color: red;">' + battery + '</strong></h2>'
    } else {
        senderBattery += battery + '</h2>'
    }
    
    var body = updateTime + solarHeader + generatedToday + iboostHeader + savedToday + waterTank + senderBattery;
    
    var tail = '</div></body></div></html>'
    
    return '<!DOCTYPE html>' + head + body + tail
};