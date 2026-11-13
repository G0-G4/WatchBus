var transport = require('./transport');
var catalog = require('./catalog.json');
var nearby = [];
var board = null;
var pending = null;
var activeStop = null;
var searchRequest = 0;
var latestBoardRequest = 0;

function send(payload) {
  Pebble.sendAppMessage(payload, function () {}, function () {
    console.log('WatchBus AppMessage delivery failed');
  });
}

function reply(request, status, values) {
  send(Object.assign({ Request: request, Status: status }, values));
}

function findStops(request) {
  searchRequest = request;
  navigator.geolocation.getCurrentPosition(function (position) {
    if (request !== searchRequest) return;
    nearby = transport.nearby(catalog.stops, position.coords.latitude, position.coords.longitude);
    reply(request, 1, { Total: nearby.length });
  }, function () {
    reply(request, -1);
  }, { enableHighAccuracy: true, maximumAge: 10000, timeout: 10000 });
}

function fetchBoard(request, index, trigger) {
  var item = nearby[index];
  if (!item) {
    reply(request, -2);
    return;
  }
  var stop = item.stop;
  latestBoardRequest = request;
  activeStop = stop[0];
  if (!pending || pending.id !== activeStop) {
    var job = { id: activeStop, result: transport.arrivals(stop, trigger) };
    pending = job;
    job.result.then(clear, clear);
    function clear() { if (pending === job) pending = null; }
  }
  pending.result.then(function (result) {
    if (activeStop !== stop[0] || request !== latestBoardRequest) return;
    board = result;
    reply(request, result.partial ? 4 : 3, {
      Total: Math.min(result.rows.length, 20), Expires: result.expires
    });
  }, function () {
    if (activeStop === stop[0] && request === latestBoardRequest) reply(request, -2);
  });
}

Pebble.addEventListener('ready', function () { send({ PhoneReady: 1 }); });

Pebble.addEventListener('appmessage', function (event) {
  var message = event.payload;
  var request = message.Request;
  var index = message.Index;
  switch (message.Command) {
    case 1:
      findStops(request);
      break;
    case 2:
      if (nearby[index]) {
        var item = nearby[index];
        reply(request, 2, { Index: index, Name: item.stop[1].slice(0, 60),
          Detail: Math.round(item.distance) + ' м' + (item.stop[5] ? '\n' + item.stop[5] : '') });
      }
      break;
    case 3:
    case 4:
      fetchBoard(request, index, message.Command === 4 ? 'auto' : 'manual');
      break;
    case 5:
      if (board && board.rows[index]) {
        var row = board.rows[index];
        reply(request, 5, { Index: index, Route: row.route,
          Arrival: row.time, Live: row.live ? 1 : 0 });
      }
      break;
  }
});
