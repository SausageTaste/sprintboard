import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { setImmediate } from 'node:timers/promises';
import test from 'node:test';
import ts from 'typescript';

// Run the production scheduler without a browser or an extra test framework.
const source = await readFile(new URL('../src/func/galleryPolling.ts', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 },
});
const { watchGalleryUpdates } = await import(`data:text/javascript;base64,${Buffer.from(outputText).toString('base64')}`);

function setup(t, overrides = {}) {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let now = 0;
    t.mock.method(performance, 'now', () => now);
    const state = { hidden: false, lightbox: false, loading: false, generation: 1 };
    const visits = t.mock.fn(async () => ({ firstPageOnly: true }));
    const refreshes = t.mock.fn(async () => {});
    const errors = t.mock.fn();
    const stop = watchGalleryUpdates({
        isPaused: () => state.hidden || state.lightbox || state.loading,
        getRequestGeneration: () => state.generation,
        visit: visits,
        refresh: refreshes,
        onError: errors,
        ...overrides,
    });
    t.after(stop);
    const tick = async (milliseconds = 2000) => {
        now += milliseconds;
        t.mock.timers.tick(milliseconds);
        await setImmediate();
    };
    const advance = async (polls) => {
        for (let i = 0; i < polls; ++i)
            await tick();
    };
    return { state, visits, refreshes, errors, stop, tick, advance };
}

test('visits occur every 15 seconds even with an unchanged index', async t => {
    const h = setup(t);
    await h.advance(7);
    assert.equal(h.visits.mock.callCount(), 0);
    assert.equal(h.refreshes.mock.callCount(), 7);
    await h.tick();
    assert.equal(h.visits.mock.callCount(), 1);
    // The visit's first-page response is not passed to page replacement.
    const context = h.refreshes.mock.calls.at(-1).arguments[0];
    assert.deepEqual(Object.keys(context).sort(), ['isCurrent', 'signal']);
    await h.advance(7);
    assert.equal(h.visits.mock.callCount(), 1);
    await h.tick();
    assert.equal(h.visits.mock.callCount(), 2);
});

for (const pause of ['hidden', 'lightbox', 'loading']) {
    test(`${pause} pauses visits and updates, then resumes on the next poll`, async t => {
        const h = setup(t);
        h.state[pause] = true;
        await h.advance(9);
        assert.equal(h.visits.mock.callCount(), 0);
        assert.equal(h.refreshes.mock.callCount(), 0);
        h.state[pause] = false;
        await h.tick();
        assert.equal(h.visits.mock.callCount(), 1);
        assert.equal(h.refreshes.mock.callCount(), 1);
    });
}

test('navigation during a visit prevents refreshing the old page', async t => {
    let finishVisit;
    const h = setup(t, { visit: () => new Promise(resolve => { finishVisit = resolve; }) });
    await h.advance(8);
    const before = h.refreshes.mock.callCount();
    h.state.generation++;
    finishVisit();
    await setImmediate();
    assert.equal(h.refreshes.mock.callCount(), before);
});

test('in-flight refresh guards reject navigation, hidden tabs, lightboxes and disposal', async t => {
    const h = setup(t);
    await h.tick();
    const context = h.refreshes.mock.calls[0].arguments[0];
    assert.equal(context.isCurrent(), true);
    for (const pause of ['hidden', 'lightbox', 'loading']) {
        h.state[pause] = true;
        assert.equal(context.isCurrent(), false);
        h.state[pause] = false;
    }
    h.state.generation++;
    assert.equal(context.isCurrent(), false);
    await h.tick();
    const nextContext = h.refreshes.mock.calls.at(-1).arguments[0];
    assert.equal(nextContext.isCurrent(), true);
    h.stop();
    assert.equal(nextContext.isCurrent(), false);
    assert.equal(nextContext.signal.aborted, true);
    const before = h.refreshes.mock.callCount();
    await h.advance(10);
    assert.equal(h.refreshes.mock.callCount(), before);
});

test('slow visits never overlap and disposal aborts the request', async t => {
    let finishVisit;
    let signal;
    const visit = t.mock.fn(s => {
        signal = s;
        return new Promise(resolve => { finishVisit = resolve; });
    });
    const h = setup(t, { visit });
    await h.advance(8);
    await h.advance(10);
    assert.equal(visit.mock.callCount(), 1);
    const before = h.refreshes.mock.callCount();
    h.stop();
    assert.equal(signal.aborted, true);
    finishVisit();
    await setImmediate();
    await h.advance(10);
    assert.equal(h.refreshes.mock.callCount(), before);
    assert.equal(visit.mock.callCount(), 1);
});

test('failed visits retry on the next poll without advancing the visit clock', async t => {
    let attempts = 0;
    const visit = t.mock.fn(async () => {
        if (++attempts === 1)
            throw new Error('temporary failure');
    });
    const h = setup(t, { visit });
    await h.advance(8);
    assert.equal(h.errors.mock.callCount(), 1);
    await h.tick();
    assert.equal(visit.mock.callCount(), 2);
    await h.tick();
    assert.equal(visit.mock.callCount(), 2);
});
