import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const workerSource = await readFile(new URL('../server/worker.js', import.meta.url), 'utf8');
const { NetplayRoom } = await import(`data:text/javascript;base64,${Buffer.from(workerSource).toString('base64')}`);
const clientSource = await readFile(new URL('../web/netplay.js', import.meta.url), 'utf8');

class Socket {
  attachment = { slot: -1, name: '', version: '', rtt: 0 };
  sent = [];
  serializeAttachment(value) { this.attachment = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.attachment); }
  send(value) { this.sent.push(JSON.parse(value)); }
  close() { this.closed = true; }
}

function roomFixture() {
  const values = new Map();
  const sockets = [];
  const ctx = {
    getWebSockets: () => sockets,
    storage: {
      get: async (key) => structuredClone(values.get(key)),
      put: async (key, value) => { values.set(key, structuredClone(value)); },
      deleteAll: async () => values.clear(),
      setAlarm: async () => {},
    },
  };
  let room = new NetplayRoom(ctx);
  room.where = async () => 'TEST';
  return {
    values, sockets,
    get room() { return room; },
    wake() { room = new NetplayRoom(ctx); room.where = async () => 'TEST'; },
    socket() { const ws = new Socket(); sockets.push(ws); return ws; },
    async join(name) {
      const ws = this.socket();
      await room.webSocketMessage(ws, JSON.stringify({ t: 'hello', name, version: 'same-build' }));
      return ws;
    },
    async message(ws, msg) { await room.webSocketMessage(ws, JSON.stringify(msg)); },
  };
}

async function runningRoom() {
  const f = roomFixture();
  f.host = await f.join('Host');
  f.guest = await f.join('Guest');
  await f.message(f.host, { t: 'start', delay: 6 });
  return f;
}

const input = (slot) => [slot, 60, 1, 0, 0, 0, 0, 16667];
const hash = (slot) => [slot, 60, 123, 456];

test('a room ticket and a slot number cannot take over a disconnected player', async () => {
  for (const token of [undefined, '', 'wrong-token']) {
    const f = await runningRoom();
    await f.room.left(f.host);
    const attacker = f.socket();
    await f.message(attacker, { t: 'rejoin', slot: 0, token });
    assert.equal(attacker.attachment.slot, -1);
    assert.equal(attacker.closed, true);
    assert.ok(f.values.get('away')[0], 'failed authentication must preserve the owner reservation');
  }
});

test('the owner can rejoin after hibernation, and the token stays private', async () => {
  const f = await runningRoom();
  const joined = f.host.sent.find((m) => m.t === 'joined');
  assert.ok(joined?.token, 'joining must issue a private reconnect token');
  assert.notEqual(joined.token, f.guest.sent.find((m) => m.t === 'joined')?.token);
  assert.ok(!JSON.stringify(f.guest.sent).includes(joined.token));
  await f.room.left(f.host);
  f.wake();
  const owner = f.socket();
  await f.message(owner, { t: 'rejoin', slot: 0, token: joined.token });
  assert.equal(owner.attachment.slot, 0);
  assert.ok(owner.sent.some((m) => m.t === 'rejoined'));
});

test('a reconnect token expires with the grace period even before the alarm runs', async () => {
  const f = await runningRoom();
  const token = f.host.sent.find((m) => m.t === 'joined')?.token;
  await f.room.left(f.host);
  const away = f.values.get('away');
  away[0].at = Date.now() - 16000;
  const attacker = f.socket();
  await f.message(attacker, { t: 'rejoin', slot: 0, token });
  assert.equal(attacker.attachment.slot, -1);
});

test('the relay rejects another player\'s input or hash, including mixed batches', async () => {
  for (const packet of [
    { t: 'in', d: [input(0)], h: [] },
    { t: 'in', d: [input(1), input(0)], h: [] },
    { t: 'in', d: [], h: [hash(0)] },
  ]) {
    const f = await runningRoom();
    f.host.sent.length = 0;
    await f.message(f.guest, packet);
    assert.equal(f.host.sent.filter((m) => m.t === 'in').length, 0);
  }
});

test('legitimate input and hash batches are relayed', async () => {
  const f = await runningRoom();
  await f.message(f.guest, { t: 'in', d: [input(1)], h: [hash(1)] });
  const packet = f.host.sent.find((m) => m.t === 'in');
  assert.deepEqual(packet.d, [input(1)]);
  assert.deepEqual(packet.h, [hash(1)]);
});

test('malformed JSON values cannot throw out of the room handler', async () => {
  const f = await runningRoom();
  for (const msg of [null, [], 3, { t: 'in', d: {} }, { t: 'in', d: [null] }]) {
    await assert.doesNotReject(f.message(f.guest, msg));
  }
});

function clientFixture() {
  const sockets = [], channels = [], timeouts = [];
  class WebSocket {
    readyState = 1;
    sent = [];
    constructor() { sockets.push(this); }
    send(value) { this.sent.push(JSON.parse(value)); }
    close() {}
    receive(msg) { this.onmessage({ data: JSON.stringify(msg) }); }
  }
  class RTCPeerConnection {
    createDataChannel() { const dc = { readyState: 'open', send() {} }; channels.push(dc); return dc; }
    createOffer() { return new Promise(() => {}); }
    close() {}
  }
  const sandbox = {
    self: {}, WebSocket, RTCPeerConnection, console,
    setInterval: () => 1, clearInterval() {},
    setTimeout: (fn) => { timeouts.push(fn); return 1; },
  };
  vm.runInNewContext(clientSource, sandbox);
  const delivered = [];
  const client = sandbox.self.RcNetplay.connect({ url: 'ws://local.invalid/netplay/TEST' });
  const ws = sockets[0];
  ws.onopen();
  ws.receive({ t: 'joined', token: 'owner-only-token' });
  ws.receive({ t: 'lobby', you: 0, players: [{ slot: 0 }, { slot: 1 }] });
  client.attach({
    _netplay_remote_input: (...args) => delivered.push(['d', ...args]),
    _netplay_remote_hash: (...args) => delivered.push(['h', ...args]),
  });
  return { ws, sockets, channels, timeouts, delivered, client };
}

test('direct channels only deliver the connected peer\'s input and hashes', () => {
  const f = clientFixture();
  const send = (msg) => f.channels[0].onmessage({ data: JSON.stringify(msg) });
  send({ t: 'in', d: [input(0)], h: [hash(0)] });
  assert.equal(f.delivered.length, 0);
  send({ t: 'in', d: [input(1), input(0)], h: [] });
  assert.equal(f.delivered.length, 0);
  send({ t: 'in', d: [input(1)], h: [hash(1)] });
  assert.equal(f.delivered.length, 2);
});

test('the client presents its private token when reconnecting', async () => {
  const f = clientFixture();
  f.ws.receive({ t: 'start', players: 2, you: 0, delay: 6 });
  f.client.testDrop();
  f.timeouts.shift()();
  await Promise.resolve();
  f.sockets[1].onopen();
  const rejoin = f.sockets[1].sent.find((m) => m.t === 'rejoin');
  assert.equal(rejoin.token, 'owner-only-token');
});
