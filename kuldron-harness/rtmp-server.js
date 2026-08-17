#!/usr/bin/env node
/*
 * Minimal RTMP publish-receiving server for the Enhanced RTMP v2 reconnect
 * acceptance harness.
 *
 * It is deliberately hand-written rather than borrowed: the tests need to
 * assert on the exact capsEx the client declares, to emit a spec-shaped
 * ReconnectRequest on demand (including deliberately malformed ones), and to
 * observe that the client's new connection overlaps its old one. It is not a
 * general-purpose server and makes no attempt to be one.
 *
 *   node rtmp-server.js --name A --port 1935 [options]
 *
 *   --reconnect-after <sec>   emit a ReconnectRequest this long after publish
 *   --reconnect-url <url>     tcUrl to put in it (omit for "stay where you are")
 *   --reconnect-repeat        keep emitting one every --reconnect-after seconds
 *   --reconnect-level <s>     override the level (default "status")
 *   --reconnect-code <s>      override the code (default the spec's)
 *
 * Every notable thing that happens is printed to stdout as one JSON object per
 * line, so the test script can assert on a transcript rather than on timing.
 */

'use strict';

const net = require('net');
const tls = require('tls');
const fs = require('fs');

const args = process.argv.slice(2);
function opt(name, fallback) {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}
function flag(name) {
  return args.includes(`--${name}`);
}

const NAME = opt('name', 'S');
const PORT = parseInt(opt('port', '1935'), 10);
const RECONNECT_AFTER = opt('reconnect-after', null) === null ? null : parseFloat(opt('reconnect-after'));
const RECONNECT_URL = opt('reconnect-url', null);
const RECONNECT_REPEAT = flag('reconnect-repeat');
const RECONNECT_LEVEL = opt('reconnect-level', 'status');
const RECONNECT_CODE = opt('reconnect-code', 'NetConnection.Connect.ReconnectRequest');
/* Length of a junk tcUrl to send instead of a real one, to prove the client
 * bounds it rather than trusting the wire length. */
const RECONNECT_TCURL_LEN = opt('reconnect-tcurl-len', null) === null ? null : parseInt(opt('reconnect-tcurl-len'), 10);
/* Send deliberately malformed command messages alongside the real ones. */
const GARBAGE_AFTER = opt('garbage-after', null) === null ? null : parseFloat(opt('garbage-after'));

const RECONNECT_SPEC_CODE = 'NetConnection.Connect.ReconnectRequest';

let connSeq = 0;
let liveConnections = 0;

function emit(event, fields) {
  process.stdout.write(JSON.stringify({ server: NAME, t: Date.now(), event, ...fields }) + '\n');
}

/* ---------------------------------------------------------------- AMF0 --- */

function amfString(s) {
  const body = Buffer.from(s, 'utf8');
  const out = Buffer.alloc(3 + body.length);
  out.writeUInt8(0x02, 0);
  out.writeUInt16BE(body.length, 1);
  body.copy(out, 3);
  return out;
}

function amfNumber(n) {
  const out = Buffer.alloc(9);
  out.writeUInt8(0x00, 0);
  out.writeDoubleBE(n, 1);
  return out;
}

function amfNull() {
  return Buffer.from([0x05]);
}

function amfPropName(name) {
  const body = Buffer.from(name, 'utf8');
  const out = Buffer.alloc(2 + body.length);
  out.writeUInt16BE(body.length, 0);
  body.copy(out, 2);
  return out;
}

/* `props` is an array of [name, encodedValueBuffer]. */
function amfObject(props) {
  const parts = [Buffer.from([0x03])];
  for (const [name, value] of props) {
    parts.push(amfPropName(name), value);
  }
  parts.push(Buffer.from([0x00, 0x00, 0x09]));
  return Buffer.concat(parts);
}

