#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { randomUUID } = require('node:crypto');

function returnEntry(romfs) {
  const u64 = at => {
    const value = romfs.readBigUInt64LE(at);
    assert(value <= BigInt(romfs.length), 'Invalid RomFS offset or size.');
    return Number(value);
  };
  assert(u64(0) === 80, 'Unexpected RomFS header.');
  const dirs = u64(24), dirsSize = u64(32), files = u64(56), filesSize = u64(64), data = u64(72);
  assert(dirsSize >= 24 && dirs + dirsSize <= romfs.length && files + filesSize <= romfs.length);
  const entry = files + romfs.readUInt32LE(dirs + 12);
  assert(entry + 32 <= files + filesSize, 'Missing return file in the RomFS root.');
  const nameSize = romfs.readUInt32LE(entry + 28);
  assert(entry + 32 + nameSize <= files + filesSize);
  assert.equal(romfs.toString('ascii', entry + 32, entry + 32 + nameSize), 'return-iz2s.nro');
  const start = data + u64(entry + 8), size = u64(entry + 16);
  assert(start + size <= romfs.length, 'Incomplete return file.');
  return romfs.subarray(start, start + size);
}

async function build(platform, input, launcher, output) {
  const platformRoot = path.resolve(platform);
  const archive = require(path.join(platformRoot, 'core/archive.cjs'));
  const nro = require(path.join(platformRoot, 'engines/iz2s-core/tools/nro-package.cjs'));
  const project = path.resolve(__dirname, '../..');
  const [program, companion, readme, license] = await Promise.all([
    fs.readFile(input), fs.readFile(launcher), fs.readFile(path.join(project, 'README.md')),
    fs.readFile(path.join(project, 'LICENSE')),
  ]);
  async function meta(image) {
    assert(nro.switch2Program(image), 'Expected a Switch 2 Ounce NRO.');
    return nro.metadata(image.length, (at, size) => image.subarray(at, at + size));
  }
  const {name, version} = await meta(program);
  assert(name === 'IZtwiNS2' && version, 'Expected a versioned IZtwiNS2 build.');
  assert.equal((await meta(companion)).name, 'IZ2S Launcher');

  // The physical SD importer installs the embedded NRO directly. Its own
  // RomFS must therefore hold the exact return entry from the IZ2S release.
  const imageSize = program.readUInt32LE(24);
  const offset = program.readBigUInt64LE(imageSize + 40);
  const size = program.readBigUInt64LE(imageSize + 48);
  assert(offset >= 56n && size >= BigInt(companion.length) &&
      offset + size <= BigInt(program.length - imageSize), 'Missing bundled launcher resources.');
  const romfs = program.subarray(imageSize + Number(offset), imageSize + Number(offset + size));
  assert(returnEntry(romfs).equals(companion),
    'Rebuild the NRO with IZTWINS2_RETURN_NRO set to the same launcher entry.');

  const id = 'iztwins2';
  const manifest = {
    format: 'izlauncher.nro-app', abi: 1, target: 'switch2', id, name, version, program: 'launcher.nro',
  };
  const descriptor = {
    format: 'izlauncher.module', formatVersion: 1, apiVersion: 1, id, name, version,
    author: 'IZtwiNS2 contributors', platforms: [], requires: { platformApi: 1 },
    switch: { abi: 1, entry: 'switch/app.json', files: ['switch/app.json', 'switch/launcher.nro'] },
  };
  const json = value => Buffer.from(JSON.stringify(value, null, 2) + '\n');
  const files = [
    { name: 'module.json', bytes: json(descriptor) },
    { name: 'switch/app.json', bytes: json(manifest) },
    { name: 'switch/launcher.nro', bytes: program },
    { name: 'README.md', bytes: readme },
    { name: 'LICENSE', bytes: license },
  ];
  const checksums = Object.fromEntries(files.map(file => [file.name, archive.sha(file.bytes)]));
  const bytes = archive.encode([...files, {
    name: 'checksums.json', bytes: json({ algorithm: 'sha256', files: checksums }),
  }]);
  const checked = archive.inspect(bytes);
  assert((await nro.moduleProgram(checked, archive.switchApp(checked))).equals(program));

  const target = path.resolve(output);
  await fs.mkdir(path.dirname(target), { recursive: true });
  const temporary = target + '.' + randomUUID() + '.tmp';
  try {
    await fs.writeFile(temporary, bytes, { flag: 'wx' });
    await fs.rename(temporary, target);
  } finally {
    await fs.rm(temporary, { force: true });
  }
  console.log(target);
}

if (process.argv.length !== 6) {
  console.error('Usage: node build-nro-bundle.cjs IZ2S_PLATFORM_ROOT IZtwiNS2.nro IZ2S.nro OUTPUT.zip');
  process.exitCode = 2;
} else build(...process.argv.slice(2)).catch(error => {
  console.error(error.message);
  process.exitCode = 1;
});
