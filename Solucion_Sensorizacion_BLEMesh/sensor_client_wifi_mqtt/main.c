/* Lidia Fernández Fernández - TFM */

/* main.c - Application main entry point */
/*
 * Copyright (c) 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Librerías cliente (sensor model) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"

#include "ble_mesh_example_init.h"
#include "board.h"

/* Librerías WiFi */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* Librerías MQTT */
#include <stdint.h>
#include <stddef.h>
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mqtt_client.h"

/* Hilos */
#include <pthread.h>
#include "esp_pthread.h"

#define CID_ESP             0x02E5
#define CID_NVAL            0xFFFF

#define PROV_OWN_ADDR       0x0001

#define MSG_SEND_TTL        3
#define MSG_SEND_REL        false
#define MSG_TIMEOUT         0
#define MSG_ROLE            ROLE_PROVISIONER

#define COMP_DATA_PAGE_0    0x00

#define APP_KEY_IDX         0x0000
#define APP_KEY_OCTET       0x12

#define COMP_DATA_1_OCTET(msg, offset)      (msg[offset])
#define COMP_DATA_2_OCTET(msg, offset)      (msg[offset + 1] << 8 | msg[offset])

/* Wifi Configurarion via project configiration menu */
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG_wifi = "wifi station";

static int s_retry_num = 0;

static const char *TAG_mqtt = "MQTT_EXAMPLE";

static esp_mqtt_client_handle_t client = NULL;

/* Variables Globales */

static int num_nodo = 0;
static char mensaje[200];
static char prov_msg[200];
static char solic_nodo[200];
static char topic_sub[200];
static char topic_publish[200];
static char topic_comun_interior[200] = "/temperatura/interior/nodo";
static char topic_comun_exterior[200] = "/temperatura/exterior/nodo";
static char rest[200];
static char auxTopic[200];

/* Cliente */

static uint8_t  dev_uuid[ESP_BLE_MESH_OCTET16_LEN];
static uint16_t server_address[CONFIG_BLE_MESH_MAX_PROV_NODES] = {
    [0 ... (CONFIG_BLE_MESH_MAX_PROV_NODES - 1)] = {
    	ESP_BLE_MESH_ADDR_UNASSIGNED
    } 
};
static uint8_t  nodes;
static uint16_t sensor_prop_id;

static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[ESP_BLE_MESH_OCTET16_LEN];
} prov_key;