function amfDecodeValue(buf, pos) {
  if (pos >= buf.length) throw new Error('truncated AMF');
  const type = buf.readUInt8(pos);
  pos += 1;
  switch (type) {
    case 0x00:
      return [buf.readDoubleBE(pos), pos + 8];
    case 0x01:
      return [buf.readUInt8(pos) !== 0, pos + 1];
    case 0x02: {
      const len = buf.readUInt16BE(pos);
      return [buf.toString('utf8', pos + 2, pos + 2 + len), pos + 2 + len];
    }
    case 0x03:
    case 0x08: {
      if (type === 0x08) pos += 4; /* ECMA array count */
      const obj = {};
      for (;;) {
        const nameLen = buf.readUInt16BE(pos);
        if (nameLen === 0 && buf.readUInt8(pos + 2) === 0x09) return [obj, pos + 3];
        const name = buf.toString('utf8', pos + 2, pos + 2 + nameLen);
        let value;
        [value, pos] = amfDecodeValue(buf, pos + 2 + nameLen);
        obj[name] = value;
      }
    }
    case 0x05:
      return [null, pos];
    case 0x06:
      return [undefined, pos];
    case 0x0c: {
      const len = buf.readUInt32BE(pos);
      return [buf.toString('utf8', pos + 4, pos + 4 + len), pos + 4 + len];
    }
    default:
      throw new Error(`unsupported AMF0 type 0x${type.toString(16)}`);
  }
}

function amfDecodeAll(buf) {
  const out = [];
  let pos = 0;
  while (pos < buf.length) {
    let value;
    [value, pos] = amfDecodeValue(buf, pos);
    out.push(value);
  }
  return out;
}

/* --------------------------------------------------------------- server --- */

class Session {
  constructor(socket) {
    this.id = ++connSeq;
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.state = 'c0c1';
    this.inChunkSize = 128;
    this.outChunkSize = 128;
    this.chunks = new Map();
    this.publishing = false;
    this.videoPackets = 0;
    this.keyframes = 0;
    this.audioPackets = 0;
    this.bytes = 0;
    this.reconnectTimer = null;
    liveConnections++;

    emit('connection_open', { conn: this.id, live: liveConnections, peer: socket.remoteAddress });

    socket.on('data', (d) => {
      this.bytes += d.length;
      this.buf = Buffer.concat([this.buf, d]);
      try {
        this.drain();
      } catch (err) {
        emit('parse_error', { conn: this.id, error: String(err && err.message) });
        socket.destroy();
      }
    });
    socket.on('close', () => {
      liveConnections--;
      if (this.reconnectTimer) clearInterval(this.reconnectTimer);
      if (this.garbageTimer) clearInterval(this.garbageTimer);
      emit('connection_close', {
        conn: this.id,
        live: liveConnections,
        video: this.videoPackets,
        keyframes: this.keyframes,
        audio: this.audioPackets,
        bytes: this.bytes,
      });
    });
    socket.on('error', (err) => emit('socket_error', { conn: this.id, error: String(err && err.message) }));
  }

  drain() {
    for (;;) {
      if (this.state === 'c0c1') {
        if (this.buf.length < 1537) return;
        const c1 = this.buf.subarray(1, 1537);
        this.buf = this.buf.subarray(1537);
        /* Plain handshake: S0, S1 (zero time, zero zero, random), S2 echoing
         * C1. librtmp treats a server that does not answer with a digest as a
         * plain-handshake server and continues. */
        const s0 = Buffer.from([3]);
        const s1 = Buffer.alloc(1536);
        s1.writeUInt32BE(0, 0);
        s1.writeUInt32BE(0, 4);
        for (let i = 8; i < 1536; i++) s1[i] = (i * 7) & 0xff;
        this.socket.write(Buffer.concat([s0, s1, c1]));
        this.state = 'c2';
        continue;
      }
      if (this.state === 'c2') {
        if (this.buf.length < 1536) return;
        this.buf = this.buf.subarray(1536);
        this.state = 'chunks';
        emit('handshake_done', { conn: this.id });
        continue;
      }

      const consumed = this.readChunk();
      if (!consumed) return;
    }
  }

