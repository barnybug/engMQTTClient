/*
The MIT License (MIT)

Copyright (c) 2016 gpbenton

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <bcm2835.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <log4c.h>
#include <mosquitto.h>
#include <sys/queue.h>
#include <semaphore.h>
#include "ctype.h"
#include "engMQTTClient.h"
#include "dev_HRF.h"
#include "decoder.h"

/* MQTT Definitions */

#define MQTT_TOPIC_BASE "energenie"

/* eTRV Topics */
#define MQTT_TOPIC_ETRV       "eTRV"
#define MQTT_TOPIC_COMMAND    "Command"
#define MQTT_TOPIC_REPORT     "Report"

#define MQTT_TOPIC_ETRV_COMMAND "/" MQTT_TOPIC_BASE "/" MQTT_TOPIC_ETRV "/" MQTT_TOPIC_COMMAND
#define MQTT_TOPIC_ETRV_REPORT  "/" MQTT_TOPIC_BASE "/" MQTT_TOPIC_ETRV "/" MQTT_TOPIC_REPORT

#define MQTT_TOPIC_TEMPERATURE  "Temperature"
#define MQTT_TOPIC_IDENTIFY     "Identify"

#define MQTT_TOPIC_RCVD_TEMP_COMMAND MQTT_TOPIC_ETRV_COMMAND "/" MQTT_TOPIC_TEMPERATURE
#define MQTT_TOPIC_SENT_TEMP_REPORT  MQTT_TOPIC_ETRV_REPORT "/" MQTT_TOPIC_TEMPERATURE

#define MQTT_TOPIC_MAX_SENSOR_LENGTH  8          // length of string of largest sensorId
                                                 // 16777215 (0xffffff)
/* ENER002 Topics */
#define MQTT_TOPIC_ENER002    "ENER002"
#define MQTT_TOPIC_ENER002_COMMAND     "/" MQTT_TOPIC_BASE "/" MQTT_TOPIC_ENER002

static const char* mqttBrokerHost =  "localhost";  // TODO configuration for other set ups
static const int   mqttBrokerPort = 1883;
static const int keepalive = 60;
static const bool clean_session = true;

/* OpenThings definitions */
static const uint8_t engManufacturerId = 0x04;   // Energenie Manufacturer Id
static const uint8_t eTRVProductId = 0x3;        // Product ID for eTRV
static const uint8_t encryptId = 0xf2;           // Encryption ID for eTRV

static int err = 0;

static pthread_mutext_t sensorListMutex;
static LIST_HEAD(listhead, entry) sensorListHead;

struct listhead  *headp;

struct entry {
    LIST_ENTRY(entry) entries;
    int sensorId;

};

enum fail_codes {
    ERROR_LOG4C_INIT=1,
    ERROR_MOSQ_NEW,
    ERROR_MOSQ_CONNECT,
    ERROR_MOSQ_LOOP_START,
    ERROR_ENER_INIT_FAIL
};


static log4c_category_t* clientlog = NULL;
static log4c_category_t* stacklog = NULL;
log4c_category_t* hrflog = NULL;


/* Converts hex string in hex to equivalent bytes
 * in bytes.
 * No input checking performed.  hex is expected
 * to contain the exact number of characters 
 * intended for output.
 */
void hexToBytes(uint8_t *bytes, char *hex) {

    int hexlength = strlen(hex);
    int bytelength = hexlength/2;
    int i;

    for (i = 0; i < hexlength; ++i) {
        int high = hex[i*2];
        int low = hex[(i*2) + 1];
        high = (high & 0xf) + ((high & 0x40) >> 6) * 9;
        low = (low & 0xf) + ((low & 0x40) >> 6) * 9;

        bytes[i] = (high << 4) | low;
    }

    if (log4c_category_is_trace_enabled(clientlog)) {
        int HEX_LOG_BUF_SIZE =  bytelength * 8;
        char logBuffer[HEX_LOG_BUF_SIZE];
        
        int logBufferUsedCount = 0;

        for (i=0; i < bytelength; ++i) {
            logBufferUsedCount += 
                snprintf(&logBuffer[logBufferUsedCount],
                         HEX_LOG_BUF_SIZE - logBufferUsedCount,
                         "[%d]=%02x%c", i, bytes[i], i%8==7?'\n':'\t');
        }

        logBuffer[HEX_LOG_BUF_SIZE - 1] = '\0';

        log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE, 
                           "Converted %s to \n%s",
                           hex,
                           logBuffer);
    }
}

