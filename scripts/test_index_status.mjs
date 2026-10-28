import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate } from 'node:timers/promises';
import { watchInitialIndexStatus } from '../src/func/indexStatus.ts';

test('startup polling retries failures, reports completion, and stops', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const responses = ['pending', 'running', 'failed', 'ready'];
    const states = [];
    const fetch = t.mock.method(globalThis, 'fetch', async () =>
        new Response(JSON.stringify({ state: responses.shift() })));
    const stop = watchInitialIndexStatus(state => states.push(state));
    t.after(stop);
    await setImmediate();
    assert.deepEqual(states, ['pending']);
    t.mock.timers.tick(2000);
    await setImmediate();
    assert.deepEqual(states, ['pending', 'running']);
    t.mock.timers.tick(2000);
    await setImmediate();
    assert.equal(states.at(-1), 'failed');
    t.mock.timers.tick(4999);
    assert.equal(fetch.mock.callCount(), 3);
    t.mock.timers.tick(1);
    await setImmediate();
    assert.equal(states.at(-1), 'ready');
    t.mock.timers.tick(60000);
    assert.equal(fetch.mock.callCount(), 4);
});

test('network errors retry and cleanup cancels the next poll', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const states = [];
    const fetch = t.mock.method(globalThis, 'fetch', async () => { throw new Error('offline'); });
    const stop = watchInitialIndexStatus(state => states.push(state));
    await setImmediate();
    assert.deepEqual(states, ['failed']);
    t.mock.timers.tick(5000);
    await setImmediate();
    assert.equal(fetch.mock.callCount(), 2);
    stop();
    assert.equal(fetch.mock.calls[0].arguments[1].signal.aborted, true);
    t.mock.timers.tick(5000);
    assert.equal(fetch.mock.callCount(), 2);
});

test('unmount ignores a late response and does not schedule another request', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let resolve;
    const fetch = t.mock.method(globalThis, 'fetch', () => new Promise(r => { resolve = r; }));
    const states = [];
    const stop = watchInitialIndexStatus(state => states.push(state));
    stop();
    resolve(new Response(JSON.stringify({ state: 'ready' })));
    await setImmediate();
    assert.deepEqual(states, []);
    t.mock.timers.tick(60000);
    assert.equal(fetch.mock.callCount(), 1);
});
