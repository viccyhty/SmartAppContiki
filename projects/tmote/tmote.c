/*
 * Copyright (c) 2011, Matthias Kovatsch and other contributors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 */

/**
 * \file
 *      Erbium (Er) REST Engine example (with CoAP-specific code)
 * \author
 *      Matthias Kovatsch <kovatsch@inf.ethz.ch>
 */

#define VERSION "0.8.3"
#define EPTYPE "Tmote-Sky"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "contiki.h"
#include "contiki-net.h"

#if !UIP_CONF_IPV6_RPL && !defined (CONTIKI_TARGET_MINIMAL_NET) && !defined (CONTIKI_TARGET_NATIVE)
#warning "Compiling with static routing!"
#include "static-routing.h"
#endif

#include "er-coap-07.h"
#include "er-coap-07-engine.h"
#include "er-coap-07-separate.h"
#include "er-coap-07-transactions.h"

#include "dev/radio-sensor.h"
#include "dev/sht11-sensor.h"

static struct etimer sht;

static int16_t rssi_value[3];
static int16_t rssi_count=0;
static int16_t rssi_position=0;
static int16_t rssi_avg=0;

static int16_t temperature=0;
static int16_t temperature_last=0;
static int16_t threshold = 20;
static uint8_t poll_time=5;

/*--------------------COAP Resources-----------------------------------------------------------*/
PERIODIC_RESOURCE(temperature, METHOD_GET, "sensors/temperature", "title=\"Temperature\";obs;rt=\"temperature\"",240*CLOCK_SECOND);
void temperature_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
	snprintf((char*)buffer, preferred_size, "%d.%02d\n",temperature/100, temperature>0 ? temperature%100 : (-1*temperature)%100);
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, buffer, strlen((char*)buffer));

}

void temperature_periodic_handler(resource_t *r) {
	static uint32_t event_i = 0;
	char content[8];

	++event_i;

  coap_packet_t notification[1]; /* This way the packet can be treated as pointer as usual. */
  coap_init_message(notification, COAP_TYPE_CON, CONTENT_2_05, 0 );
  coap_set_payload(notification, content, snprintf(content, 7, "%d.%02d\n",temperature/100, temperature>0 ? temperature%100 : (-1*temperature)%100));

	REST.notify_subscribers(r, event_i, notification);

}



/*--------- Threshold ---------------------------------------------------------*/
RESOURCE(threshold, METHOD_GET | METHOD_PUT, "config/threshold", "title=\"Threshold\";rt=\"threshold\"");
void threshold_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
	if (REST.get_method_type(request)==METHOD_GET)
	{
		snprintf((char*)buffer, preferred_size,"%d.%02d", threshold/100, threshold%100);
		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
		REST.set_response_payload(response, buffer, strlen((char*)buffer));
	}
	else {
		const uint8_t * string = NULL;
    int success = 0;
    int len = coap_get_payload(request, &string);
		uint16_t value;
		if (len == 3) {
			if (isdigit(string[0]) && isdigit(string[2]) && string[1]=='.'){
				value = (atoi((char*) string)) * 100;
				value += atoi((char*) string+2)*10;
				success = 1;
			}
		}
	  if(success){
			threshold=value;
    	REST.set_response_status(response, REST.status.CHANGED);
    }
   	else{
   		REST.set_response_status(response, REST.status.BAD_REQUEST);
   	}
	}
}

/*------------------- HeartBeat --------------------------------------------------------------------------*/
PERIODIC_RESOURCE(heartbeat, METHOD_GET, "debug/heartbeat", "title=\"Heartbeat\";obs;rt=\"heartbeat\"",60*CLOCK_SECOND);
void heartbeat_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
	snprintf((char*)buffer, preferred_size, "version:%s,uptime:%lu,rssi:%i",VERSION,clock_seconds(),rssi_avg);
 	REST.set_response_payload(response, buffer, strlen((char*)buffer));
}

void heartbeat_periodic_handler(resource_t *r){
	static uint32_t event_counter=0;
	static char	content[50];

	++event_counter;
	
	rssi_value[rssi_position]= radio_sensor.value(RADIO_SENSOR_LAST_PACKET);
	if(rssi_count<3){
		rssi_count++;
	}
	rssi_position++;
	rssi_position = (rssi_position) % 3;
	
	rssi_avg = (rssi_count>0)?(rssi_value[0]+rssi_value[1]+rssi_value[2])/rssi_count:0;
	coap_packet_t notification[1];
	coap_init_message(notification, COAP_TYPE_NON, CONTENT_2_05, 0);
	coap_set_payload(notification, content, snprintf(content, sizeof(content), "version:%s,uptime:%lu,rssi:%i",VERSION,clock_seconds(),rssi_avg));

	REST.notify_subscribers(r, event_counter, notification);

}

PROCESS(tmote_temperature, "Tmote Sky Temperature Sensor");
AUTOSTART_PROCESSES(&tmote_temperature);

PROCESS_THREAD(tmote_temperature, ev, data)
{
	PROCESS_BEGIN();

	/* if static routes are used rather than RPL */
	#if !UIP_CONF_IPV6_RPL && !defined (CONTIKI_TARGET_MINIMAL_NET) && !defined (CONTIKI_TARGET_NATIVE)
	set_global_address();
	configure_routing();
	#endif

	/* Initialize the REST engine. */
	rest_init_engine();

	SENSORS_ACTIVATE(sht11_sensor);
	SENSORS_ACTIVATE(radio_sensor);

	rest_activate_periodic_resource(&periodic_resource_temperature);
	rest_activate_resource(&resource_threshold);
	rest_activate_periodic_resource(&periodic_resource_heartbeat);
	
	etimer_set(&sht, 30*CLOCK_SECOND);

  /* Define application-specific events here. */
	while(1) {
		PROCESS_WAIT_EVENT();
		if (ev == PROCESS_EVENT_TIMER){
			if(etimer_expired(&sht)){
				temperature = -3960+sht11_sensor.value(SHT11_SENSOR_TEMP);
				if (temperature < temperature_last - threshold || temperature > temperature_last + threshold){
					temperature_last=temperature;
					temperature_periodic_handler(&resource_temperature);	
				}
				etimer_set(&sht, CLOCK_SECOND * poll_time);
			}
		}
	} /* while (1) */

	PROCESS_END();
}