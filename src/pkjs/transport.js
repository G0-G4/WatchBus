var API = 'https://api.moscowapp.mos.ru/v8.2/';
var TIMETABLE = 'https://transport.mos.ru/ru/ajax/App/V2_ScheduleV2Controller/detail';
var moscowOffset = 3 * 60 * 60;
var retryUntil = {};
var scheduleCache = {};
var scheduleCacheDay;

function request(url, operation, trigger) {
  var host = url.split('/')[2];
  return new Promise(function (resolve, reject) {
    var started = Date.now();
    if (started < (retryUntil[host] || 0)) {
      console.log('WatchBus ' + trigger + ' ' + operation + ' retryAt=' + retryUntil[host]);
      reject(new Error('retry-after'));
      return;
    }
    var xhr = new XMLHttpRequest();
    function finish(error) {
      var message = 'WatchBus ' + trigger + ' ' + operation +
        ' status=' + xhr.status + ' ms=' + (Date.now() - started);
      if (error) message += ' error=' + error;
      if (xhr.status === 429) message += ' retryAt=' + (retryUntil[host] || 'unspecified');
      console.log(message);
      if (error) reject(new Error(error));
      else resolve(xhr.responseText);
    }
    xhr.open('GET', url);
    if (operation === 'schedule') xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
    xhr.timeout = 12000;
    xhr.onload = function () {
      if (xhr.status === 429) {
        var value = xhr.getResponseHeader('Retry-After');
        var time = /^\d+$/.test(value || '') ? Date.now() + Number(value) * 1000 : Date.parse(value);
        if (Number.isFinite(time) && time > Date.now()) retryUntil[host] = time;
      }
      finish(xhr.status === 200 ? null : 'http');
    };
    xhr.onerror = function () { finish('network'); };
    xhr.ontimeout = function () { finish('timeout'); };
    xhr.send();
  });
}

function distance(lat, lon, stop) {
  var radians = Math.PI / 180;
  var a = Math.sin((stop[2] - lat) * radians / 2);
  var b = Math.sin((stop[3] - lon) * radians / 2);
  var h = a * a + b * b * Math.cos(lat * radians) * Math.cos(stop[2] * radians);
  return 12742000 * Math.asin(Math.sqrt(Math.min(1, h)));
}

function nearby(stops, lat, lon) {
  return stops.map(function (stop) { return { stop: stop, distance: distance(lat, lon, stop) }; })
    .filter(function (item) { return item.distance <= 1000; })
    .sort(function (a, b) {
      return a.distance - b.distance || (a.stop[0] < b.stop[0] ? -1 : 1);
    });
}

function live(board, uuid, now) {
  if (!board || board.id !== uuid || !Array.isArray(board.routePath)) throw new Error('format');
  return board.routePath.reduce(function (rows, route) {
    if ((route.type !== 'bus' && route.type !== 'tram') || typeof route.number !== 'string') return rows;
    (route.externalForecast || []).forEach(function (forecast) {
      if (forecast.byTelemetry === 1 && Number.isFinite(forecast.time) && forecast.time > 0) {
        rows.push({ route: route.number, time: now + forecast.time, live: true });
      }
    });
    return rows;
  }, []);
}

function entities(text) {
  var named = { quot: '"', amp: '&', apos: "'", lt: '<', gt: '>', nbsp: ' ' };
  return text.replace(/&(#x[\da-f]+|#\d+|\w+);/gi, function (whole, key) {
    if (key[0] === '#') return String.fromCharCode(key[1] === 'x' ?
      parseInt(key.slice(2), 16) : Number(key.slice(1)));
    return named[key] || whole;
  });
}

function calendarDay(now) {
  return Math.floor((now + moscowOffset) / 86400) * 86400 - moscowOffset;
}

