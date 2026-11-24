#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');

async function run(platform, staging, output) {
    const archive = require(path.join(platform, 'core/archive.cjs'));
    const pkg = require(path.join(platform, 'core/console/sd-launcher/package.cjs'));
    const installer = require(path.join(platform, 'engines/iz2s-core/tools/prepare-sd-app.cjs'));
    const engine = await fs.readFile(path.join(staging, 'engine.izdata'));
    const sha256 = archive.sha(engine);
    const file = 'asset-' + sha256 + '.izdata';
    const asset = {file, path: file, bytes: engine.length, sha256};
    const manifest = {
        format: 'hblauncher.gpu-app', abi: 1, id: 'iztwins2', name: 'IZtwiNS2', version: '0.11.0',
        description: 'Twitch live streams, videos, clips and chat',
        transition: 'none', category: 'media', sdOnly: true, icon: 'icon.png',
        shaders: {vertex: 'app.vert.spv', fragment: 'app.frag.spv', compute: 'app.comp.spv'},
        initialState: 'state.bin', data: 'data.bin',
        native: {
            format: 'iz2s.native-app', abi: 7, capabilities: ['audio-out', 'cooperative-image'],
            code: 'native.bin', initial: 'native-data.bin', dataBytes: 4096, assets: [asset],
        },
        memory: {bytes: 12 * 1024 * 1024, initial: 'pixels.bin'},
    };
    await Promise.all([
        fs.writeFile(path.join(staging, file), engine),
        fs.writeFile(path.join(staging, 'native-data.bin'), Buffer.from(file + '\0')),
        fs.writeFile(path.join(staging, 'state.bin'), Buffer.alloc(1024)),
        fs.writeFile(path.join(staging, 'data.bin'), Buffer.alloc(4)),
        fs.writeFile(path.join(staging, 'pixels.bin'), Buffer.alloc(4)),
        fs.writeFile(path.join(staging, 'app.json'), JSON.stringify(manifest, null, 2) + '\n'),
    ]);
    const built = await pkg.build(staging, output, {sb4: true});
    const header = pkg.decode(built.bytes);
    assert.equal(header.nativeAbi, 7);
    assert.equal(header.assetManifest.count, 1);
    assert(header.sections.nativeCode.length > 0);
    for (const stage of ['vertex', 'fragment', 'compute']) assert(header.sections[stage].length > 20);
    const verification = await installer.prepare(built.output, output);
    try {
        assert.equal(verification.sha256, built.sha256);
        assert.equal(verification.abi, 7);
        assert.equal(verification.assets, 1);
        assert.equal(archive.sha(await fs.readFile(path.join(verification.folder, file))), sha256);
        const portable = await installer.portableApp(verification);
        assert(portable.packed);
        assert(archive.decode(portable.bytes).every(item => !item.name.endsWith('.nro')));
        await fs.writeFile(path.join(output, 'IZtwiNS2-0.11.0.iza'), portable.bytes);
        console.log(JSON.stringify({abi: header.nativeAbi, launcher: built.sha256, engine: sha256}));
    } finally {
        await fs.rm(verification.folder, {recursive: true, force: true});
    }
}

const [, , platform, staging, output] = process.argv;
if (!output) throw new Error('Usage: package.cjs <IZ2S platform> <staging directory> <output directory>');
run(path.resolve(platform), path.resolve(staging), path.resolve(output)).catch(error => {
    console.error(error);
    process.exitCode = 1;
});