static esp_ble_mesh_cfg_srv_t config_server = {
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

static esp_ble_mesh_client_t config_client;
static esp_ble_mesh_client_t sensor_client;

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_SENSOR_CLI(NULL, &sensor_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provision = {
    .prov_uuid          = dev_uuid,
    .prov_unicast_addr  = PROV_OWN_ADDR,
    .prov_start_address = 0x0005,
};

/* Temperatura ambiente interior actual*/
float temperaturaInterior(int nodo) {

	float tempInt = 0.0;
	char n[500];
	sprintf(n, "%i", nodo);
	strlcpy(topic_publish, rest, sizeof(rest));	// Se deja topic_publish vacío
	strlcpy(auxTopic, topic_comun_interior, sizeof(topic_comun_interior));	// Se hace una copia de topic_comun_interior
	/* Valor "sintético" de la temperatura 
	    Valor mínimo = 18 ºC
		Valor máximo = 30 ºC 			*/	
	srand48(time(NULL));
	tempInt = drand48() * (30.1 - 17.9) + 17.9;
	//tempInt = drand48() * (25.1 - 19.9) + 19.9;
	strncat(auxTopic, n, 100); // Creación del topic en función del número de nodo que sea
	strlcpy(topic_publish, auxTopic, sizeof(auxTopic)); // Se copia en topic_publish el topic correspondiente
	strlcpy(auxTopic, rest, sizeof(rest)); // Se deja auxTopic vacío
	return tempInt;
} 

/* Obtención de manera sintética la Temperatura ambiente exterior actual*/
float temperaturaExterior(int nodo){

	float tempExt = 0.0;
	char n[500];
	sprintf(n, "%i", nodo);
	strlcpy(topic_publish, rest, sizeof(rest));	// Se deja topic_publish vacío
	strlcpy(auxTopic, topic_comun_exterior, sizeof(topic_comun_exterior)); // Se hace una copia de topic_comun_interior
	/* Valor "sintético" de la temperatura 
	    Valor mínimo = 15 ºC
		Valor máximo = 35 ºC 			*/
	srand48(time(NULL));
	tempExt = drand48() * (35.1 - 14.9) + 14.9;
	strncat(auxTopic, n, 100); // Creación del topic en función del número de nodo que sea
	strlcpy(topic_publish, auxTopic, sizeof(auxTopic)); // Se copia en topic_publish el topic correspondiente
	strlcpy(auxTopic, rest, sizeof(rest)); // Se deja auxTopic vacío
	return tempExt;
} 

/* Ciente BLE Mesh */

static void example_ble_mesh_set_msg_common(esp_ble_mesh_client_common_param_t *common,
                                            esp_ble_mesh_node_t *node,
                                            esp_ble_mesh_model_t *model, uint32_t opcode)
{
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = prov_key.net_idx;
    common->ctx.app_idx = prov_key.app_idx;
    common->ctx.addr = node->unicast_addr;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->ctx.send_rel = MSG_SEND_REL;
    common->msg_timeout = MSG_TIMEOUT;
    common->msg_role = MSG_ROLE;
}

static esp_err_t prov_complete(uint16_t node_index, const esp_ble_mesh_octet16_t uuid,
                               uint16_t primary_addr, uint8_t element_num, uint16_t net_idx)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get = {0};
    esp_ble_mesh_node_t *node = NULL;
    char name[11] = {'\0'};
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "node_index %u, primary_addr 0x%04x, element_num %u, net_idx 0x%03x",
        node_index, primary_addr, element_num, net_idx);
    ESP_LOG_BUFFER_HEX("uuid", uuid, ESP_BLE_MESH_OCTET16_LEN);

    server_address[nodes++] = primary_addr;

    /* Nodo aprovisionado */
    sprintf(prov_msg, "%d+%02x:%02x:%02x:%02x:%02x:%02x", nodes, uuid[2] & 0xff, uuid[3] & 0xff, uuid[4] & 0xff, uuid[5] & 0xff, uuid[6] & 0xff, uuid[7] & 0xff);
    esp_mqtt_client_publish(client, "/aprovisionado/nodo", prov_msg, 0, 2, 0);
    ESP_LOGI(TAG_mqtt, "sent provisioning successful... ");


    sprintf(name, "%s%02x", "NODE-", node_index);
    err = esp_ble_mesh_provisioner_set_node_name(node_index, name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set node name");
        return ESP_FAIL;
    }

    node = esp_ble_mesh_provisioner_get_node_with_addr(primary_addr);
    if (node == NULL) {
        ESP_LOGE(TAG, "Failed to get node 0x%04x info", primary_addr);
        return ESP_FAIL;
    }

    example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get.comp_data_get.page = COMP_DATA_PAGE_0;
    err = esp_ble_mesh_config_client_get_state(&common, &get);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Config Composition Data Get");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    esp_err_t err = ESP_OK;

    /* Due to the API esp_ble_mesh_provisioner_set_dev_uuid_match, Provisioner will only
     * use this callback to report the devices, whose device UUID starts with 0xdd & 0xdd,
     * to the application layer.
     */

    ESP_LOG_BUFFER_HEX("Device address", addr, BD_ADDR_LEN);
    ESP_LOGI(TAG, "Address type 0x%02x, adv type 0x%02x", addr_type, adv_type);
    ESP_LOG_BUFFER_HEX("Device UUID", dev_uuid, ESP_BLE_MESH_OCTET16_LEN);
    ESP_LOGI(TAG, "oob info 0x%04x, bearer %s", oob_info, (bearer & ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");

    memcpy(add_dev.addr, addr, BD_ADDR_LEN);
    add_dev.addr_type = (uint8_t)addr_type;
    memcpy(add_dev.uuid, dev_uuid, ESP_BLE_MESH_OCTET16_LEN);
    add_dev.oob_info = oob_info;
    add_dev.bearer = (uint8_t)bearer;
    /* Note: If unprovisioned device adv packets have not been received, we should not add
             device with ADD_DEV_START_PROV_NOW_FLAG set. */
    err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
            ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning device");
    }
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT, err_code %d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT, err_code %d", param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT");
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid, param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type, param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type, param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT, bearer %s",
            param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT, bearer %s, reason 0x%02x",
            param->provisioner_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT", param->provisioner_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        prov_complete(param->provisioner_prov_complete.node_idx, param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr, param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT, err_code %d", param->provisioner_add_unprov_dev_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT, err_code %d", param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT, err_code %d", param->provisioner_set_node_name_comp.err_code);
        if (param->provisioner_set_node_name_comp.err_code == 0) {
            const char *name = esp_ble_mesh_provisioner_get_node_name(param->provisioner_set_node_name_comp.node_index);
            if (name) {
                ESP_LOGI(TAG, "Node %d name %s", param->provisioner_set_node_name_comp.node_index, name);
            }
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT, err_code %d", param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == 0) {
            prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, prov_key.app_idx,
                                ESP_BLE_MESH_MODEL_ID_SENSOR_CLI, CID_NVAL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to bind AppKey to sensor client");
            }
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT, err_code %d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_STORE_NODE_COMP_DATA_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_STORE_NODE_COMP_DATA_COMP_EVT, err_code %d", param->provisioner_store_node_comp_data_comp.err_code);
        break;
    default:
        break;
    }
}

