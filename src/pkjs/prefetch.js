var transport = require('./transport');

function create(onPreview) {
  var cache = new Map();
  var queue = [];
  var queued = new Set();
  var running = 0;
  var selection = null;
  var timer;
  var backgroundSchedule = false;
  var openStop = null;

  function now() { return Math.floor(Date.now() / 1000); }

  function entry(stop) {
    if (!cache.has(stop[0])) cache.set(stop[0], {});
    return cache.get(stop[0]);
  }

  function snapshot(stop) {
    var data = entry(stop);
    var time = now();
    var forecast = data.forecast;
    var live = forecast && forecast.expires > time ? forecast.rows : [];
    var planned = data.schedule ? data.schedule.rows : [];
    return {
      rows: transport.mergeArrivals(live.filter(future), planned.filter(future)),
      expires: forecast ? forecast.expires : 0
    };
    function future(row) { return row.time > time; }
  }

  function forecast(stop, trigger) {
    var data = entry(stop);
    if (!data.forecastJob) {
      data.forecastJob = transport.forecast(stop, trigger).then(function (result) {
        data.forecast = result;
        onPreview(stop);
        return result;
      }).finally(function () { delete data.forecastJob; });
    }
    return data.forecastJob;
  }

  function schedule(stop, trigger) {
    var data = entry(stop);
    if (!data.scheduleJob) {
      data.scheduleJob = transport.planned(stop, data.forecast.internalId, now(), trigger)
        .then(function (result) {
          if (!result.failed || result.rows.length || !data.schedule) data.schedule = result;
          onPreview(stop);
          return result;
        }).finally(function () { delete data.scheduleJob; });
    }
    return data.scheduleJob;
  }

  function pump() {
    while (!openStop && running < 2 && queue.length) {
      var stop = queue.shift();
      queued.delete(stop[0]);
      var data = entry(stop);
      if (data.forecastJob || data.attempted || (data.forecast && data.forecast.expires > now())) continue;
      data.attempted = true;
      running++;
      forecast(stop, 'prefetch').catch(function () {}).finally(function () {
        running--;
        pump();
        considerSchedule();
      });
    }
  }

  function enqueue(stops) {
    stops.forEach(function (stop) {
      if (!queued.has(stop[0])) {
        queued.add(stop[0]);
        queue.push(stop);
      }
    });
    pump();
  }

  function considerSchedule() {
    if (!selection || timer || openStop || backgroundSchedule) return;
    var stop = selection;
    var data = entry(stop);
    if (!data.forecast || data.forecast.expires <= now() ||
        snapshot(stop).rows.length || data.scheduleAttempted) return;
    timer = setTimeout(function () {
      timer = null;
      if (selection !== stop || openStop || snapshot(stop).rows.length) return;
      data.scheduleAttempted = true;
      backgroundSchedule = true;
      schedule(stop, 'prefetch').catch(function () {}).finally(function () {
        backgroundSchedule = false;
        considerSchedule();
      });
    }, 750);
  }

  function select(stop) {
    if (selection !== stop) {
      clearTimeout(timer);
      timer = null;
      selection = stop;
    }
    considerSchedule();
  }

  function reset(stops) {
    select(null);
    queue = [];
    queued.clear();
    cache.forEach(function (data) {
      delete data.attempted;
      delete data.scheduleAttempted;
    });
    enqueue(stops.slice(0, 5));
  }

  function board(stop, trigger, useCache, update) {
    openStop = stop;
    select(null);
    var data = entry(stop);
    var current = data.forecast;
    var job = data.forecastJob || (useCache && current && current.expires > now()
      ? Promise.resolve(current) : forecast(stop, trigger));
    return job.then(function () {
      update(snapshot(stop));
      return schedule(stop, trigger);
    }).then(function (result) {
      var value = snapshot(stop);
      value.partial = result.failed;
      return value;
    });
  }

  function close() { openStop = null; pump(); }

  return { reset: reset, enqueue: enqueue, select: select, board: board, close: close, snapshot: snapshot };
}

module.exports = create;
