const m = require('zigbee-herdsman-converters/lib/modernExtend');

const reporting = {
    temperature: {min: 600, max: 3600, change: 10},
    humidity: {min: 600, max: 3600, change: 50},
    batteryPercentage: {min: 3600, max: 65000, change: 2},
    batteryVoltage: {min: 3600, max: 65000, change: 2},
};

function withoutConfigure(extension) {
    const {configure: _configure, ...behavior} = extension;
    return behavior;
}

async function configureStep(name, operation) {
    try {
        await operation();
    } catch (error) {
        throw new Error(`Frostbee configure ${name} failed: ${error.message}`, {cause: error});
    }
}

const orderedConfigure = {
    isModernExtend: true,
    configure: [async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        if (!endpoint) {
            throw new Error('Frostbee configure failed: endpoint 1 is missing');
        }

        for (const cluster of ['msTemperatureMeasurement', 'msRelativeHumidity', 'genPowerCfg']) {
            await configureStep(`bind ${cluster}`, () => endpoint.bind(cluster, coordinatorEndpoint));
        }

        await configureStep('report temperature', () => endpoint.configureReporting('msTemperatureMeasurement', [{
            attribute: 'measuredValue',
            minimumReportInterval: reporting.temperature.min,
            maximumReportInterval: reporting.temperature.max,
            reportableChange: reporting.temperature.change,
        }]));
        await configureStep('report humidity', () => endpoint.configureReporting('msRelativeHumidity', [{
            attribute: 'measuredValue',
            minimumReportInterval: reporting.humidity.min,
            maximumReportInterval: reporting.humidity.max,
            reportableChange: reporting.humidity.change,
        }]));
        await configureStep('report battery percentage', () => endpoint.configureReporting('genPowerCfg', [{
            attribute: 'batteryPercentageRemaining',
            minimumReportInterval: reporting.batteryPercentage.min,
            maximumReportInterval: reporting.batteryPercentage.max,
            reportableChange: reporting.batteryPercentage.change,
        }]));
        await configureStep('report battery voltage', () => endpoint.configureReporting('genPowerCfg', [{
            attribute: 'batteryVoltage',
            minimumReportInterval: reporting.batteryVoltage.min,
            maximumReportInterval: reporting.batteryVoltage.max,
            reportableChange: reporting.batteryVoltage.change,
        }]));

        await configureStep('read temperature', () =>
            endpoint.read('msTemperatureMeasurement', ['measuredValue']));
        await configureStep('read humidity', () =>
            endpoint.read('msRelativeHumidity', ['measuredValue']));
        await configureStep('read battery attributes', () =>
            endpoint.read('genPowerCfg', ['batteryVoltage', 'batteryPercentageRemaining', 0xff01, 0xff02]));

        if (device.powerSource !== 'Battery') {
            device.powerSource = 'Battery';
            device.save();
        }
    }],
};

const definition = {
    zigbeeModel: ['FBE_TH_1'],
    model: 'FBE_TH_1',
    vendor: 'Frostbee',
    description: 'Temperature & humidity sensor (SHT40)',
    ota: true,
    extend: [
        withoutConfigure(m.battery({
            voltage: true,
            percentageReporting: true,
            voltageReporting: true,
            percentageReportingConfig: reporting.batteryPercentage,
            voltageReportingConfig: reporting.batteryVoltage,
        })),
        withoutConfigure(m.enumLookup({
            name: 'battery_type',
            lookup: {'alkaline': 0, 'nimh': 1, 'lithium': 2},
            cluster: 'genPowerCfg',
            attribute: {ID: 0xff01, type: 0x20},
        })),
        withoutConfigure(m.numeric({
            name: 'battery_series_count',
            cluster: 'genPowerCfg',
            attribute: {ID: 0xff02, type: 0x20},
            description: 'Number of batteries in series',
            valueMin: 1,
            valueMax: 4,
        })),
        withoutConfigure(m.temperature({reporting: reporting.temperature})),
        withoutConfigure(m.humidity({reporting: reporting.humidity})),
        orderedConfigure,
    ],
};

module.exports = definition;
