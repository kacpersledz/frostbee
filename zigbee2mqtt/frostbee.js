import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
    zigbeeModel: ['FBE_TH_1'],
    model: 'FBE_TH_1',
    vendor: 'Frostbee',
    description: 'Temperature & humidity sensor (SHT40)',
    ota: true,
    extend: [
        m.battery({
            voltage: true,
            voltageReporting: true,
            percentageReportingConfig: {min: 3600, max: 65000, change: 2},
            voltageReportingConfig: {min: 3600, max: 65000, change: 2},
        }),
        m.temperature({reporting: {min: 600, max: 3600, change: 10}}),
        m.humidity({reporting: {min: 600, max: 3600, change: 50}}),
    ],
};