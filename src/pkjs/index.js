var transport = require('./transport');
var catalog = require('./catalog.json');
var nearby = [];
var indices = new Map();
var searchRequest = 0;
var board = null;
var messages = [];
var sending = false;
var prefetch = require('./prefetch')(preview);

function send(payload) {
  messages.push(payload);
  flush();
}

function flush() {
  if (sending || !messages.length) return;
  sending = true;
  Pebble.sendAppMessage(messages.shift(), complete, function () {
    console.log('WatchBus AppMessage delivery failed');
    complete();
  });
}

function complete() {
  sending = false;
  flush();
}

function reply(request, status, values) {
  send(Object.assign({ Request: request, Status: status }, values));
}

function preview(stop) {
  var index = indices.get(stop[0]);
  if (index === undefined) return;
  var snapshot = prefetch.snapshot(stop);
  var message = { Status: 6, Generation: searchRequest, Index: index, Expires: snapshot.expires };
  snapshot.rows.slice(0, 2).forEach(function (row, index) {
    message[index ? 'SecondRoute' : 'Route'] = row.route.slice(0, 16);
    message[index ? 'SecondArrival' : 'Arrival'] = row.time;
    message[index ? 'SecondLive' : 'Live'] = Number(row.live);
  });
  send(message);
}

function findStops(request) {
  searchRequest = request;
  indices.clear();
  nearby = [];
  prefetch.reset([]);
  prefetch.close();
  navigator.geolocation.getCurrentPosition(function (position) {
    if (request !== searchRequest) return;
    nearby = transport.nearby(catalog.stops, position.coords.latitude, position.coords.longitude);
    nearby.forEach(function (item, index) { indices.set(item.stop[0], index); });
    reply(request, 1, { Total: nearby.length });
    prefetch.reset(nearby.map(function (item) { return item.stop; }));
  }, function () {
    if (request === searchRequest) reply(request, -1);
  }, { enableHighAccuracy: true, maximumAge: 10000, timeout: 10000 });
}

function offer(request, value, status) {
  board.transfer = value;
  reply(request, status, { Total: Math.min(value.rows.length, 20), Expires: value.expires });
}

function fetchBoard(request, index, command) {
  if (!nearby[index]) { reply(request, -2); return; }
  var stop = nearby[index].stop;
  var current = { request: request };
  board = current;
  var sent = false;
  function intermediate(value) {
    if (board === current && !sent && value.rows.length) {
      sent = true;
      offer(request, value, 7);
    }
  }
  if (command === 8) intermediate(prefetch.snapshot(stop));
  prefetch.board(stop, command === 4 ? 'auto' : 'manual', command === 8, intermediate)
    .then(function (result) {
      if (board !== current) return;
      var status = result.partial ? 4 : 3;
      if (sent) {
        current.final = { value: result, status: status };
        send({ Status: 8, Generation: request });
      } else {
        offer(request, result, status);
      }
    }, function () {
      if (board !== current) return;
      if (sent) {
        current.final = { status: -2 };
        send({ Status: 8, Generation: request });
      } else reply(request, -2);
    });
}

Pebble.addEventListener('ready', function () { send({ PhoneReady: 1 }); });
Pebble.addEventListener('appmessage', function (event) {
  var message = event.payload;
  var request = message.Request;
  var index = message.Index;
  var item = nearby[index];
  switch (message.Command) {
    case 1:
      board = null;
      findStops(request);
      break;
    case 2:
      if (item) {
        reply(request, 2, { Index: index, Name: item.stop[1].slice(0, 60),
          Detail: Math.round(item.distance) + ' м' + (item.stop[5] ? '\n' + item.stop[5] : '') });
        prefetch.enqueue([item.stop]);
        preview(item.stop);
      }
      break;
    case 3:
    case 4:
    case 8:
      fetchBoard(request, index, message.Command);
      break;
    case 5:
      if (board && board.transfer && board.transfer.rows[index]) {
        var row = board.transfer.rows[index];
        reply(request, 5, { Index: index, Route: row.route, Arrival: row.time, Live: Number(row.live) });
      }
      break;
    case 6:
      if (board) { board = null; prefetch.close(); }
      prefetch.select(item ? item.stop : null);
      break;
    case 7:
      if (board && board.final) {
        if (board.final.value) offer(request, board.final.value, board.final.status);
        else reply(request, board.final.status);
      }
      break;
  }
});