void my_message_callback(struct mosquitto *mosq, void *userdata, 
                         const struct mosquitto_message *message)
{
    char **topics;
    int topic_count;
    int i;

    log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE, "%s", __FUNCTION__);

    if (mosquitto_sub_topic_tokenise(message->topic, &topics, &topic_count) 
        != 
        MOSQ_ERR_SUCCESS) {
        log4c_category_error(clientlog, "Unable to tokenise topic");
        return;
    }

    if (log4c_category_is_trace_enabled(clientlog)) {
        for (i=0; i<topic_count; i++) {
            log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE,
                               "%d: %s", i, topics[i]);
        }
    }

    if (topic_count < 2) {
        log4c_category_error(clientlog, "Invalid Topic count %d", topic_count);
        mosquitto_sub_topic_tokens_free(&topics, topic_count);
        return;
    }

    if (strcmp(MQTT_TOPIC_BASE, topics[1]) != 0) {
        log4c_category_error(clientlog, "Received base topic %s", topics[1]);
        mosquitto_sub_topic_tokens_free(&topics, topic_count);
        return;
    }

    if (strcmp(MQTT_TOPIC_ENER002, topics[2]) == 0) {
        // Message for plug in socket type ENER002

        if (topic_count != 5) {
            log4c_category_error(clientlog, "Invalid topic count(%d) for %s", 
                                 topic_count,
                                 MQTT_TOPIC_ENER002);
            mosquitto_sub_topic_tokens_free(&topics, topic_count);
            return;
        }

#define MAX_OOK_ADDRESS_TOPIC_LENGTH (OOK_MSG_ADDRESS_LENGTH * 2)
#define MAX_OOK_DEVICE_TOPIC_LENGTH  1

        char address[MAX_OOK_ADDRESS_TOPIC_LENGTH + 1];
        char device[MAX_OOK_DEVICE_TOPIC_LENGTH + 1];

        strncpy(address, topics[3], MAX_OOK_ADDRESS_TOPIC_LENGTH);
        address[MAX_OOK_ADDRESS_TOPIC_LENGTH] = '\0';
        strncpy(device, topics[4], MAX_OOK_DEVICE_TOPIC_LENGTH);
        device[MAX_OOK_DEVICE_TOPIC_LENGTH] = '\0';

        log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE, "Freeing tokens");

        mosquitto_sub_topic_tokens_free(&topics, topic_count);

        log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE, "Tokens Freed");


        if(message->payloadlen == 0) {
            log4c_category_error(clientlog, "No Payload for %s", 
                                 MQTT_TOPIC_ENER002);
            return;
        }

        int onOff;
        int socketNum;

        if (strcasecmp("On", message->payload) == 0) {
            onOff = 1;
        } else if (strcasecmp("Off", message->payload) == 0) {
            onOff = 0;
        } else {
            log4c_category_error(clientlog, "Invalid Payload for %s", 
                                 MQTT_TOPIC_ENER002);
            return;
        }

        socketNum = *device - '0';
        if (socketNum < 0 || socketNum > 4) {
            log4c_category_error(clientlog, "Invalid socket number: %d",
                                 socketNum);
            return;
        }

        /* Check we only have hex digits in address */
        int i;
        for (i = 0; i < strlen(address); i++) {
            if (isxdigit(address[i]) == 0) {
                log4c_category_error(clientlog, 
                                     "Address must only contain hex digits: %s",
                                     address);
                return;
            }
        }

        uint8_t addressBytes[OOK_MSG_ADDRESS_LENGTH];
        int paddedAddressLength = OOK_MSG_ADDRESS_LENGTH * 2;
        int j;
        char addressPadded[paddedAddressLength + 1];

        // Pad the address with 0
        for (i = 0; i < paddedAddressLength - strlen(address); i++) {
            addressPadded[i] = '0';
        }
        for (j= 0; j < strlen(address); i++, j++) {
            addressPadded[i] = address[j];
        }
        addressPadded[paddedAddressLength] = '\0'; 

        // Convert to bytes
        hexToBytes(addressBytes, addressPadded);

        log4c_category_debug(clientlog, "Sending %d to address:socket %s:%d",
                             onOff, addressPadded, socketNum);

        HRF_send_OOK_msg(addressBytes, socketNum, onOff);

    } else if (strcmp(MQTT_TOPIC_ETRV, topics[2]) == 0) {
        // Message for eTRV Radiator Valve
        
        if (topic_count < 5) {
            log4c_category_error(clientlog, "Invalid topic count(%d) for %s", 
                                 topic_count,
                                 MQTT_TOPIC_ETRV);
            mosquitto_sub_topic_tokens_free(&topics, topic_count);
            return;
        }
        

        if (strcmp(MQTT_TOPIC_COMMAND, topics[3]) != 0) {
            log4c_category_error(clientlog, "Invalid command %s for %s", topics[3], topics[2]);
            mosquitto_sub_topic_tokens_free(&topics, topic_count);
            return;
        }


        if (strcmp(MQTT_TOPIC_IDENTIFY, topics[4]) == 0) {
            // Send Identify command to eTRV
            //
            char sensorId[MQTT_TOPIC_MAX_SENSOR_LENGTH + 1];
            strncpy(sensorId, topics[5], MQTT_TOPIC_MAX_SENSOR_LENGTH);
            sensorId[MQTT_TOPIC_MAX_SENSOR_LENGTH] = '\0';

            mosquitto_sub_topic_tokens_free(&topics, topic_count);

            int intSensorId = atoi(sensorId);

            if (intSensorId == 0) {
                // Assume 0 isn't valid sensor id
                log4c_category_error(clientlog, "SensorId must be an integer: %s", sensorId);
                return;
            }

            log4c_category_info(clientlog, "Sending Identify to %s", sensorId);

            HRF_send_FSK_msg(HRF_make_FSK_msg(engManufacturerId, encryptId, 
                                              eTRVProductId, intSensorId, 2, 0xBF , 0), encryptId);

        } else {
            log4c_category_warn(clientlog, 
                           "Can't handle %s commands for %s yet", topics[4], topics[2]);
            mosquitto_sub_topic_tokens_free(&topics, topic_count);
        }


    }else{
        log4c_category_warn(clientlog, 
                           "Can't handle messages for %s yet", topics[2]);
    }

}