  readChunk() {
    const buf = this.buf;
    if (buf.length < 1) return false;

    let pos = 0;
    const first = buf.readUInt8(pos++);
    const fmt = first >> 6;
    let csid = first & 0x3f;

    if (csid === 0) {
      if (buf.length < pos + 1) return false;
      csid = 64 + buf.readUInt8(pos++);
    } else if (csid === 1) {
      if (buf.length < pos + 2) return false;
      csid = 64 + buf.readUInt8(pos) + buf.readUInt8(pos + 1) * 256;
      pos += 2;
    }

    let st = this.chunks.get(csid);
    if (!st) {
      st = { timestamp: 0, length: 0, type: 0, streamId: 0, payload: [], have: 0 };
      this.chunks.set(csid, st);
    }

    const headerLen = [11, 7, 3, 0][fmt];
    if (buf.length < pos + headerLen) return false;

    let tsField = st.timestamp;
    if (fmt <= 2) {
      tsField = buf.readUIntBE(pos, 3);
      pos += 3;
    }
    if (fmt <= 1) {
      st.length = buf.readUIntBE(pos, 3);
      pos += 3;
      st.type = buf.readUInt8(pos);
      pos += 1;
    }
    if (fmt === 0) {
      st.streamId = buf.readUInt32LE(pos);
      pos += 4;
    }

    if (tsField === 0xffffff) {
      if (buf.length < pos + 4) return false;
      tsField = buf.readUInt32BE(pos);
      pos += 4;
    }
    st.timestamp = tsField;

    if (st.have === 0) st.payload = [];
    const remaining = st.length - st.have;
    const take = Math.min(remaining, this.inChunkSize);
    if (buf.length < pos + take) return false;

    st.payload.push(buf.subarray(pos, pos + take));
    st.have += take;
    pos += take;
    this.buf = buf.subarray(pos);

    if (st.have >= st.length) {
      const body = Buffer.concat(st.payload);
      st.have = 0;
      st.payload = [];
      this.handleMessage(st.type, st.streamId, body);
    }
    return true;
  }

  handleMessage(type, streamId, body) {
    switch (type) {
      case 0x01:
        this.inChunkSize = body.readUInt32BE(0);
        emit('set_chunk_size', { conn: this.id, size: this.inChunkSize });
        return;
      case 0x08:
        this.audioPackets++;
        if (body.length >= 2 && body.readUInt8(1) === 0) {
          this.audioSeqHeaders = (this.audioSeqHeaders || 0) + 1;
          emit('audio_sequence_header', { conn: this.id, n: this.audioSeqHeaders });
        }
        return;
      case 0x09:
        this.videoPackets++;
        if (body.length >= 2) {
          const b0v = body.readUInt8(0);
          const isEx = (b0v & 0x80) !== 0;
          /* Legacy: byte 1 is AVCPacketType, 0 = sequence header.
           * Enhanced: the low nibble of byte 0 is PacketType, 0 = SequenceStart. */
          const isSeqHeader = isEx ? (b0v & 0x0f) === 0 : body.readUInt8(1) === 0;
          if (isSeqHeader) {
            this.videoSeqHeaders = (this.videoSeqHeaders || 0) + 1;
            emit('video_sequence_header', {
              conn: this.id,
              n: this.videoSeqHeaders,
              before_first_keyframe: this.keyframes === 0,
            });
          }
        }
        if (body.length > 0) {
          const b0 = body.readUInt8(0);
          /* Legacy FLV video tag: high nibble is the frame type, 1 = key.
           * Enhanced RTMP sets the high bit and moves the frame type to
           * bits 6-4, but the frame-type field sits in the same place. */
          const frameType = (b0 & 0x80) !== 0 ? (b0 >> 4) & 0x07 : (b0 >> 4) & 0x0f;
          if (frameType === 1) {
            this.keyframes++;
            if (this.keyframes <= 3 || this.keyframes % 10 === 0)
              emit('keyframe', { conn: this.id, n: this.keyframes });
          }
        }
        return;
      case 0x12:
      case 0x0f:
        return; /* onMetaData and friends */
      case 0x14:
      case 0x11:
        this.handleCommand(type === 0x11 ? body.subarray(1) : body, streamId);
        return;
      default:
        return;
    }
  }

