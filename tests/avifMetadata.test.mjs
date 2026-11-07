import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import ts from 'typescript';

const source = await readFile(new URL('../src/widgets/AvifMetadata.tsx', import.meta.url), 'utf8');
const { outputText } = ts.transpileModule(source, {
    compilerOptions: {
        target: ts.ScriptTarget.ES2022,
        module: ts.ModuleKind.ES2022,
        jsx: ts.JsxEmit.ReactJSX,
    },
});
const moduleSource = outputText.replace('"react/jsx-runtime"', JSON.stringify(import.meta.resolve('react/jsx-runtime')));
const { default: AvifMetadata } = await import(`data:text/javascript;base64,${Buffer.from(moduleSource).toString('base64')}`);
const render = metadata => renderToStaticMarkup(createElement(AvifMetadata, { metadata }));

const escapeHtml = text => text.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#x27;');

test('renders complete workflow and execution graph JSON without an empty XMP panel', () => {
    const workflow = { nodes: [], links: [], label: '한글', longValue: 'text '.repeat(10000) };
    const prompt = { '18:1': { class_type: 'UNETLoader', inputs: { unet_name: 'example.safetensors' } } };
    const html = render({ xmp: '', exif: { Make: `workflow:${JSON.stringify(workflow)}`, Model: `prompt:${JSON.stringify(prompt)}` } });
    assert.ok(html.includes('Workflow (EXIF Make)'));
    assert.ok(html.includes('Execution Graph (EXIF Model)'));
    for (const value of [workflow, prompt])
        assert.ok(html.includes(escapeHtml(JSON.stringify(value, null, 2))));
    assert.ok(!html.includes('workflow:'));
    assert.ok(!html.includes('prompt:'));
    assert.ok(!html.includes('XMP Data'));
    assert.ok(html.includes('max-height:220px;overflow:auto'));
});

test('preserves invalid JSON and ordinary EXIF values as escaped plain text', () => {
    for (const value of ['workflow:{invalid JSON', '<script>alert("test")</script>', 'prompt:{"unrecognized":"prefix for Make"}']) {
        const html = render({ xmp: '', exif: { Make: value } });
        assert.ok(html.includes(escapeHtml(value)));
        assert.ok(!html.includes('<script>'));
    }
    assert.ok(render({ xmp: '', exif: { Make: 'Camera maker' } }).includes('EXIF Make'));
});

test('supports older responses, mixed metadata, and empty metadata', () => {
    const xmp = '<x:xmpmeta>original XMP</x:xmpmeta>';
    assert.ok(render({ xmp }).includes(escapeHtml(xmp)));
    const mixed = render({ xmp, exif: { Model: 'prompt:{}' } });
    assert.ok(mixed.includes('XMP Data'));
    assert.ok(mixed.includes('Execution Graph (EXIF Model)'));
    assert.equal(render({ xmp: '' }), '');
    assert.equal(render({ xmp: '', exif: {} }), '');
});
