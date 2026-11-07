import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import ts from 'typescript';

const source = await readFile(new URL('../src/widgets/GenerationMetadata.tsx', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022, jsx: ts.JsxEmit.ReactJSX },
});
const code = outputText.replace('"react/jsx-runtime"', JSON.stringify(import.meta.resolve('react/jsx-runtime')));
const { default: GenerationMetadata } = await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}`);
const value = (text, node) => ({ value: text, node, input: 'text' });
const fixture = () => ({
    source: 'avif.exif.Model', status: 'resolved', issues: [], unclassified: [],
    stages: [{ output: '25', sampler: '18:7', models: [value('model.safetensors', '18:1')], positive: [value('positive text', '18:17')], negative: [value('negative text', '18:5')] }],
});
const render = info => renderToStaticMarkup(createElement(GenerationMetadata, { info, legacyModel: 'old model', legacyPrompts: ['old prompt'] }));

test('shows model, separate prompt roles, and expandable provenance', () => {
    const html = render(fixture());
    for (const text of ['model.safetensors', 'Positive Prompt', 'positive text', 'Negative Prompt', 'negative text', 'Source nodes', 'node 18:17', 'avif.exif.Model'])
        assert.ok(html.includes(text));
    assert.ok(html.includes('<details>'));
    assert.ok(!html.includes('old prompt'));
});

test('distinguishes empty prompts, unknown values, and unclassified text', () => {
    const info = fixture();
    info.status = 'partial';
    info.stages[0].positive = [];
    info.stages[0].negative = [value('', '18:5')];
    info.issues.push({ node: 'generator', input: 'text', reason: 'Unsupported node: TextGenerate' });
    info.unclassified = [value('<script>unclassified</script>', 'unknown')];
    const html = render(info);
    for (const text of ['Some values could not be resolved', 'Not resolved', '(empty)', 'Unsupported node: TextGenerate', 'Unclassified Text', 'not searchable'])
        assert.ok(html.includes(text));
    assert.ok(html.includes('&lt;script&gt;unclassified&lt;/script&gt;'));
    assert.ok(!html.includes('<script>'));
});

test('groups ambiguous outputs and multiple sampling stages', () => {
    const info = fixture();
    info.status = 'ambiguous';
    info.stages.push({ ...info.stages[0], sampler: 'another sampler' });
    info.stages.push({ ...info.stages[0], output: 'another output' });
    const html = render(info);
    for (const text of ['excluded from search', 'Saved output 1', 'Saved output 2', 'Sampling stage 1', 'Sampling stage 2'])
        assert.ok(html.includes(text));
});

test('supports unresolved metadata and older server responses', () => {
    const info = fixture();
    info.status = 'unresolved';
    info.stages = [];
    assert.ok(render(info).includes('Generation values could not be resolved'));
    const html = render(undefined);
    assert.ok(html.includes('old model') && html.includes('old prompt') && html.includes('SD Prompt'));
});
