import http from 'http';
import mqtt from 'mqtt';

const PORT       = parseInt(process.env.PORT       || '7431');
const MQTT_URL   = process.env.MQTT_URL             || 'mqtt://localhost:1883';
const MQTT_USER  = process.env.MQTT_USER            || '';
const MQTT_PASS  = process.env.MQTT_PASS            || '';
const MQTT_TOPIC = process.env.MQTT_TOPIC           || 'lametric/subscribers';

const mqttClient = mqtt.connect(MQTT_URL, {
    username: MQTT_USER,
    password: MQTT_PASS,
    reconnectPeriod: 3000,
});

mqttClient.on('connect', () => console.log(`MQTT connected: ${MQTT_URL}`));
mqttClient.on('error',   (e) => console.error('MQTT error:', e.message));

const server = http.createServer((req, res) => {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        res.end();
        return;
    }

    if (req.method !== 'POST' || req.url !== '/subscribers') {
        res.writeHead(404);
        res.end('not found');
        return;
    }

    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
        let count;
        try {
            const payload = JSON.parse(body);
            count = String(parseInt(payload.count, 10));
            if (isNaN(parseInt(count))) throw new Error('bad count');
        } catch {
            res.writeHead(400);
            res.end('bad request');
            return;
        }

        mqttClient.publish(MQTT_TOPIC, count, { retain: true }, (err) => {
            if (err) {
                console.error('publish error:', err.message);
                res.writeHead(502);
                res.end('mqtt error');
            } else {
                console.log(`[${new Date().toISOString()}] subscribers: ${count}`);
                res.writeHead(200);
                res.end('ok');
            }
        });
    });
});

server.listen(PORT, () => console.log(`Listening on :${PORT}`));
