const m = require('zigbee-herdsman-converters/lib/modernExtend');

const commonExtend = [
    m.battery({
        voltage: true,
        percentageReporting: true,
        voltageReporting: true,
        percentageReportingConfig: {min: 3600, max: 65000, change: 2},
        voltageReportingConfig: {min: 3600, max: 65000, change: 2},
    }),
    m.enumLookup({
        name: 'battery_type',
        lookup: {'alkaline': 0, 'nimh': 1, 'lithium': 2},
        cluster: 'genPowerCfg',
        attribute: {ID: 0xff01, type: 0x20},
    }),
    m.numeric({
        name: 'battery_series_count',
        cluster: 'genPowerCfg',
        attribute: {ID: 0xff02, type: 0x20},
        description: 'Number of batteries in series',
        valueMin: 1,
        valueMax: 4,
    }),
    m.temperature({
        reporting: {min: 600, max: 3600, change: 10},
    }),
    m.humidity({
        reporting: {min: 600, max: 3600, change: 50},
    }),
];

const definitions = [
    {
        zigbeeModel: ['FBE_TH_1'],
        model: 'FBE_TH_1',
        vendor: 'Frostbee',
        description: 'Temperature & humidity sensor (SHT40)',
        ota: true,
        extend: commonExtend,
    },
    {
        zigbeeModel: ['FBE_TH_2'],
        model: 'FBE_TH_2',
        vendor: 'Frostbee',
        description: 'Temperature & humidity sensor (BME280)',
        ota: true,
        extend: [
            ...commonExtend,
            m.pressure({
                reporting: {min: 600, max: 3600, change: 1},
            })],
    },
];

module.exports = definitions;