static void example_ble_mesh_parse_node_comp_data(const uint8_t *data, uint16_t length)
{
    uint16_t cid, pid, vid, crpl, feat;
    uint16_t loc, model_id, company_id;
    uint8_t nums, numv;
    uint16_t offset;
    int i;

    cid = COMP_DATA_2_OCTET(data, 0);
    pid = COMP_DATA_2_OCTET(data, 2);
    vid = COMP_DATA_2_OCTET(data, 4);
    crpl = COMP_DATA_2_OCTET(data, 6);
    feat = COMP_DATA_2_OCTET(data, 8);
    offset = 10;

    ESP_LOGI(TAG, "********************** Composition Data Start **********************");
    ESP_LOGI(TAG, "* CID 0x%04x, PID 0x%04x, VID 0x%04x, CRPL 0x%04x, Features 0x%04x *", cid, pid, vid, crpl, feat);
    for (; offset < length; ) {
        loc = COMP_DATA_2_OCTET(data, offset);
        nums = COMP_DATA_1_OCTET(data, offset + 2);
        numv = COMP_DATA_1_OCTET(data, offset + 3);
        offset += 4;
        ESP_LOGI(TAG, "* Loc 0x%04x, NumS 0x%02x, NumV 0x%02x *", loc, nums, numv);
        for (i = 0; i < nums; i++) {
            model_id = COMP_DATA_2_OCTET(data, offset);
            ESP_LOGI(TAG, "* SIG Model ID 0x%04x *", model_id);
            offset += 2;
        }
        for (i = 0; i < numv; i++) {
            company_id = COMP_DATA_2_OCTET(data, offset);
            model_id = COMP_DATA_2_OCTET(data, offset + 2);
            ESP_LOGI(TAG, "* Vendor Model ID 0x%04x, Company ID 0x%04x *", model_id, company_id);
            offset += 4;
        }
    }
    ESP_LOGI(TAG, "*********************** Composition Data End ***********************");
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    static uint16_t wait_model_id, wait_cid;
    esp_ble_mesh_node_t *node = NULL;
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Config client, event %u, addr 0x%04x, opcode 0x%04x",
        event, param->params->ctx.addr, param->params->opcode);

    if (param->error_code) {
        ESP_LOGE(TAG, "Send config client message failed (err %d)", param->error_code);
        return;
    }

    node = esp_ble_mesh_provisioner_get_node_with_addr(param->params->ctx.addr);
    if (!node) {
        ESP_LOGE(TAG, "Node 0x%04x not exists", param->params->ctx.addr);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            ESP_LOG_BUFFER_HEX("Composition data", param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            example_ble_mesh_parse_node_comp_data(param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            err = esp_ble_mesh_provisioner_store_node_comp_data(param->params->ctx.addr,
                param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store node composition data");
                break;
            }

            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set.app_key_add.net_idx = prov_key.net_idx;
            set.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set.app_key_add.app_key, prov_key.app_key, ESP_BLE_MESH_OCTET16_LEN);
            err = esp_ble_mesh_config_client_set_state(&common, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config AppKey Add");
            }
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set.model_app_bind.element_addr = node->unicast_addr;
            set.model_app_bind.model_app_idx = prov_key.app_idx;
            set.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_SENSOR_SRV;
            set.model_app_bind.company_id = CID_NVAL;
            err = esp_ble_mesh_config_client_set_state(&common, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config Model App Bind");
                return;
            }
            wait_model_id = ESP_BLE_MESH_MODEL_ID_SENSOR_SRV;
            wait_cid = CID_NVAL;
        } else if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            if (param->status_cb.model_app_status.model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_SRV &&
                param->status_cb.model_app_status.company_id == CID_NVAL) {
                example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
                set.model_app_bind.element_addr = node->unicast_addr;
                set.model_app_bind.model_app_idx = prov_key.app_idx;
                set.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_SENSOR_SETUP_SRV;
                set.model_app_bind.company_id = CID_NVAL;
                err = esp_ble_mesh_config_client_set_state(&common, &set);
                if (err) {
                    ESP_LOGE(TAG, "Failed to send Config Model App Bind");
                    return;
                }
                wait_model_id = ESP_BLE_MESH_MODEL_ID_SENSOR_SETUP_SRV;
                wait_cid = CID_NVAL;
            } else if (param->status_cb.model_app_status.model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_SETUP_SRV &&
                param->status_cb.model_app_status.company_id == CID_NVAL) {
                ESP_LOGW(TAG, "Provision and config successfully");
            }
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS) {
            ESP_LOG_BUFFER_HEX("Composition data", param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        switch (param->params->opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
            esp_ble_mesh_cfg_client_get_state_t get = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
            get.comp_data_get.page = COMP_DATA_PAGE_0;
            err = esp_ble_mesh_config_client_get_state(&common, &get);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config Composition Data Get");
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set.app_key_add.net_idx = prov_key.net_idx;
            set.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set.app_key_add.app_key, prov_key.app_key, ESP_BLE_MESH_OCTET16_LEN);
            err = esp_ble_mesh_config_client_set_state(&common, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config AppKey Add");
            }
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set.model_app_bind.element_addr = node->unicast_addr;
            set.model_app_bind.model_app_idx = prov_key.app_idx;
            set.model_app_bind.model_id = wait_model_id;
            set.model_app_bind.company_id = wait_cid;
            err = esp_ble_mesh_config_client_set_state(&common, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config Model App Bind");
            }
            break;
        default:
            break;
        }
        break;
    default:
        ESP_LOGE(TAG, "Invalid config client event %u", event);
        break;
    }
}

