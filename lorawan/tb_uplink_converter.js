// OryzaFlow — Data converter de UPLINK para ThingsBoard (Integrations center → Data converters → Add → Uplink, JS).
// Integración "The Things Stack" (camino B): TB recibe el JSON de TTS y este converter lo traduce a
// { deviceName, deviceType, telemetry, attributes }. El deviceName debe coincidir con el device ya existente en TB
// (oryzaflow-edge-01 = Node.serialEsp32 del backend), así la Rule Chain actual lo reenvía a HiveMQ sin cambios.
// Usa decoded_payload si TTS tiene cargado el formatter de uplink; si no, decodifica frm_payload (base64) aquí mismo.
// Nota: ThingsBoard envuelve este código en function decoder(payload, metadata) { ... }; por eso las funciones
// auxiliares van primero y hay un único return al final.

var base64Chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

var base64ToBytesArray = function (s) {
    var out = [], bits = 0, acc = 0;
    for (var i = 0; i < s.length; i++) {
        var c = base64Chars.indexOf(s.charAt(i));
        if (c < 0) continue;
        acc = ((acc << 6) | c) & 0xffffff;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push((acc >> bits) & 0xff); }
    }
    return out;
};

// Mismo codec v1.1 que lorawan/ttn_uplink_formatter.js (ESQUEMA_TELEMETRIA.md §5).
var decodeBytes = function (b) {
    if (b && b.length === 1 && (b[0] & 0x10)) return { fault: true };   // latido sin lectura del sensor
    if (!b || b.length < 7) return null;
    var u16 = function (i) { return (b[i] << 8) | b[i + 1]; };
    var s16 = function (i) { var v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; };
    var flags = b[0];
    var lot = (flags & 0x01) !== 0;
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
};

// payload llega como arreglo de bytes (o ya como texto/objeto según la versión de TB). Se evita decodeToJson()
// porque el validador del editor de thingsboard.cloud no lo reconoce y deshabilita el botón Test.
var parsePayload = function (p) {
    if (p === null || p === undefined) return {};
    if (typeof p === 'string') return JSON.parse(p);
    if (typeof p === 'object' && p.length === undefined) return p;   // ya es un objeto JSON
    var pct = '';
    for (var i = 0; i < p.length; i++) pct += '%' + ('0' + (p[i] & 0xff).toString(16)).slice(-2);
    var text;
    try { text = decodeURIComponent(pct); }                            // UTF-8 correcto
    catch (e) { text = ''; for (var j = 0; j < p.length; j++) text += String.fromCharCode(p[j] & 0xff); }
    return JSON.parse(text);
};

var data = parsePayload(payload);

var deviceName = 'oryzaflow-edge-01';
if (data.end_device_ids && data.end_device_ids.device_id) deviceName = data.end_device_ids.device_id;
var up = data.uplink_message || {};
var telemetry = null;
if (up.decoded_payload && up.decoded_payload.t !== undefined) {
    telemetry = up.decoded_payload;
} else if (up.frm_payload) {
    telemetry = decodeBytes(base64ToBytesArray(up.frm_payload));
}

var ts = data.received_at ? Date.parse(data.received_at) : Date.now();
var rx = {};
if (up.rx_metadata && up.rx_metadata.length > 0) rx = up.rx_metadata[0];

var attributes = { transport: 'lorawan' };
if (rx.gateway_ids && rx.gateway_ids.gateway_id) attributes.gatewayId = rx.gateway_ids.gateway_id;
if (up.f_port !== undefined) attributes.fPort = up.f_port;
if (up.f_cnt !== undefined) attributes.fCnt = up.f_cnt;

var telemetryList = [];
if (telemetry && telemetry.fault && telemetry.t === undefined) {
    // Latido de 1 byte (sensor SHT31 sin lectura): NO se publica telemetría (la Rule Chain no lo reenvía a HiveMQ y el
    // backend no ingiere una lectura vacía); queda registrado como atributo del device para diagnóstico.
    attributes.sensorFault = true;
    attributes.sensorFaultTs = ts;
    telemetry = null;
}
if (telemetry) {
    var values = {};
    for (var k in telemetry) values[k] = telemetry[k];
    if (rx.rssi !== undefined) values.rssi = rx.rssi;
    if (rx.snr !== undefined) values.snr = rx.snr;
    telemetryList.push({ ts: ts, values: values });
}

var result = {
    deviceName: deviceName,
    deviceType: 'oryzaflow-edge',
    attributes: attributes,
    telemetry: telemetryList
};
return result;
