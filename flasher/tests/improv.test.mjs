// The firmware's Improv code (components/improv, as host/improv_host serve)
// against the client library the web installer itself uses
// (improv-wifi-serial-sdk, the version esp-web-tools pins), over a pipe that
// stands in for the serial port. The emulated board writes log lines between
// its packets, as the real one does on its console UART.
//   make -C host improv_host && cd flasher && npm test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { register } from 'node:module';
import { fileURLToPath } from 'node:url';

// the SDK's dist imports "./util/hex-formatter" without an extension, which
// browsers' bundlers accept and node does not
register('data:text/javascript,' + encodeURIComponent(`
export async function resolve(spec, ctx, next) {
  try { return await next(spec, ctx); }
  catch (e) { if (spec.startsWith('.') && !spec.endsWith('.js')) return next(spec + '.js', ctx); throw e; }
}`));
const { ImprovSerial } = await import('improv-wifi-serial-sdk/dist/serial.js');
const { ImprovSerialCurrentState } = await import('improv-wifi-serial-sdk/dist/const.js');

const HOST = fileURLToPath(new URL('../../host/improv_host', import.meta.url));

// a Web Serial port's two streams on the emulated board's stdin / stdout
function board() {
    const proc = spawn(HOST, ['serve'], { stdio: ['pipe', 'pipe', 'inherit'] });
    let log = '';
    let open = true;
    const readable = new ReadableStream({
        start(c) {
            proc.stdout.on('data', (b) => { log += b.toString('latin1'); if (open) c.enqueue(new Uint8Array(b)); });
            proc.stdout.on('end', () => { if (open) c.close(); open = false; });
        },
        cancel() { open = false; },    // client.close()
    });
    const writable = new WritableStream({ write: (chunk) => { proc.stdin.write(Buffer.from(chunk)); } });
    return { port: { readable, writable }, proc, log: () => log };
}

const quiet = { log() {}, debug() {}, error() {} };

test('the installer sets up the WiFi through the firmware', async () => {
    const b = board();
    const client = new ImprovSerial(b.port, quiet);
    try {
        // detection right after flashing, then the device info
        const info = await client.initialize(3000);
        assert.deepEqual(
            { firmware: info.firmware, version: info.version, chipFamily: info.chipFamily, name: info.name },
            { firmware: 'ESP32 USB to AirPrint', version: '0.2.0-test', chipFamily: 'ESP32-S3', name: 'Canon MG2500 (ESP32)' });
        assert.equal(client.state, ImprovSerialCurrentState.READY);
        assert.equal(client.nextUrl, undefined);

        // the network list (the SDK sorts it by name); the 10-byte result for "x" survives
        const ssids = await client.scan(5000);
        assert.deepEqual(ssids, [
            { name: 'Cafe', rssi: -81, secured: false },
            { name: 'HomeNet', rssi: -48, secured: true },
            { name: 'x', rssi: -9, secured: false },
        ]);

        // a wrong password is reported, and the board can be asked again
        await assert.rejects(client.provision('HomeNet', 'wrong', 5000), /UNABLE_TO_CONNECT/);
        assert.equal(client.state, ImprovSerialCurrentState.READY);

        await client.provision('HomeNet', 'correct horse', 5000);
        assert.equal(client.state, ImprovSerialCurrentState.PROVISIONED);
        assert.equal(client.nextUrl, 'http://192.168.1.20/');

        // what the installer asks when it is opened again on a set-up board
        await client.requestCurrentState(3000);
        assert.equal(client.state, ImprovSerialCurrentState.PROVISIONED);
        assert.equal(client.nextUrl, 'http://192.168.1.20/');
        assert.match(b.log(), /tick \d+/);     // log lines went by in between
    } finally {
        await client.close();
        b.proc.stdin.end();
        b.proc.kill();
    }
});
