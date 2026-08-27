const assert = require('node:assert/strict');
const test = require('node:test');
const {prepareDefinition} = require('zigbee-herdsman-converters');
const definition = require('./frostbee');

const expectedEvents = [
    ['bind', 'msTemperatureMeasurement'],
    ['bind', 'msRelativeHumidity'],
    ['bind', 'genPowerCfg'],
    ['report', 'msTemperatureMeasurement', 'measuredValue', 600, 3600, 10],
    ['report', 'msRelativeHumidity', 'measuredValue', 600, 3600, 50],
    ['report', 'genPowerCfg', 'batteryPercentageRemaining', 3600, 65000, 2],
    ['report', 'genPowerCfg', 'batteryVoltage', 3600, 65000, 2],
    ['read', 'msTemperatureMeasurement', 'measuredValue'],
    ['read', 'msRelativeHumidity', 'measuredValue'],
    ['read', 'genPowerCfg', 'batteryVoltage', 'batteryPercentageRemaining', 0xff01, 0xff02],
    ['save', 'Battery'],
];

function mockDevice(events) {
    const endpoint = {
        ID: 1,
        bind: async (cluster) => events.push(['bind', cluster]),
        configureReporting: async (cluster, configurations) => {
            for (const configuration of configurations) {
                events.push([
                    'report',
                    cluster,
                    configuration.attribute,
                    configuration.minimumReportInterval,
                    configuration.maximumReportInterval,
                    configuration.reportableChange,
                ]);
            }
        },
        read: async (cluster, attributes) => events.push(['read', cluster, ...attributes]),
    };

    return {
        endpoints: [endpoint],
        getEndpoint: (id) => id === 1 ? endpoint : undefined,
        powerSource: undefined,
        save() {
            events.push(['save', this.powerSource]);
        },
    };
}

function exposeNames(prepared) {
    const exposes = typeof prepared.exposes === 'function' ?
        prepared.exposes({isDummyDevice: true}, {}) : prepared.exposes;
    return new Set(exposes.map((expose) => expose.property ?? expose.name));
}

test('preserves the external converter contract without generated configure callbacks', () => {
    assert.deepEqual(definition.zigbeeModel, ['FBE_TH_1']);
    assert.equal(definition.ota, true);
    assert.equal(definition.extend.filter((extension) => extension.configure).length, 1);

    const prepared = prepareDefinition(definition);
    const names = exposeNames(prepared);
    for (const name of [
        'temperature',
        'humidity',
        'battery',
        'voltage',
        'battery_type',
        'battery_series_count',
    ]) {
        assert(names.has(name), `missing expose: ${name}`);
    }
    assert(prepared.fromZigbee.length > 0);
    assert(prepared.toZigbee.length > 0);

    const fromClusters = new Set(prepared.fromZigbee.map((converter) => converter.cluster));
    for (const cluster of ['msTemperatureMeasurement', 'msRelativeHumidity', 'genPowerCfg']) {
        assert(fromClusters.has(cluster), `missing fromZigbee converter: ${cluster}`);
    }

    const toKeys = new Set(prepared.toZigbee.flatMap((converter) => converter.key));
    for (const key of ['battery', 'voltage', 'battery_type', 'battery_series_count']) {
        assert(toKeys.has(key), `missing toZigbee key: ${key}`);
    }
});

test('binds, configures reporting, and reads exactly once in order', async () => {
    for (let run = 0; run < 2; run++) {
        const prepared = prepareDefinition(definition);
        const events = [];
        const device = mockDevice(events);

        await prepared.configure(device, {ID: 1}, prepared);

        assert.deepEqual(events, expectedEvents);
    }
});

test('does not persist an already-correct power source', async () => {
    const prepared = prepareDefinition(definition);
    const events = [];
    const device = mockDevice(events);
    device.powerSource = 'Battery';

    await prepared.configure(device, {ID: 1}, prepared);

    assert.deepEqual(events, expectedEvents.slice(0, -1));
});
