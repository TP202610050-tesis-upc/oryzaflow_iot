// OryzaFlow — Data converter de UPLINK para ThingsBoard (Integrations center → Data converters → Add → Uplink).
// Integración "The Things Stack" (camino B): TB recibe el JSON de TTS y este converter lo traduce a
// { deviceName, deviceType, telemetry, attributes }. El deviceName debe coincidir con el device ya existente en TB
// (oryzaflow-edge-01 = Node.serialEsp32 del backend), así la Rule Chain actual lo reenvía a HiveMQ sin cambios.
// Usa decoded_payload si TTS tiene cargado el formatter de uplink; si no, decodifica frm_payload (base64) aquí mismo.

var data = decodeToJson(payload);

var deviceName = (data.end_device_ids && data.end_device_ids.device_id) || 'oryzaflow-edge-01';
var up = data.uplink_message || {};
var telemetry = null;

if (up.decoded_payload && up.decoded_payload.t !== undefined) {
    telemetry = up.decoded_payload;
} else if (up.frm_payload) {
    telemetry = decodeBytes(base64ToBytesArray(up.frm_payload));
}

var ts = data.received_at ? Date.parse(data.received_at) : Date.now();
var rx = (up.rx_metadata && up.rx_metadata[0]) || {};

var result = {
    deviceName: deviceName,
    deviceType: 'oryzaflow-edge',
    attributes: {
        transport: 'lorawan',
        gatewayId: rx.gateway_ids ? rx.gateway_ids.gateway_id : undefined,
        fPort: up.f_port,
        fCnt: up.f_cnt
    },
    telemetry: []
};
if (telemetry) {
    var values = {};
    for (var k in telemetry) values[k] = telemetry[k];
    if (rx.rssi !== undefined) values.rssi = rx.rssi;
    if (rx.snr !== undefined) values.snr = rx.snr;
    result.telemetry.push({ ts: ts, values: values });
}
return result;

// ---- helpers (mismo codec v1.1 que lorawan/ttn_uplink_formatter.js) ----
function decodeBytes(b) {
    if (!b || b.length < 7) return null;
    function u16(i) { return (b[i] << 8) | b[i + 1]; }
    function s16(i) { var v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; }
    var flags = b[0], lot = (flags & 0x01) !== 0;
    var d = { t: s16(1) / 100, rh: u16(3) / 100, rain: (flags & 0x02) !== 0, me: u16(5) / 100, lot: lot };
    if (flags & 0x10) d.fault = true;
    var o = 7;
    if (lot) {
        d.m = u16(o) / 100; o += 2;
        d.mr = u16(o) / 10000; o += 2;
        if (flags & 0x04) { d.dr = s16(o) / 100; o += 2; }
        if (flags & 0x08) { d.te = u16(o) / 100; o += 2; }
        if (flags & 0x20) { d.seg = b[o]; o += 1; }
        if (flags & 0x40) { d.om = s16(o) / 100; o += 2; }
    }
    return d;
}

function base64ToBytesArray(s) {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var out = [], bits = 0, acc = 0;
    for (var i = 0; i < s.length; i++) {
        var c = chars.indexOf(s.charAt(i));
        if (c < 0) continue;
        acc = (acc << 6) | c; bits += 6;
        if (bits >= 8) { bits -= 8; out.push((acc >> bits) & 0xff); }
    }
    return out;
}
