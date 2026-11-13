const assert = require('node:assert/strict');
const test = require('node:test');
const transport = require('../src/pkjs/transport');
const create = require('../src/pkjs/prefetch');

const settle = () => new Promise(resolve => setImmediate(resolve));

test('prefetch bounds concurrency, shares the active request and prioritizes Select', async t => {
  const requests = [];
  const calls = [];
  const stops = Array.from({length: 8}, (_, i) => [String(i)]);
  t.mock.method(transport, 'forecast', (stop, trigger) => {
    calls.push([stop[0], trigger]);
    return new Promise(resolve => requests.push(() => resolve({
      rows: [{route: '42', live: true, time: 2000}], expires: 1200, internalId: 'id'
    })));
  });
  t.mock.method(Date, 'now', () => 1000000);
  let finishSchedule;
  t.mock.method(transport, 'planned', () => new Promise(resolve => { finishSchedule = resolve; }));
  const cache = create(() => {});
  cache.reset(stops);
  cache.enqueue([stops[1], stops[6]]);
  assert.deepEqual(calls, [['0','prefetch'], ['1','prefetch']]);

  let intermediate;
  const board = cache.board(stops[1], 'manual', true, result => { intermediate = result; });
  requests[1]();
  await settle();
  assert.equal(intermediate.rows[0].route, '42');
  assert.equal(calls.length, 2);
  finishSchedule({ rows: [{route: '7', live: false, time: 1500}], failed: false });
  assert.deepEqual((await board).rows.map(row => row.route), ['7','42']);

  cache.reset([stops[7]]);
  cache.close();
  assert.deepEqual(calls.at(-1), ['7','prefetch']);
});

test('only a stationary selection without predictions loads background schedule', async t => {
  t.mock.timers.enable({apis: ['setTimeout']});
  t.mock.method(transport, 'forecast', async () => ({
    rows: [], expires: Math.floor(Date.now() / 1000) + 120, internalId: 'id'
  }));
  const planned = t.mock.method(transport, 'planned', async () => ({rows: [], failed: false}));
  const cache = create(() => {});
  const a = ['a'], b = ['b'];
  cache.reset([a,b]);
  cache.select(a);
  await settle();
  t.mock.timers.tick(500);
  cache.select(b);
  t.mock.timers.tick(749);
  assert.equal(planned.mock.callCount(), 0);
  t.mock.timers.tick(1);
  assert.equal(planned.mock.callCount(), 1);
  assert.equal(planned.mock.calls[0].arguments[0], b);
  await settle();
});

test('expired forecasts disappear while future schedule remains', async t => {
  let time = 1000;
  t.mock.method(Date, 'now', () => time * 1000);
  t.mock.method(transport, 'forecast', async () => ({
    rows: [{ route: '42', time: 1300, live: true }], expires: 1120, internalId: 'id'
  }));
  t.mock.method(transport, 'planned', async () => ({
    rows: [{ route: '7', time: 1400, live: false }], failed: false
  }));
  const cache = create(() => {});
  const stop = ['stop'];
  await cache.board(stop, 'manual', false, () => {});
  time = 1120;
  assert.deepEqual(cache.snapshot(stop).rows.map(row => row.route), ['7']);
  time = 1400;
  assert.deepEqual(cache.snapshot(stop).rows, []);
});

test('foreground and background forecasts share the server pause', async t => {
  let requests = 0;
  t.mock.method(console, 'log', () => {});
  t.mock.method(Date, 'now', () => 1000000);
  const old = global.XMLHttpRequest;
  global.XMLHttpRequest = class {
    open() {}
    getResponseHeader() { return '120'; }
    send() {
      requests++;
      this.status = 429;
      this.onload();
    }
  };
  try {
    const cache = create(() => {});
    cache.reset([['first'], ['second']]);
    await settle();
    await assert.rejects(cache.board(['second'], 'manual', true, () => {}), /retry-after/);
    assert.equal(requests, 1);
  } finally {
    global.XMLHttpRequest = old;
  }
});