function scheduleTimes(document, internalId, stop, day) {
  var coordinates = /data-coords="([^"]+)"/.exec(document);
  if (!coordinates) {
    if (document.indexOf('Расписание на заданную дату не найдено') >= 0) return [];
    throw new Error('format');
  }
  var points = JSON.parse(entities(coordinates[1])).features.filter(function (feature) {
    return feature.geometry.type === 'Point';
  });
  var blocks = document.match(/<li\b[^>]*data-stop="\d+"[^>]*>[\s\S]*?<\/li>/g) || [];
  var normalize = function (name) {
    return name.replace(/["«»]/g, '').replace(/\s+/g, ' ').trim().toLowerCase().replace(/ё/g, 'е');
  };
  var entries = blocks.map(function (block) {
    var name = /class="a_dotted d-inline">([^<]+)/.exec(block);
    return {
      block: block,
      number: Number(/data-stop="(\d+)"/.exec(block)[1]),
      name: name ? normalize(entities(name[1])) : null
    };
  });
  if (!points.some(function (point) { return String(point.id) === String(internalId); })) return [];
  if (!entries.length) return [];
  var offsets = [];
  points.forEach(function (point, index) {
    if (normalize(point.properties.hintContent) !== entries[0].name) return;
    var offset = index - entries[0].number;
    if (entries.every(function (entry) {
      var match = points[entry.number + offset];
      return match && normalize(match.properties.hintContent) === entry.name;
    })) offsets.push(offset);
  });
  if (offsets.length !== 1) throw new Error('format');
  var times = [];
  entries.forEach(function (entry) {
    var point = points[entry.number + offsets[0]];
    if (String(point.id) !== String(internalId)) return;
    if (distance(point.geometry.coordinates[1], point.geometry.coordinates[0], stop) > 3) return;
    var block = entry.block;
    var date = /data-date="(\d{4}-\d\d-\d\d)"/.exec(block);
    if (!date || Date.parse(date[1] + 'T00:00:00+03:00') / 1000 !== day) throw new Error('format');
    var hours = /<strong>(\d\d):<\/strong>([\s\S]*?)(?=<strong>\d\d:<\/strong>|$)/g;
    var hour;
    while ((hour = hours.exec(block))) {
      var minutes = /class="div10[^"]*">\s*(\d\d)\s*</g;
      var minute;
      while ((minute = minutes.exec(hour[2]))) {
        if (+hour[1] < 24 && +minute[1] < 60) times.push(day + +hour[1] * 3600 + +minute[1] * 60);
      }
    }
  });
  return times;
}

function planned(stop, internalId, now, trigger) {
  var day = calendarDay(now);
  if (scheduleCacheDay !== day) {
    scheduleCache = {};
    scheduleCacheDay = day;
  }
  var days = now + 3600 >= day + 86400 ? [day, day + 86400] : [day];
  var jobs = [];
  stop[4].forEach(function (route) {
    days.forEach(function (date) {
      [0, 1].forEach(function (direction) {
        jobs.push({ route: route, day: date, direction: direction });
      });
    });
  });
  var rows = [];
  var failures = 0;
  var seen = {};
  function load(job) {
    var key = [stop[0], job.route[0], job.day, job.direction].join(':');
    if (scheduleCache[key]) return Promise.resolve(scheduleCache[key]);
    var parts = new Date((job.day + moscowOffset) * 1000).toISOString().slice(0, 10).split('-');
    var url = TIMETABLE + '?mgt_schedule%5Broute%5D=' + job.route[0] +
      '&mgt_schedule%5Bdirection%5D=' + job.direction +
      '&mgt_schedule%5Bdate%5D=' + parts.reverse().join('.');
    return request(url, 'schedule', trigger).then(function (document) {
      var times = scheduleTimes(document, internalId, stop, job.day);
      scheduleCache[key] = times;
      return times;
    });
  }
  function next() {
    var job = jobs.shift();
    if (!job) return Promise.resolve();
    return load(job).then(function (times) {
      times.forEach(function (time) {
        var key = [job.route[0], job.direction, time].join(':');
        if (time > now && !seen[key]) {
          seen[key] = true;
          rows.push({ route: job.route[1], time: time, live: false });
        }
      });
    }).catch(function (error) {
      failures++;
      if (error.message === 'format') console.log('WatchBus schedule error=format');
    }).then(next);
  }
  return Promise.all([next(), next(), next()]).then(function () {
    return { rows: rows, failed: failures > 0 };
  });
}

function mergeArrivals(forecasts, schedule) {
  var forecastTrips = new Set(forecasts.map(function (row) { return row.tripId; }).filter(Boolean));
  return forecasts.concat(schedule.filter(function (row) {
    return !row.tripId || !forecastTrips.has(row.tripId);
  })).sort(function (a, b) {
    return a.time - b.time || Number(b.live) - Number(a.live);
  });
}

function arrivals(stop, trigger) {
  return request(API + 'stop_v2/' + encodeURIComponent(stop[0]), 'forecast', trigger)
    .then(function (body) {
      var now = Math.floor(Date.now() / 1000);
      var board;
      var forecasts;
      try {
        board = JSON.parse(body);
        forecasts = live(board, stop[0], now);
      } catch (error) {
        console.log('WatchBus ' + trigger + ' forecast error=format');
        throw new Error('format');
      }
      return planned(stop, board.internal_id, now, trigger).then(function (schedule) {
        return { rows: mergeArrivals(forecasts, schedule.rows), expires: now + 120,
          partial: schedule.failed };
      });
    });
}

module.exports = { nearby: nearby, live: live, scheduleTimes: scheduleTimes, arrivals: arrivals, mergeArrivals: mergeArrivals };