void my_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
    log4c_category_log(clientlog, LOG4C_PRIORITY_TRACE, "%s", __FUNCTION__);

    if(!result){
        log4c_category_log(clientlog, LOG4C_PRIORITY_NOTICE, 
                           "Connected to broker at %s", mqttBrokerHost);
        /* Subscribe to broker information topics on successful connect. */
        mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_ENER002_COMMAND "/#", 2);

        mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_ETRV_COMMAND "/#", 2);
    }else{
        log4c_category_log(clientlog, LOG4C_PRIORITY_WARN, 
                           "Connect Failed with error %d", result);
    }
}

void my_subscribe_callback(struct mosquitto *mosq, void *userdata, int mid,
                           int qos_count, const int *granted_qos)
{
    log4c_category_log(clientlog, LOG4C_PRIORITY_NOTICE, 
                       "Subscribed (mid: %d): %d", mid, granted_qos[0]);
}

void my_log_callback(struct mosquitto *mosq, void *userdata, int level, 
                     const char *str)
{
    log4c_priority_level_t priority;

    switch (level) {
        case  MOSQ_LOG_INFO:
            priority = LOG4C_PRIORITY_INFO;
            break;

        case	MOSQ_LOG_NOTICE:
            priority = LOG4C_PRIORITY_NOTICE;
            break;
        case	MOSQ_LOG_WARNING:
            priority = LOG4C_PRIORITY_WARN;
            break;

        case	MOSQ_LOG_ERR:
            priority = LOG4C_PRIORITY_ERROR;
            break;

        case	MOSQ_LOG_DEBUG:
            priority = LOG4C_PRIORITY_DEBUG;
            break;

        default:
            priority = LOG4C_PRIORITY_WARN;
            break;
    }

    log4c_category_log(stacklog, priority, "%s", str);
}

