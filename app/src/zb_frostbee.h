/*
 * Frostbee - Zigbee Device Definition
 *
 * Validated production shape:
 * - single endpoint
 * - Basic, Identify, Power Config, Temp Measurement, Humidity server clusters
 * - Identify client cluster
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZB_FROSTBEE_H
#define ZB_FROSTBEE_H 1

#define FROSTBEE_ENDPOINT          1
#define FROSTBEE_IN_CLUSTER_NUM    6
#define FROSTBEE_OUT_CLUSTER_NUM   1

#define FROSTBEE_REPORT_ATTR_COUNT \
	(ZB_ZCL_TEMP_MEASUREMENT_REPORT_ATTR_COUNT + \
	 ZB_ZCL_REL_HUMIDITY_MEASUREMENT_REPORT_ATTR_COUNT + \
	 ZB_ZCL_PRESSURE_MEASUREMENT_REPORT_ATTR_COUNT + \
	 2)

#define ZB_DECLARE_FROSTBEE_CLUSTER_LIST(                        \
		cluster_list_name,                               \
		basic_attr_list,                                 \
		identify_client_attr_list,                       \
		identify_server_attr_list,                       \
		power_config_attr_list,                          \
		temp_measurement_attr_list,                      \
		humidity_attr_list,                              \
		pressure_attr_list)                              \
	zb_zcl_cluster_desc_t cluster_list_name[] =              \
	{                                                        \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_BASIC,                 \
			ZB_ZCL_ARRAY_SIZE(                       \
				basic_attr_list, zb_zcl_attr_t), \
			(basic_attr_list),                       \
			ZB_ZCL_CLUSTER_SERVER_ROLE,              \
			ZB_ZCL_MANUF_CODE_INVALID),              \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_IDENTIFY,              \
			ZB_ZCL_ARRAY_SIZE(                       \
				identify_server_attr_list,        \
				zb_zcl_attr_t),                  \
			(identify_server_attr_list),              \
			ZB_ZCL_CLUSTER_SERVER_ROLE,              \
			ZB_ZCL_MANUF_CODE_INVALID),              \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_POWER_CONFIG,          \
			ZB_ZCL_ARRAY_SIZE(                       \
				power_config_attr_list,          \
				zb_zcl_attr_t),                  \
			(power_config_attr_list),                \
			ZB_ZCL_CLUSTER_SERVER_ROLE,              \
			ZB_ZCL_MANUF_CODE_INVALID),              \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,      \
			ZB_ZCL_ARRAY_SIZE(                       \
				temp_measurement_attr_list,       \
				zb_zcl_attr_t),                  \
			(temp_measurement_attr_list),             \
			ZB_ZCL_CLUSTER_SERVER_ROLE,              \
			ZB_ZCL_MANUF_CODE_INVALID),              \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, \
			ZB_ZCL_ARRAY_SIZE(                          \
				humidity_attr_list, zb_zcl_attr_t), \
			(humidity_attr_list),                      \
			ZB_ZCL_CLUSTER_SERVER_ROLE,                \
			ZB_ZCL_MANUF_CODE_INVALID),                \
		ZB_ZCL_CLUSTER_DESC( \
            ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT, \
            ZB_ZCL_ARRAY_SIZE(pressure_attr_list, zb_zcl_attr_t), \
                (pressure_attr_list), \
            ZB_ZCL_CLUSTER_SERVER_ROLE, \
            ZB_ZCL_MANUF_CODE_INVALID), \
		ZB_ZCL_CLUSTER_DESC(                             \
			ZB_ZCL_CLUSTER_ID_IDENTIFY,              \
			ZB_ZCL_ARRAY_SIZE(                       \
				identify_client_attr_list,        \
				zb_zcl_attr_t),                  \
			(identify_client_attr_list),              \
			ZB_ZCL_CLUSTER_CLIENT_ROLE,              \
			ZB_ZCL_MANUF_CODE_INVALID),              \
	}

#define ZB_ZCL_DECLARE_FROSTBEE_DESC(ep_name, ep_id, in_clust_num, out_clust_num) \
	ZB_DECLARE_SIMPLE_DESC(in_clust_num, out_clust_num);                      \
	ZB_AF_SIMPLE_DESC_TYPE(in_clust_num, out_clust_num)                       \
		simple_desc_##ep_name =                                           \
	{                                                                         \
		ep_id,                                                            \
		ZB_AF_HA_PROFILE_ID,                                              \
		ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,                               \
		0,                                                                \
		0,                                                                \
		in_clust_num,                                                     \
		out_clust_num,                                                    \
		{                                                                 \
			ZB_ZCL_CLUSTER_ID_BASIC,                                  \
			ZB_ZCL_CLUSTER_ID_IDENTIFY,                               \
			ZB_ZCL_CLUSTER_ID_POWER_CONFIG,                           \
			ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,                       \
			ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,               \
			ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,                   \
			ZB_ZCL_CLUSTER_ID_IDENTIFY,                               \
		}                                                                 \
	}

#define ZB_DECLARE_FROSTBEE_EP(ep_name, ep_id, cluster_list)    \
	ZB_ZCL_DECLARE_FROSTBEE_DESC(                            \
		ep_name, ep_id,                                  \
		FROSTBEE_IN_CLUSTER_NUM,                         \
		FROSTBEE_OUT_CLUSTER_NUM);                       \
	ZBOSS_DEVICE_DECLARE_REPORTING_CTX(                      \
		reporting_info##ep_name,                          \
		FROSTBEE_REPORT_ATTR_COUNT);                      \
	ZB_AF_DECLARE_ENDPOINT_DESC(                             \
		ep_name, ep_id,                                 \
		ZB_AF_HA_PROFILE_ID,                            \
		0,                                              \
		NULL,                                           \
		ZB_ZCL_ARRAY_SIZE(cluster_list,                 \
				  zb_zcl_cluster_desc_t),       \
		cluster_list,                                   \
		(zb_af_simple_desc_1_1_t *)&simple_desc_##ep_name, \
		FROSTBEE_REPORT_ATTR_COUNT,                     \
		reporting_info##ep_name,                        \
		0,                                              \
		NULL)

enum frostbee_battery_type {
    BATTERY_TYPE_ALKALINE = 0,
    BATTERY_TYPE_NIMH = 1,
    BATTERY_TYPE_LITHIUM = 2,
};

#endif /* ZB_FROSTBEE_H */
