// The WebRTC stack's crypto (platform/src/rtc) against Node's, which is
// OpenSSL's: random inputs every run, checked by rtc_vectors.c, and our
// signatures checked here in turn.
//
//   node rtc_vectors.mjs <purr_platform_rtc_vectors> <scratch folder> [cases of each kind]

import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [program, scratch, countText] = process.argv.slice(2);
const count = Number(countText || 200);
mkdirSync(scratch, { recursive: true });
const casesPath = resolve(scratch, 'rtc_cases.txt');
const signedPath = resolve(scratch, 'rtc_signed.txt');

const hex = b => (b.length ? Buffer.from(b).toString('hex') : '-');
const random = n => crypto.randomBytes(n);
const size = most => crypto.randomInt(most + 1);
const lines = [];

function prf(secret, label, seed, n) {
    const s = Buffer.concat([Buffer.from(label), seed]);
    let a = crypto.createHmac('sha256', secret).update(s).digest();
    const parts = [];
    for (let got = 0; got < n; got += 32) {
        parts.push(crypto.createHmac('sha256', secret).update(Buffer.concat([a, s])).digest());
        a = crypto.createHmac('sha256', secret).update(a).digest();
    }
    return Buffer.concat(parts).subarray(0, n);
}

// P-256 keys from a random private key, as Node makes them
function keys() {
    const ecdh = crypto.createECDH('prime256v1');
    ecdh.generateKeys();
    const d = ecdh.getPrivateKey();
    const pub = ecdh.getPublicKey();
    const jwk = { kty: 'EC', crv: 'P-256', x: pub.subarray(1, 33).toString('base64url'), y: pub.subarray(33).toString('base64url') };
    // Node drops leading zeros of the private key: put them back
    const priv = Buffer.concat([Buffer.alloc(32 - d.length), d]);
    return {
        ecdh, priv, pub,
        privateKey: crypto.createPrivateKey({ key: { ...jwk, d: priv.toString('base64url') }, format: 'jwk' }),
        publicKey: crypto.createPublicKey({ key: jwk, format: 'jwk' }),
    };
}

for (let i = 0; i < count; i++) {
    const data = random(size(400));
    for (const [kind, name] of [['sha256', 'sha256'], ['sha1', 'sha1'], ['md5', 'md5']]) {
        lines.push(`${kind} ${hex(data)} ${hex(crypto.createHash(name).update(data).digest())}`);
    }
    const key = random(1 + size(130));
    lines.push(`hmac256 ${hex(key)} ${hex(data)} ${hex(crypto.createHmac('sha256', key).update(data).digest())}`);
    lines.push(`hmac1 ${hex(key)} ${hex(data)} ${hex(crypto.createHmac('sha1', key).update(data).digest())}`);
    const secret = random(1 + size(64)), seed = random(size(80)), label = 'label ' + i;
    lines.push(`prf ${hex(secret)} ${hex(Buffer.from(label))} ${hex(seed)} ${hex(prf(secret, label, seed, 1 + size(120)))}`);

    const aeadKey = random(32), nonce = random(12), ad = random(size(40)), text = random(size(700));
    const cipher = crypto.createCipheriv('chacha20-poly1305', aeadKey, nonce, { authTagLength: 16 });
    cipher.setAAD(ad);
    const sealed = Buffer.concat([cipher.update(text), cipher.final(), cipher.getAuthTag()]);
    lines.push(`seal ${hex(aeadKey)} ${hex(nonce)} ${hex(ad)} ${hex(text)} ${hex(sealed)}`);

    const a = keys(), b = keys();
    lines.push(`public ${hex(a.priv)} ${hex(a.pub)}`);
    lines.push(`ecdh ${hex(a.priv)} ${hex(b.pub)} ${hex(a.ecdh.computeSecret(b.pub))}`);
    const message = random(size(200));
    const sig = crypto.sign('sha256', message, { key: a.privateKey, dsaEncoding: 'ieee-p1363' });
    lines.push(`verify ${hex(a.pub)} ${hex(message)} ${hex(sig)} 1`);
    const bad = Buffer.from(sig);
    bad[crypto.randomInt(64)] ^= 1 << crypto.randomInt(8);
    lines.push(`verify ${hex(a.pub)} ${hex(message)} ${hex(bad)} 0`);
    lines.push(`verify ${hex(b.pub)} ${hex(message)} ${hex(sig)} 0`); // Someone else's key
    lines.push(`sign ${hex(a.priv)} ${hex(message)}`);
}
writeFileSync(casesPath, lines.join('\n') + '\n');

const run = spawnSync(program, [casesPath, signedPath], { encoding: 'utf8' });
process.stdout.write(run.stdout || '');
process.stderr.write(run.stderr || '');
let failed = run.status !== 0;

// Our signatures, checked by Node
let signatures = 0, wrong = 0;
for (const line of readFileSync(signedPath, 'utf8').split('\n')) {
    const [kind, pub, message, sig] = line.split(' ');
    if (kind !== 'signed') continue;
    signatures++;
    const key = Buffer.from(pub, 'hex');
    const publicKey = crypto.createPublicKey({ key: { kty: 'EC', crv: 'P-256',
        x: key.subarray(1, 33).toString('base64url'), y: key.subarray(33).toString('base64url') }, format: 'jwk' });
    const data = message === '-' ? Buffer.alloc(0) : Buffer.from(message, 'hex');
    if (!crypto.verify('sha256', data, { key: publicKey, dsaEncoding: 'ieee-p1363' }, Buffer.from(sig, 'hex'))) wrong++;
}
console.log(`${signatures} of our signatures, ${wrong} Node didn't accept`);
if (wrong || signatures !== count) failed = true;
process.exit(failed ? 1 : 0);
