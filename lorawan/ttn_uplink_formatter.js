// OryzaFlow — Payload formatter de UPLINK para The Things Stack (Applications → oryzaflow → Payload formatters → Uplink,
// tipo "Custom Javascript formatter"). Es el decode() de referencia del codec binario v1.1 (ESQUEMA_TELEMETRIA.md §5)
// y reconstruye EXACTAMENTE el JSON canónico de §2 (mismas claves cortas que publica el edge por WiFi).
//
//   [0]     flags   bit0 lot · bit1 rain · bit2 hasDr · bit3 hasTe · bit4 fault · bit5 hasSeg · bit6 hasOm
//   [1..2]  t   int16  ×100 (°C)
//   [3..4]  rh  uint16 ×100 (%)
//   [5..6]  me  uint16 ×100 (% b.h.)
//   si lot: m uint16 ×100 · mr uint16 ×10000 · [dr int16 ×100] · [te uint16 ×100] · [seg uint8] · [om int16 ×100]
function decodeUplink(input) {
  var b = input.bytes;
  var warnings = [];
  if (b && b.length === 1 && (b[0] & 0x10)) {
    return { data: { fault: true }, warnings: ["latido sin lectura del sensor (SHT31)"] };
  }
  if (!b || b.length < 7) {
    return { data: {}, errors: ["payload demasiado corto (" + (b ? b.length : 0) + " B)"] };
  }
  function u16(i) { return (b[i] << 8) | b[i + 1]; }
  function s16(i) { var v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; }

  var flags = b[0];
  var lot = (flags & 0x01) !== 0;
  var d = {
    t: s16(1) / 100,
    rh: u16(3) / 100,
    rain: (flags & 0x02) !== 0,
    me: u16(5) / 100,
    lot: lot
  };
  if (flags & 0x10) d.fault = true;

  var o = 7;
  if (lot) {
    if (b.length < o + 4) return { data: d, errors: ["lote activo pero faltan m/mr"] };
    d.m = u16(o) / 100; o += 2;
    d.mr = u16(o) / 10000; o += 2;
    if (flags & 0x04) { d.dr = s16(o) / 100; o += 2; }
    if (flags & 0x08) { d.te = u16(o) / 100; o += 2; }
    if (flags & 0x20) { d.seg = b[o]; o += 1; }
    if (flags & 0x40) { d.om = s16(o) / 100; o += 2; }
  }
  if (o !== b.length) warnings.push("bytes sobrantes: " + (b.length - o));
  // fPort 1 = telemetría. Cualquier otro puerto se decodifica igual pero se avisa.
  if (input.fPort !== 1) warnings.push("fPort inesperado: " + input.fPort);
  return { data: d, warnings: warnings };
}