  handleCommand(body, streamId) {
    let values;
    try {
      values = amfDecodeAll(body);
    } catch (err) {
      emit('command_decode_error', { conn: this.id, error: String(err && err.message) });
      return;
    }

    const name = values[0];
    const txn = values[1];

    if (name === 'connect') {
      const cmdObj = values[2] || {};
      emit('connect', {
        conn: this.id,
        app: cmdObj.app,
        tcUrl: cmdObj.tcUrl,
        flashVer: cmdObj.flashVer,
        capsEx: Object.prototype.hasOwnProperty.call(cmdObj, 'capsEx') ? cmdObj.capsEx : null,
        capsExPresent: Object.prototype.hasOwnProperty.call(cmdObj, 'capsEx'),
      });

      this.sendWindowAck(2500000);
      this.sendPeerBandwidth(2500000, 2);
      this.sendSetChunkSize(4096);
      this.sendCommand(3, 0, [
        amfString('_result'),
        amfNumber(txn),
        amfObject([
          ['fmsVer', amfString('FMS/3,5,7,7009')],
          ['capabilities', amfNumber(31)],
          ['mode', amfNumber(1)],
        ]),
        amfObject([
          ['level', amfString('status')],
          ['code', amfString('NetConnection.Connect.Success')],
          ['description', amfString('Connection succeeded.')],
          ['objectEncoding', amfNumber(0)],
        ]),
      ]);
      return;
    }

    if (name === 'releaseStream' || name === 'FCPublish' || name === 'FCUnpublish' || name === 'deleteStream') {
      emit('command', { conn: this.id, name });
      return;
    }

    if (name === 'createStream') {
      this.sendCommand(3, 0, [amfString('_result'), amfNumber(txn), amfNull(), amfNumber(1)]);
      return;
    }

    if (name === 'publish') {
      const streamName = values[3];
      emit('publish', { conn: this.id, stream: streamName, streamId });
      this.publishing = true;

      /* Exactly what an ordinary server sends here, and exactly the message a
       * client that gates on `level` alone mistakes for a reconnect request. */
      this.sendCommand(5, 1, [
        amfString('onStatus'),
        amfNumber(0),
        amfNull(),
        amfObject([
          ['level', amfString('status')],
          ['code', amfString('NetStream.Publish.Start')],
          ['description', amfString(`${streamName} is now published`)],
        ]),
      ]);

      if (GARBAGE_AFTER !== null) {
        this.garbageTimer = setInterval(() => this.sendGarbage(), GARBAGE_AFTER * 1000);
      }

      if (RECONNECT_AFTER !== null) {
        const fire = () => this.sendReconnectRequest();
        if (RECONNECT_REPEAT) {
          this.reconnectTimer = setInterval(fire, RECONNECT_AFTER * 1000);
        } else {
          this.reconnectTimer = setTimeout(fire, RECONNECT_AFTER * 1000);
        }
      }
      return;
    }

    emit('command', { conn: this.id, name });
  }

  /* Command messages that are wrong in different ways. None of these should
   * do anything to the client except be ignored. */
  sendGarbage() {
    if (this.socket.destroyed) return;
    const n = (this.garbageSeq = (this.garbageSeq || 0) + 1);

    if (n % 5 === 1) {
      /* A well-formed onStatus whose info object is truncated mid-property. */
      const full = Buffer.concat([
        amfString('onStatus'),
        amfNumber(0),
        amfNull(),
        amfObject([
          ['level', amfString('status')],
          ['code', amfString(RECONNECT_SPEC_CODE)],
          ['tcUrl', amfString('rtmp://127.0.0.1:1/live')],
        ]),
      ]);
      this.sendMessage(3, 0x14, 0, full.subarray(0, full.length - 12));
    } else if (n % 5 === 2) {
      /* An AMF string whose declared length runs past the message. */
      const b = Buffer.alloc(11);
      b.writeUInt8(0x02, 0);
      b.writeUInt16BE(60000, 1);
      b.write('onStatus', 3);
      this.sendMessage(3, 0x14, 0, b);
    } else if (n % 5 === 3) {
      /* Random bytes that happen to start with an AMF string marker. */
      const b = Buffer.alloc(64);
      b.writeUInt8(0x02, 0);
      for (let i = 1; i < b.length; i++) b[i] = (i * 37 + n) & 0xff;
      this.sendMessage(3, 0x14, 0, b);
    } else if (n % 5 === 4) {
      /* onStatus with the reconnect code but the info object at the wrong
       * argument position, and a non-object where the info object belongs. */
      this.sendCommand(3, 0, [
        amfString('onStatus'),
        amfNumber(0),
        amfObject([['level', amfString('status')], ['code', amfString(RECONNECT_SPEC_CODE)]]),
        amfNumber(42),
      ]);
    } else {
      /* An empty command message. */
      this.sendMessage(3, 0x14, 0, Buffer.alloc(0));
    }
    emit('garbage_sent', { conn: this.id, n });
  }

