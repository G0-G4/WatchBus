const assert = require('node:assert/strict');
const test = require('node:test');
const { live, scheduleTimes, nearby } = require('../src/pkjs/transport');

test('only telemetry with a valid ETA is a live forecast', () => {
  const uuid = 'stop';
  const routePath = [
    { number: '688', type: 'bus', externalForecast: [{ time: 50, byTelemetry: 1 }] },
    { number: '733', type: 'bus', electrobus: true, externalForecast: [{ time: 70, byTelemetry: 1 }] },
    { number: 'А', type: 'tram', externalForecast: [{ time: 80, byTelemetry: 1 },
      { time: 90, byTelemetry: 0 }, { time: -1, byTelemetry: 1 }] },
  ];
  assert.deepEqual(live({ id: uuid, routePath }, uuid, 1000), [
    { route: '688', time: 1050, live: true },
    { route: '733', time: 1070, live: true },
    { route: 'А', time: 1080, live: true },
  ]);
  assert.deepEqual(live({ id: uuid, routePath: [] }, uuid, 1000), []);
  assert.throws(() => live({ id: 'another stop', routePath }, uuid, 1000), /format/);
});

test('schedule resolves shifted row numbers and keeps the Moscow calendar date', () => {
  const stop = ['uuid', 'Площадка', 55.7, 37.4];
  const day = Date.parse('2026-11-13T00:00:00+03:00') / 1000;
  const feature = { id: 2029, geometry: { type: 'Point', coordinates: [37.4, 55.7] },
    properties: { hintContent: 'Площадка' } };
  const coordinates = JSON.stringify({ features: [feature] }).replace(/"/g, '&quot;');
  const document = `<div data-coords="${coordinates}"><li data-date="2026-11-13" data-stop="2">
    <div class="a_dotted d-inline">Площадка</div>
    <strong>00:</strong><div class="div10">24</div>
    <strong>23:</strong><div class="div10">54</div></li></div>`;
  assert.deepEqual(scheduleTimes(document, 2029, stop, day), [day + 1440, day + 86040]);
  assert.deepEqual(scheduleTimes(document, 2030, stop, day), []);
  assert.deepEqual(scheduleTimes('Расписание на заданную дату не найдено', 2029, stop, day), []);
  assert.throws(() => scheduleTimes(document, 2029, stop, day + 86400), /format/);
  assert.throws(() => scheduleTimes('<html></html>', 2029, stop, day), /format/);
});

test('nearby keeps same-name platforms distinct and order deterministic', () => {
  const stops = [['b', 'Площадка', 55.7, 37.4], ['a', 'Площадка', 55.7, 37.4],
    ['c', 'Дальняя', 56, 37.4]];
  assert.deepEqual(nearby(stops, 55.7, 37.4).map(x => x.stop[0]), ['a', 'b']);
});

test('mixed board replaces only a reliably identified scheduled trip', () => {
  const { mergeArrivals } = require('../src/pkjs/transport');
  const forecast = { route: '733', time: 120, live: true, tripId: 'trip-1' };
  const scheduled = { route: '733', time: 100, live: false, tripId: 'trip-1' };
  const uncertain = { route: '733', time: 121, live: false };
  assert.deepEqual(mergeArrivals([forecast], [scheduled, uncertain]), [forecast, uncertain]);
  assert.deepEqual(mergeArrivals([], [scheduled]), [scheduled]);
});
