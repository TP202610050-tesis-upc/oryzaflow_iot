// OryzaFlow — Payload formatter de DOWNLINK para The Things Stack (Applications → oryzaflow → Payload formatters →
// Downlink, "Custom Javascript formatter"). Codifica el aprovisionamiento del backend (ESQUEMA_TELEMETRIA.md §5.4)
// para que el backend pueda encolar downlinks con JSON (`decoded_payload`) y TTS produzca los bytes.
// Todos los enteros big-endian. fPort 10.
//
//   SET_LOT   (0x01, 23 B): inicioTs u32 (epoch s) · m0 u16×100 · mTarget u16×100 · k u16×10000 · n u16×1000 ·
//                            modelo u8 (0 PAGE, 1 MIDILLI) · a u16×1000 · b i16×10000 · offsetM i16×100 · segmento u8 · lotId u16
//   CLEAR_LOT (0x02, 1 B)
//   SET_NODE  (0x03, 7 B):  offsetT i16×100 · offsetRh i16×100 · intervaloEnvioSegundos u16
//   TIME_HINT (0x04, 5 B):  epoch u32 (respaldo si la red no responde DeviceTimeReq; TTS lo rellena con "ahora" si falta)
//
// Ejemplos de decoded_payload:
//   { "cmd": "SET_LOT", "lotId": 12, "segmento": 2, "inicioTs": 1790000000, "m0": 15.3, "mTarget": 12.0,
//     "k": 0.12, "n": 1.05, "modelo": "PAGE", "a": 1, "b": 0, "offsetM": 0 }
//   { "cmd": "CLEAR_LOT" }
//   { "cmd": "SET_NODE", "offsetT": -0.3, "offsetRh": 1.2, "intervaloEnvioSegundos": 300 }
//   { "cmd": "TIME_HINT" }
function encodeDownlink(input) {
  var d = input.data || {};
  var out = [];
  var errors = [];
  function u8(v) { out.push(Math.max(0, Math.min(255, Math.round(v))) & 0xff); }
  function u16(v) { v = Math.max(0, Math.min(65535, Math.round(v))); out.push((v >> 8) & 0xff, v & 0xff); }
  function s16(v) { v = Math.max(-32768, Math.min(32767, Math.round(v))); if (v < 0) v += 0x10000; out.push((v >> 8) & 0xff, v & 0xff); }
  function u32(v) { v = Math.max(0, Math.round(v)); out.push((v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff); }
  function num(x, def) { return (x === undefined || x === null || isNaN(Number(x))) ? def : Number(x); }

  switch (d.cmd) {
    case "SET_LOT":
      if (d.inicioTs === undefined || d.m0 === undefined || d.mTarget === undefined) {
        errors.push("SET_LOT requiere inicioTs (epoch s), m0 y mTarget (% b.h.)");
        break;
      }
      out.push(0x01);
      u32(num(d.inicioTs, 0) > 1e11 ? num(d.inicioTs, 0) / 1000 : num(d.inicioTs, 0));   // acepta ms o s
      u16(num(d.m0, 0) * 100);
      u16(num(d.mTarget, 0) * 100);
      u16(num(d.k, 0.12) * 10000);
      u16(num(d.n, 1.05) * 1000);
      u8(d.modelo === "MIDILLI" || d.modelo === 1 ? 1 : 0);
      u16(num(d.a, 1.0) * 1000);
      s16(num(d.b, 0.0) * 10000);
      s16(num(d.offsetM, 0.0) * 100);
      u8(num(d.segmento, 1));
      u16(num(d.lotId, 0));
      break;
    case "CLEAR_LOT":
      out.push(0x02);
      break;
    case "SET_NODE":
      out.push(0x03);
      s16(num(d.offsetT, 0) * 100);
      s16(num(d.offsetRh, 0) * 100);
      u16(num(d.intervaloEnvioSegundos, 300));
      break;
    case "TIME_HINT":
      out.push(0x04);
      u32(num(d.epoch, Math.floor(Date.now() / 1000)));
      break;
    default:
      errors.push("cmd desconocido: " + d.cmd + " (SET_LOT | CLEAR_LOT | SET_NODE | TIME_HINT)");
  }
  return { bytes: out, fPort: 10, errors: errors };
}

// Espejo para que la consola muestre legible lo que se encoló.
function decodeDownlink(input) {
  var b = input.bytes;
  function u16(i) { return (b[i] << 8) | b[i + 1]; }
  function s16(i) { var v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; }
  function u32(i) { return ((b[i] << 24) >>> 0) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]; }
  if (!b || b.length < 1) return { data: {}, errors: ["vacío"] };
  switch (b[0]) {
    case 0x01:
      if (b.length < 21) return { data: {}, errors: ["SET_LOT corto"] };
      return { data: { cmd: "SET_LOT", inicioTs: u32(1), m0: u16(5) / 100, mTarget: u16(7) / 100, k: u16(9) / 10000,
        n: u16(11) / 1000, modelo: b[13] === 1 ? "MIDILLI" : "PAGE", a: u16(14) / 1000, b: s16(16) / 10000,
        offsetM: s16(18) / 100, segmento: b[20], lotId: b.length >= 23 ? u16(21) : undefined } };
    case 0x02: return { data: { cmd: "CLEAR_LOT" } };
    case 0x03: return { data: { cmd: "SET_NODE", offsetT: s16(1) / 100, offsetRh: s16(3) / 100, intervaloEnvioSegundos: u16(5) } };
    case 0x04: return { data: { cmd: "TIME_HINT", epoch: u32(1) } };
    default: return { data: {}, errors: ["cmd desconocido 0x" + b[0].toString(16)] };
  }
}