  sendReconnectRequest() {
    if (this.socket.destroyed) return;

    const props = [
      ['level', amfString(RECONNECT_LEVEL)],
      ['code', amfString(RECONNECT_CODE)],
      ['description', amfString('This ingest is going away.')],
    ];
    if (RECONNECT_TCURL_LEN !== null) {
      props.push(['tcUrl', amfString('rtmp://127.0.0.1/' + 'a'.repeat(RECONNECT_TCURL_LEN))]);
    } else if (RECONNECT_URL !== null) {
      props.push(['tcUrl', amfString(RECONNECT_URL)]);
    }

    emit('reconnect_request_sent', {
      conn: this.id,
      level: RECONNECT_LEVEL,
      code: RECONNECT_CODE,
      tcUrl: RECONNECT_URL,
      tcUrlLen: RECONNECT_TCURL_LEN,
      spec_shaped: RECONNECT_LEVEL === 'status' && RECONNECT_CODE === RECONNECT_SPEC_CODE,
    });

    /* NetConnection onStatus: stream id 0, transaction id 0, null command
     * object, then the Info Object. */
    this.sendCommand(3, 0, [amfString('onStatus'), amfNumber(0), amfNull(), amfObject(props)]);
  }

  sendCommand(csid, streamId, parts) {
    this.sendMessage(csid, 0x14, streamId, Buffer.concat(parts));
  }

  sendWindowAck(size) {
    const b = Buffer.alloc(4);
    b.writeUInt32BE(size, 0);
    this.sendMessage(2, 0x05, 0, b);
  }

  sendPeerBandwidth(size, limitType) {
    const b = Buffer.alloc(5);
    b.writeUInt32BE(size, 0);
    b.writeUInt8(limitType, 4);
    this.sendMessage(2, 0x06, 0, b);
  }

  sendSetChunkSize(size) {
    const b = Buffer.alloc(4);
    b.writeUInt32BE(size, 0);
    this.sendMessage(2, 0x01, 0, b);
    this.outChunkSize = size;
  }

  sendMessage(csid, type, streamId, body) {
    if (this.socket.destroyed) return;

    const out = [];
    let offset = 0;
    let firstChunk = true;

    while (offset < body.length || firstChunk) {
      const take = Math.min(body.length - offset, this.outChunkSize);
      if (firstChunk) {
        const header = Buffer.alloc(12);
        header.writeUInt8((0 << 6) | csid, 0);
        header.writeUIntBE(0, 1, 3);
        header.writeUIntBE(body.length, 4, 3);
        header.writeUInt8(type, 7);
        header.writeUInt32LE(streamId, 8);
        out.push(header);
        firstChunk = false;
      } else {
        out.push(Buffer.from([(3 << 6) | csid]));
      }
      out.push(body.subarray(offset, offset + take));
      offset += take;
    }

    this.socket.write(Buffer.concat(out));
  }
}

const TLS_KEY = opt('tls-key', null);
const TLS_CERT = opt('tls-cert', null);

const onConnection = (socket) => {
  socket.setNoDelay(true);
  new Session(socket);
};

/* RTMPS is plain RTMP inside TLS, so the same session logic serves both. */
const server = TLS_KEY
  ? tls.createServer({ key: fs.readFileSync(TLS_KEY), cert: fs.readFileSync(TLS_CERT) }, onConnection)
  : net.createServer(onConnection);

if (TLS_KEY) {
  server.on('tlsClientError', (err) => emit('tls_client_error', { error: String(err && err.message) }));
}

server.on('error', (err) => {
  emit('server_error', { error: String(err && err.message) });
  process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
  emit('listening', { port: PORT, tls: !!TLS_KEY });
});

process.on('SIGTERM', () => {
  emit('shutdown', {});
  process.exit(0);
});
