// OryzaFlow — Data converter de DOWNLINK para ThingsBoard (Integrations center → Data converters → Add → Downlink, JS).
// Camino B: el backend sigue escribiendo/borrando shared attributes del device en TB (ProvisioningService).
// La Rule Chain envía los mensajes "Attributes Updated" / "Attributes Deleted" al nodo "integration downlink" de la
// integración The Things Stack, y este converter los convierte en el downlink binario §5.4 (fPort 10) que TTS
// entrega al nodo tras su siguiente uplink. Los bytes se generan aquí (frm_payload base64), sin depender del
// formatter de downlink de TTS.
//
//   Attributes Updated con loteId/loteInicioTs/m0/... -> SET_LOT (0x01)
//   Attributes Deleted con loteId                     -> CLEAR_LOT (0x02)
//   Attributes Updated solo con offsetT/offsetRh/intervaloEnvioSegundos -> SET_NODE (0x03)
// Nota: ThingsBoard envuelve este código en function Encoder(msg, metadata, msgType, integrationMetadata) { ... };
// por eso las auxiliares van primero y hay un único return al final.

var base64Chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

var bytesToBase64Str = function (bytes) {
    var s = '';
    for (var i = 0; i < bytes.length; i += 3) {
        var b0 = bytes[i];
        var b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
        var b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
        var n = (b0 << 16) | (b1 << 8) | b2;
        s += base64Chars.charAt((n >> 18) & 63) + base64Chars.charAt((n >> 12) & 63);
        s += (i + 1 < bytes.length) ? base64Chars.charAt((n >> 6) & 63) : '=';
        s += (i + 2 < bytes.length) ? base64Chars.charAt(n & 63) : '=';
    }
    return s;
};

var num = function (x, def) {
    if (x === undefined || x === null) return def;
    var v = Number(x);
    return isNaN(v) ? def : v;
};

var out = [];
var u8 = function (v) { out.push(Math.max(0, Math.min(255, Math.round(v))) & 0xff); };
var u16 = function (v) { v = Math.max(0, Math.min(65535, Math.round(v))); out.push((v >> 8) & 0xff, v & 0xff); };
var s16 = function (v) { v = Math.max(-32768, Math.min(32767, Math.round(v))); if (v < 0) v += 0x10000; out.push((v >> 8) & 0xff, v & 0xff); };
var u32 = function (v) { v = Math.max(0, Math.round(v)); out.push((v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff); };

var data = msg;                        // "Attributes Updated": los atributos; "Attributes Deleted": { attributes: [...] }
var has = function (k) { return data[k] !== undefined && data[k] !== null; };

var cmd = null;
var deletedKeys = data.attributes;
if (msgType === 'ATTRIBUTES_DELETED' || (deletedKeys && deletedKeys.length !== undefined)) {
    var keys = deletedKeys || [];
    var clears = false;
    for (var i = 0; i < keys.length; i++) if (keys[i] === 'loteId') clears = true;
    if (clears) { cmd = 'CLEAR_LOT'; out.push(0x02); }
} else if (has('loteId') && has('loteInicioTs') && has('m0')) {
    cmd = 'SET_LOT';
    var inicio = num(data.loteInicioTs, 0);
    var lotId = 0;
    var m = /(\d+)$/.exec(String(data.loteId));
    if (m) lotId = Number(m[1]);
    out.push(0x01);
    u32(inicio > 1e11 ? inicio / 1000 : inicio);          // loteInicioTs llega en ms (Date.getTime)
    u16(num(data.m0, 0) * 100);
    u16(num(data.mTarget, 12.0) * 100);
    u16(num(data.k, 0.12) * 10000);
    u16(num(data.n, 1.05) * 1000);
    u8(data.modelo === 'MIDILLI' ? 1 : 0);
    u16(num(data.a, 1.0) * 1000);
    s16(num(data.b, 0.0) * 10000);
    s16(num(data.offsetM, 0.0) * 100);
    u8(num(data.segmento, 1));
    u16(lotId);
} else if (has('offsetT') || has('offsetRh') || has('intervaloEnvioSegundos')) {
    cmd = 'SET_NODE';
    out.push(0x03);
    s16(num(data.offsetT, 0) * 100);
    s16(num(data.offsetRh, 0) * 100);
    u16(num(data.intervaloEnvioSegundos, 300));
}

var deviceName = (metadata && metadata.deviceName) ? metadata.deviceName : 'oryzaflow-edge-01';
var result = null;
if (cmd !== null) {
    var body = {
        downlinks: [{
            f_port: 10,
            frm_payload: bytesToBase64Str(out),
            priority: 'NORMAL',
            confirmed: false
        }]
    };
    result = {
        contentType: 'JSON',
        data: JSON.stringify(body),
        metadata: {
            deviceName: deviceName,
            cmd: cmd,
            // MQTT de TTS: v3/{application}@{tenant}/devices/{device}/down/replace (replace = solo queda el último)
            topic: 'v3/oryzaflow@oryzaflow/devices/' + deviceName + '/down/replace'
        }
    };
}
// Sin comando (p. ej. solo cambió tMax/perfil): null = no se envía ningún downlink.
return result;