// receive in variable length packet mode, display and resend. Data with swapped first 2 bytes
int main(int argc, char **argv){
    		
    extern	uint8_t recieve_temp_report;
    extern char received_temperature[MAX_DATA_LENGTH];
    struct mosquitto *mosq = NULL;
	
    if (log4c_init()) {
        fprintf(stderr, "log4c_init() failed");
        return ERROR_LOG4C_INIT;
    }

    clientlog = log4c_category_get("MQTTClient");
    stacklog = log4c_category_get("MQTTStack");
    hrflog = log4c_category_get("hrf");

    LIST_INIT(&sensorListHead);

	if (!bcm2835_init()) {
        log4c_category_crit(clientlog, "bcm2835_init() failed");
		return ERROR_ENER_INIT_FAIL;
    }
	
	// LED INIT
	bcm2835_gpio_fsel(greenLED, BCM2835_GPIO_FSEL_OUTP);			// LED green
	bcm2835_gpio_fsel(redLED, BCM2835_GPIO_FSEL_OUTP);			// LED red
    ledControl(greenLED, ledOff);
    ledControl(redLED, ledOn);

	// SPI INIT
	bcm2835_spi_begin();	
	bcm2835_spi_setClockDivider(SPI_CLOCK_DIVIDER_9p6MHZ); 		
	bcm2835_spi_setDataMode(BCM2835_SPI_MODE0); 				// CPOL = 0, CPHA = 0
	bcm2835_spi_chipSelect(BCM2835_SPI_CS1);					// chip select 1

	HRF_config_FSK();
	HRF_wait_for(ADDR_IRQFLAGS1, MASK_MODEREADY, TRUE);			// wait until ready after mode switching
	HRF_clr_fifo();

    mosquitto_lib_init();
    mosq = mosquitto_new("Energenie Controller", clean_session, NULL);
    if(!mosq){
        log4c_category_log(clientlog, LOG4C_PRIORITY_CRIT, "Out of memory");
        return ERROR_MOSQ_NEW;
    }
    mosquitto_log_callback_set(mosq, my_log_callback);
    mosquitto_connect_callback_set(mosq, my_connect_callback);
    mosquitto_message_callback_set(mosq, my_message_callback);
    mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);

    if((err = mosquitto_connect_async(mosq, mqttBrokerHost, mqttBrokerPort, keepalive)) 
       != MOSQ_ERR_SUCCESS){
        log4c_category_log(clientlog, LOG4C_PRIORITY_CRIT, 
                           "Unable to connect: %d", err);
        return ERROR_MOSQ_CONNECT;
    }



    if ((err = mosquitto_loop_start(mosq)) != MOSQ_ERR_SUCCESS) {
        log4c_category_log(clientlog, LOG4C_PRIORITY_CRIT,
                           "Loop start failed: %d", err);
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return ERROR_MOSQ_LOOP_START;
    }

    ledControl(redLED, ledOff);
    ledControl(greenLED, ledOn);

	while (1){
        uint32_t rcvdSensorId;
		
		HRF_receive_FSK_msg(encryptId, eTRVProductId, engManufacturerId, &rcvdSensorId );

        if (send_join_response) {
            if ( join_manu_id == engManufacturerId &&
                 join_prod_id == eTRVProductId) {

                /* We got a join request for an eTRV */
                log4c_category_debug(clientlog, "send Join response for sensorId %d", rcvdSensorId);

                HRF_send_FSK_msg(
                                 HRF_make_FSK_msg(join_manu_id, encryptId, join_prod_id, join_sensor_id,
                                                  2, PARAM_JOIN_RESP, 0), 
                                 encryptId);
                send_join_response = FALSE;
            } else {
                log4c_category_info(clientlog, 
                        "Received Join message for ManufacturerId:%d ProductId:%d SensorId:%d", 
                        join_manu_id, join_prod_id, join_sensor_id);
            }
        }

        if (recieve_temp_report) {
            log4c_category_debug(clientlog, "send NIL command for sensorId %d", rcvdSensorId);
            HRF_send_FSK_msg(HRF_make_FSK_msg(engManufacturerId, encryptId, 
                                              eTRVProductId, rcvdSensorId, 0), 
                             encryptId);
            //HRF_send_FSK_msg(HRF_make_FSK_msg(engManufacturerId, encryptId, 
                                      //eTRVProductId, rcvdSensorId, 2, 0xBF , 0), encryptId);
            log4c_category_info(clientlog, "SensorId=%d Temperature=%s", 
                                rcvdSensorId, received_temperature);

            char mqttTopic[strlen(MQTT_TOPIC_SENT_TEMP_REPORT) 
                            + MQTT_TOPIC_MAX_SENSOR_LENGTH 
                            + 5 + 1];

            snprintf(mqttTopic, sizeof(mqttTopic), "%s/%d", 
                     MQTT_TOPIC_SENT_TEMP_REPORT, rcvdSensorId);
            mosquitto_publish(mosq, NULL, mqttTopic, 
                              strlen(received_temperature), received_temperature, 0, false);
            recieve_temp_report = FALSE;  
        }
			
/*         if (recieve_temp_report)
        {
      
            if (queued_data)
            {
                                
                printf("send temp report\n");
                HRF_send_FSK_msg(HRF_make_FSK_msg(manufacturerId, encryptId, productId, sensorId,
                                              4, PARAM_TEMP_SET, 0x92, (data & 0xff), (data >> 8 & 0xff)), encryptId);
            queued_data = FALSE;
            recieve_temp_report = FALSE;
            
          } else {
            printf("send IDENTIFY command\n");
            HRF_send_FSK_msg(HRF_make_FSK_msg(manufacturerId, encryptId, 
                                      productId, sensorId, 2, 0xBF , 0), encryptId);
            recieve_temp_report = FALSE;  
      }
      }                                                                                                                   */
		
		
	/*	if (!quiet && difftime(currentTime, monitorControlTime) > 1)
		{
			monitorControlTime = time(NULL);
			static bool switchState = false;
			switchState = !switchState;
			printf("send temp message:\trelay %s\n", switchState ? "ON" : "OFF");
			bcm2835_gpio_write(LEDG, switchState);
			HRF_send_FSK_msg(HRF_make_FSK_msg(manufacturerId, encryptId, productId, sensorId,
											  4, PARAM_TEMP_SET, 0x92, 0x10, 0x20),
							 encryptId);
		}
		*/
        usleep(10000);
	}
	bcm2835_spi_end();
	return 0;
}


/* vim: set cindent sw=4 ts=4 expandtab path+=/usr/local/include : */