void example_ble_mesh_send_sensor_message(uint32_t opcode)
{
    esp_ble_mesh_sensor_client_get_state_t get = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_node_t *node = NULL;
    esp_err_t err = ESP_OK;
    int i;

    for ( i = 0; i < nodes; i++ )
    {

        node = esp_ble_mesh_provisioner_get_node_with_addr(server_address[i]);
        if (node == NULL) {
            ESP_LOGE(TAG, "Node 0x%04x not exists", server_address[i]);
            return;
        }
    
        example_ble_mesh_set_msg_common(&common, node, sensor_client.model, opcode);
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_SENSOR_CADENCE_GET:
            get.cadence_get.property_id = sensor_prop_id;
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTINGS_GET:
            get.settings_get.sensor_property_id = sensor_prop_id;
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SERIES_GET:
            get.series_get.property_id = sensor_prop_id;
            break;
        default:
            break;
        }

        if(i != 0)
        	num_nodo ++;
        else
        	num_nodo = 1;
    
        err = esp_ble_mesh_sensor_client_get_state(&common, &get);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send sensor message 0x%04x", opcode);
        }

		// Wait 3 seconds.
        ESP_LOGI(TAG, "Waiting a bit...");
		vTaskDelay(3000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Done...");
    }
}

static void example_ble_mesh_sensor_timeout(uint32_t opcode)
{
    switch (opcode) {
    case ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET:
        ESP_LOGW(TAG, "Sensor Descriptor Get timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_CADENCE_GET:
        ESP_LOGW(TAG, "Sensor Cadence Get timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_CADENCE_SET:
        ESP_LOGW(TAG, "Sensor Cadence Set timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTINGS_GET:
        ESP_LOGW(TAG, "Sensor Settings Get timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTING_GET:
        ESP_LOGW(TAG, "Sensor Setting Get timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTING_SET:
        ESP_LOGW(TAG, "Sensor Setting Set timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_GET:
        ESP_LOGW(TAG, "Sensor Get timeout 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_COLUMN_GET:
        ESP_LOGW(TAG, "Sensor Column Get timeout, opcode 0x%04x", opcode);
        break;
    case ESP_BLE_MESH_MODEL_OP_SENSOR_SERIES_GET:
        ESP_LOGW(TAG, "Sensor Series Get timeout, opcode 0x%04x", opcode);
        break;
    default:
        ESP_LOGE(TAG, "Unknown Sensor Get/Set opcode 0x%04x", opcode);
        return;
    }

    example_ble_mesh_send_sensor_message(opcode);
}

static void example_ble_mesh_sensor_client_cb(esp_ble_mesh_sensor_client_cb_event_t event,
                                              esp_ble_mesh_sensor_client_cb_param_t *param)
{
    esp_ble_mesh_node_t *node = NULL;

    ESP_LOGI(TAG, "Sensor client, event %u, addr 0x%04x", event, param->params->ctx.addr);

    if (param->error_code) {
        ESP_LOGE(TAG, "Send sensor client message failed (err %d)", param->error_code);
        return;
    }

    node = esp_ble_mesh_provisioner_get_node_with_addr(param->params->ctx.addr);
    if (!node) {
        ESP_LOGE(TAG, "Node 0x%04x not exists", param->params->ctx.addr);
        return;
    }

    float tempInt, tempExt;

    switch (event) {
    case ESP_BLE_MESH_SENSOR_CLIENT_GET_STATE_EVT:
        switch (param->params->opcode) {
        case ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET:
            ESP_LOGI(TAG, "Sensor Descriptor Status, opcode 0x%04x", param->params->ctx.recv_op);
            if (param->status_cb.descriptor_status.descriptor->len != ESP_BLE_MESH_SENSOR_SETTING_PROPERTY_ID_LEN &&
                param->status_cb.descriptor_status.descriptor->len % ESP_BLE_MESH_SENSOR_DESCRIPTOR_LEN) {
                ESP_LOGE(TAG, "Invalid Sensor Descriptor Status length %d", param->status_cb.descriptor_status.descriptor->len);
                return;
            }
            if (param->status_cb.descriptor_status.descriptor->len) {
                ESP_LOG_BUFFER_HEX("Sensor Descriptor", param->status_cb.descriptor_status.descriptor->data,
                    param->status_cb.descriptor_status.descriptor->len);
                /* If running with sensor server example, sensor client can get two Sensor Property IDs.
                 * Currently we use the first Sensor Property ID for the following demonstration.
                 */
                sensor_prop_id = param->status_cb.descriptor_status.descriptor->data[1] << 8 |
                                 param->status_cb.descriptor_status.descriptor->data[0];
            }
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_CADENCE_GET:
            ESP_LOGI(TAG, "Sensor Cadence Status, opcode 0x%04x, Sensor Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.cadence_status.property_id);
            ESP_LOG_BUFFER_HEX("Sensor Cadence", param->status_cb.cadence_status.sensor_cadence_value->data,
                param->status_cb.cadence_status.sensor_cadence_value->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTINGS_GET:
            ESP_LOGI(TAG, "Sensor Settings Status, opcode 0x%04x, Sensor Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.settings_status.sensor_property_id);
            ESP_LOG_BUFFER_HEX("Sensor Settings", param->status_cb.settings_status.sensor_setting_property_ids->data,
                param->status_cb.settings_status.sensor_setting_property_ids->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTING_GET:
            ESP_LOGI(TAG, "Sensor Setting Status, opcode 0x%04x, Sensor Property ID 0x%04x, Sensor Setting Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.setting_status.sensor_property_id,
                param->status_cb.setting_status.sensor_setting_property_id);
            if (param->status_cb.setting_status.op_en) {
                ESP_LOGI(TAG, "Sensor Setting Access 0x%02x", param->status_cb.setting_status.sensor_setting_access);
                ESP_LOG_BUFFER_HEX("Sensor Setting Raw", param->status_cb.setting_status.sensor_setting_raw->data,
                    param->status_cb.setting_status.sensor_setting_raw->len);
            }
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_GET:
            ESP_LOGI(TAG, "Sensor Status, opcode 0x%04x", param->params->ctx.recv_op);
            if (param->status_cb.sensor_status.marshalled_sensor_data->len) {
                ESP_LOG_BUFFER_HEX("Sensor Data", param->status_cb.sensor_status.marshalled_sensor_data->data,
                    param->status_cb.sensor_status.marshalled_sensor_data->len);
                uint8_t *data = param->status_cb.sensor_status.marshalled_sensor_data->data;
                uint16_t length = 0;
                for (; length < param->status_cb.sensor_status.marshalled_sensor_data->len; ) {
                    uint8_t fmt = ESP_BLE_MESH_GET_SENSOR_DATA_FORMAT(data);
                    uint8_t data_len = ESP_BLE_MESH_GET_SENSOR_DATA_LENGTH(data, fmt);
                    uint16_t prop_id = ESP_BLE_MESH_GET_SENSOR_DATA_PROPERTY_ID(data, fmt);
                    uint8_t mpid_len = (fmt == ESP_BLE_MESH_SENSOR_DATA_FORMAT_A ?
                                        ESP_BLE_MESH_SENSOR_DATA_FORMAT_A_MPID_LEN : ESP_BLE_MESH_SENSOR_DATA_FORMAT_B_MPID_LEN);
                    ESP_LOGI(TAG, "Format %s, length 0x%02x, Sensor Property ID 0x%04x",
                        fmt == ESP_BLE_MESH_SENSOR_DATA_FORMAT_A ? "A" : "B", data_len, prop_id);
                    if (data_len != ESP_BLE_MESH_SENSOR_DATA_ZERO_LEN) {
                        ESP_LOG_BUFFER_HEX("Sensor Data", data + mpid_len, data_len + 1);
                        
                        /*ID asociada a la propiedad de Temperatura interior*/
                        if (prop_id == 0x56){
                        	ESP_LOGI(TAG_mqtt, "Numero de nodo: %i", num_nodo);
                        	tempInt = temperaturaInterior(num_nodo);
                        	ESP_LOGI(TAG_mqtt, "Temperatura interior: %.1f ºC", tempInt);
                        	sprintf(mensaje, "%.1f", tempInt);
                        	esp_mqtt_client_publish(client, topic_publish, mensaje, 0, 2, 0);
                        	ESP_LOGI(TAG_mqtt, "sent publish successful... ");
                        }
                        /*ID asociada a la propiedad de Temperatura exterior*/
                        else if (prop_id == 0x5B){
                        	ESP_LOGI(TAG_mqtt, "Numero de nodo: %i", num_nodo);
                        	tempExt = temperaturaExterior(num_nodo);
                        	ESP_LOGI(TAG_mqtt, "Temperatura exterior: %.1f ºC", tempExt);
                        	sprintf(mensaje, "%.1f", tempExt);
                        	esp_mqtt_client_publish(client, topic_publish, mensaje, 0, 2, 0);
                        	ESP_LOGI(TAG_mqtt, "sent publish successful... ");
                        }

                        length += mpid_len + data_len + 1;
                    } else {
                        length += mpid_len;
                    }
                    data += length;
                }
            }
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_COLUMN_GET:
            ESP_LOGI(TAG, "Sensor Column Status, opcode 0x%04x, Sensor Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.column_status.property_id);
            ESP_LOG_BUFFER_HEX("Sensor Column", param->status_cb.column_status.sensor_column_value->data,
                param->status_cb.column_status.sensor_column_value->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SERIES_GET:
            ESP_LOGI(TAG, "Sensor Series Status, opcode 0x%04x, Sensor Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.series_status.property_id);
            ESP_LOG_BUFFER_HEX("Sensor Series", param->status_cb.series_status.sensor_series_value->data,
                param->status_cb.series_status.sensor_series_value->len);
            break;
        default:
            ESP_LOGE(TAG, "Unknown Sensor Get opcode 0x%04x", param->params->ctx.recv_op);
            break;
        }
        break;
    case ESP_BLE_MESH_SENSOR_CLIENT_SET_STATE_EVT:
        switch (param->params->opcode) {
        case ESP_BLE_MESH_MODEL_OP_SENSOR_CADENCE_SET:
            ESP_LOGI(TAG, "Sensor Cadence Status, opcode 0x%04x, Sensor Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.cadence_status.property_id);
            ESP_LOG_BUFFER_HEX("Sensor Cadence", param->status_cb.cadence_status.sensor_cadence_value->data,
                param->status_cb.cadence_status.sensor_cadence_value->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_SENSOR_SETTING_SET:
            ESP_LOGI(TAG, "Sensor Setting Status, opcode 0x%04x, Sensor Property ID 0x%04x, Sensor Setting Property ID 0x%04x",
                param->params->ctx.recv_op, param->status_cb.setting_status.sensor_property_id,
                param->status_cb.setting_status.sensor_setting_property_id);
            if (param->status_cb.setting_status.op_en) {
                ESP_LOGI(TAG, "Sensor Setting Access 0x%02x", param->status_cb.setting_status.sensor_setting_access);
                ESP_LOG_BUFFER_HEX("Sensor Setting Raw", param->status_cb.setting_status.sensor_setting_raw->data,
                    param->status_cb.setting_status.sensor_setting_raw->len);
            }
            break;
        default:
            ESP_LOGE(TAG, "Unknown Sensor Set opcode 0x%04x", param->params->ctx.recv_op);
            break;
        }
        break;
    case ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT:
        break;
    case ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT:
        example_ble_mesh_sensor_timeout(param->params->opcode);
    default:
        break;
    }
}

static esp_err_t ble_mesh_init(void)
{
    uint8_t match[2] = { 0x32, 0x10 };
    esp_err_t err = ESP_OK;

    prov_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    prov_key.app_idx = APP_KEY_IDX;
    memset(prov_key.app_key, APP_KEY_OCTET, sizeof(prov_key.app_key));

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_sensor_client_callback(example_ble_mesh_sensor_client_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack");
        return err;
    }

    err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set matching device uuid");
        return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh provisioner");
        return err;
    }

    err = esp_ble_mesh_provisioner_add_local_app_key(prov_key.app_key, prov_key.net_idx, prov_key.app_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add local AppKey");
        return err;
    }

    ESP_LOGI(TAG, "BLE Mesh sensor client initialized");

    return ESP_OK;
}

/* WiFi */

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_wifi, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG_wifi,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_wifi, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG_wifi, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_wifi, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_wifi, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG_wifi, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/* MQTT */

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    int n_nodo;
    float tempInt, tempExt;

    switch (event->event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_mqtt, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "/solicitud/tempInt", 2);
            ESP_LOGI(TAG_mqtt, "sent subscribe successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, "/solicitud/tempExt", 2);
            ESP_LOGI(TAG_mqtt, "sent subscribe successful, msg_id=%d", msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_mqtt, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            break;

        case MQTT_EVENT_PUBLISHED:
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG_mqtt, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG_mqtt, "Nueva solicitud: ");
            printf("TOPIC = %.*s\r\n", event->topic_len, event->topic);
            printf("NODO = %.*s\r\n", event->data_len, event->data);
            /* Topic Recibido ...*/
            strlcpy(topic_sub, rest, sizeof(rest)); // Se deja topic_sub vacío
            strlcpy(topic_sub, event->topic, 19);
            ESP_LOGI(TAG_mqtt, "Confirmación Topic: %s", topic_sub);
            /* Solicitud desde el nodo... */
			strlcpy(solic_nodo, rest, sizeof(rest)); // Se deja solic_nodo vacío
            strlcpy(solic_nodo, event->data, 2);
            ESP_LOGI(TAG_mqtt, "Confirmación Nodo: %s", solic_nodo);
            n_nodo = atoi(solic_nodo);
            /* Temperatura Interna*/
            if(strcmp(topic_sub, "/solicitud/tempInt") == 0) {
            tempInt = temperaturaInterior(n_nodo);
            ESP_LOGI(TAG_mqtt, "Temperatura interior: %.1f ºC", tempInt);
            sprintf(mensaje, "%.1f", tempInt);
            esp_mqtt_client_publish(client, topic_publish, mensaje, 0, 2, 0);
            ESP_LOGI(TAG_mqtt, "sent publish successful... ");
       		}
       		else if(strcmp(topic_sub, "/solicitud/tempExt") == 0) {
       		tempExt = temperaturaExterior(n_nodo);
            ESP_LOGI(TAG_mqtt, "Temperatura exterior: %.1f ºC", tempExt);
            sprintf(mensaje, "%.1f", tempExt);
            esp_mqtt_client_publish(client, topic_publish, mensaje, 0, 2, 0);
            ESP_LOGI(TAG_mqtt, "sent publish successful... ");
       		}
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG_mqtt, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
    ESP_LOGI(TAG_mqtt, "MQTT_CHECK");
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG_mqtt, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Initializing...");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    board_init();

    err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    nodes = 0;

    err = ble_mesh_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
    }

    /* Wifi */
    ESP_LOGI(TAG_wifi, "ESP_WIFI_MODE_STA");
    pthread_t t1;
    pthread_create(&t1, NULL, (void*) wifi_init_sta, NULL);
    
    /* MQTT */
    ESP_LOGI(TAG_mqtt, "MQTT_CLIENT");
    pthread_t t2;
    pthread_create(&t2, NULL, (void*) mqtt_app_start, NULL);

}
