#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <ctype.h>
#include "contiki-raven.h"
#include "rs232.h"
#include "ringbuf.h"

#if WITH_COAP == 3
#include "coap-03.h"
#include "coap-03-transactions.h"
#elif WITH_COAP == 6
#include "coap-06.h"
#include "coap-06-transactions.h"
#else
#error "CoAP version defined by WITH_COAP not implemented"
#endif


#include "UsefulMicropendousDefines.h"
// set up external SRAM prior to anything else to make sure malloc() has access to it
void EnableExternalSRAM (void) __attribute__ ((naked)) __attribute__ ((section (".init3")));
void EnableExternalSRAM(void)
{
  PORTE_EXT_SRAM_SETUP;  // set up control port
  ENABLE_EXT_SRAM;       // enable the external SRAM
  XMCRA = ((1 << SRE));  // enable XMEM interface with 0 wait states
  XMCRB = 0;
  SELECT_EXT_SRAM_BANK0; // select Bank 0
}


#define MAX(a,b) ((a)<(b)?(b):(a))

PROCESS(coap_process, "Coap");
PROCESS(plogg_process, "Plogg comm");


// request
#define AT_COMMAND_PREFIX "AT"
// command for info
#define AT_COMMAND_INFO AT_COMMAND_PREFIX"I"
// command for reset
#define AT_COMMAND_RESET AT_COMMAND_PREFIX"Z"
// command for hangup
#define AT_COMMAND_HANGUP AT_COMMAND_PREFIX"H"
// command for unicast: AT+UCAST:<16Byte src addr>,<response>\r\n
#define AT_COMMAND_UNICAST AT_COMMAND_PREFIX"+UCAST:"
// response when everything is ok
#define AT_RESPONSE_OK "OK"
// response when an error occured
#define AT_RESPONSE_ERROR "ERROR"

#define ISO_nl       0x0a
#define ISO_cr       0x0d


/*_________________________RS232_____________________________________________*/
/*---------------------------------------------------------------------------*/
static struct ringbuf uart_buf;
static unsigned char uart_buf_data[128] = {0};
static char state = 0;
static uint8_t poll_number = 0;

static char poll_return[128];
uint8_t poll_return_index;

static struct {
	bool activated;

 	uint16_t date_y;
 	char date_m[4];
 	uint16_t date_d;
	uint16_t time_h;
	uint16_t time_m;
	uint16_t time_s;

	uint16_t plogg_time_d;
	uint16_t plogg_time_h;
	uint16_t plogg_time_m;
	uint16_t plogg_time_s;
	uint16_t equipment_time_d;
	uint16_t equipment_time_h;
	uint16_t equipment_time_m;
	uint16_t equipment_time_s;

	long current_max_value;
 	uint16_t current_max_date_y;
	char current_max_date_m[4];
 	uint16_t current_max_date_d;
	uint16_t current_max_time_h;
	uint16_t current_max_time_m;
	uint16_t current_max_time_s;

	long voltage_max_value;
 	uint16_t voltage_max_date_y;
 	char voltage_max_date_m[4];
 	uint16_t voltage_max_date_d;
	uint16_t voltage_max_time_h;
	uint16_t voltage_max_time_m;
	uint16_t voltage_max_time_s;
	
	long watts_max_value;
 	uint16_t watts_max_date_y;
 	char watts_max_date_m[4];
 	uint16_t watts_max_date_d;
	uint16_t watts_max_time_h;
	uint16_t watts_max_time_m;
	uint16_t watts_max_time_s;

	long frequency;
	long current;
	long voltage;
	long phase_angle;

	long watts_total;
	unsigned long watts_con;
	unsigned long watts_gen;

	long power_total;
	unsigned long power_gen;
	unsigned long power_con;

	uint16_t tariff_zone;
	uint16_t tariff0_start;
	uint16_t tariff0_end;
	
	uint16_t tariff0_rate;
	uint16_t tariff1_rate;
	unsigned long tariff0_consumed;
	unsigned long tariff0_cost;
	unsigned long tariff1_consumed;
	unsigned long tariff1_cost;


	uint16_t timer0_start;
	uint16_t timer0_end;
	uint16_t timer1_start;
	uint16_t timer1_end;
	uint16_t timer2_start;
	uint16_t timer2_end;
	uint16_t timer3_start;
	uint16_t timer3_end;

} poll_data;

/*---------------------------------------------------------------------------*/
static int uart_get_char(unsigned char c)
{
  ringbuf_put(&uart_buf, c);
  if (c=='\r') ++state;
  if ((state==1 && c=='\n') || ringbuf_size(&uart_buf)==127) {
    ringbuf_put(&uart_buf, '\0');
    process_post(&plogg_process, PROCESS_EVENT_MSG, NULL);
    state = 0;
  }
  return 1;
}

static long get_signed_pseudo_float_3(char* string){
	long number=0;
	char deci = '0';
	char centi = '0';
	char mili = '0';
	sscanf_P(string,PSTR("%ld%*c%c%c%c"),&number, &deci, &centi, &mili);
	if (number< 0){
		number *= 1000;
		if (isdigit(deci)){
			number -= (deci -'0') * 100;
			if(isdigit(centi)){
				number -= (centi - '0') *10;
				if(isdigit(mili)){
					number-= (mili - '0');
				}
			}
		}
	}
	else {
		number *= 1000;
		if (isdigit(deci)){
			number += (deci -'0') * 100;
			if(isdigit(centi)){
				number += (centi - '0')*10;
				if(isdigit(mili)){
					number+= (mili - '0');
				}
			}
		}
	}
	return number;
}

static unsigned long get_unsigned_pseudo_float_3(char* string){
	unsigned long number=0;
	char deci = '0';
	char centi = '0';
	char mili = '0';
	sscanf_P(string,PSTR("%lu%*c%c%c%c"),&number, &deci, &centi, &mili);
	number *= 1000;
	if (isdigit(deci)){
		number += (deci -'0') * 100;
		if(isdigit(centi)){
			number += (centi - '0') * 10;
			if(isdigit(mili)){
				number+= (mili - '0');
			}
		}
	}
	return number;
}

static void parse_Poll(){

	//printf("%s\r\n",poll_return);

	if( strncmp_P(poll_return,PSTR("Time entry"),10) == 0) {
		sscanf_P(poll_return+27,PSTR("%u %3s %u %u:%u:%u"),&poll_data.date_y,&poll_data.date_m,&poll_data.date_d,&poll_data.time_h,&poll_data.time_m,&poll_data.time_s);
		poll_data.date_m[3]='\0';
		//printf("%u %s %02u\r\n",poll_data.date_y, poll_data.date_m, poll_data.date_d);
		//printf("%02u:%02u:%02u\r\n",poll_data.time_h,poll_data.time_m,poll_data.time_s);
	}
	else if (strncmp_P(poll_return,PSTR("Watts (-Gen +Con)"),17) == 0) {
		poll_data.watts_total = get_signed_pseudo_float_3(poll_return+27);
	//	printf("Total Watts: %ld.%03ld\r\n",poll_data.watts_total/1000, (poll_data.watts_total <0 ) ? ((poll_data.watts_total % 1000)*-1) : (poll_data.watts_total %1000) );
	}
	
	else if (strncmp_P(poll_return,PSTR("Cumulative Watts (Gen)"),22) == 0) {
		poll_data.watts_gen = get_unsigned_pseudo_float_3(poll_return+27);
	//	printf("Cumulative Watts (gen): %lu.%03lu\r\n",poll_data.watts_gen/1000, poll_data.watts_gen % 1000 );
	}
	else if (strncmp_P(poll_return,PSTR("Cumulative Watts (Con)"),22) == 0) {
		poll_data.watts_con = get_unsigned_pseudo_float_3(poll_return+27);
	//	printf("Cumulative Watts (con): %lu.%03lu\r\n",poll_data.watts_con/1000, poll_data.watts_con % 1000 );
	}
	else if (strncmp_P(poll_return,PSTR("Frequency"),9) == 0){
		poll_data.frequency = get_signed_pseudo_float_3(poll_return+27);
		printf("%d", sizeof(poll_data));
	}
	else if (strncmp_P(poll_return,PSTR("RMS Voltage"),11) == 0){
		poll_data.voltage = get_signed_pseudo_float_3(poll_return+27);
	}
	else if (strncmp_P(poll_return,PSTR("RMS Current"),11) == 0){
		poll_data.current = get_signed_pseudo_float_3(poll_return+27);
	}
	else if (strncmp_P(poll_return,PSTR("Reactive Power (-G/+C)"),22) == 0){
		poll_data.power_total = get_signed_pseudo_float_3(poll_return+27);
	}
	else if (strncmp_P(poll_return,PSTR("Acc Reactive Pwr (Gen)"),22) == 0){
		poll_data.power_gen = get_unsigned_pseudo_float_3(poll_return+27);
	}
	else if (strncmp_P(poll_return,PSTR("Acc Reactive Pwr (Con)"),22) == 0){
		poll_data.power_con = get_unsigned_pseudo_float_3(poll_return+27);
	}
	else if (strncmp_P(poll_return,PSTR("Phase Angle (V/I)"),17) == 0){
		poll_data.phase_angle = get_signed_pseudo_float_3(poll_return+27);
		//printf("Phase: %ld.%03ld\r\n",poll_data.phase_angle/1000, (poll_data.phase_angle <0 ) ? ((poll_data.phase_angle % 1000)*-1) : (poll_data.phase_angle %1000) );
	}
	else if (strncmp_P(poll_return,PSTR("Plogg on time"),13) == 0){
		sscanf_P(poll_return+27,PSTR("%u %*s %u:%u:%u"),&poll_data.plogg_time_d,&poll_data.plogg_time_h,&poll_data.plogg_time_m,&poll_data.plogg_time_s);
		//printf("%u days %02u:%02u:%02u\r\n",poll_data.plogg_time_d,poll_data.plogg_time_h,poll_data.plogg_time_m,poll_data.plogg_time_s);
	}
	else if (strncmp_P(poll_return,PSTR("Equipment on time"),17) == 0){
		sscanf_P(poll_return+27,PSTR("%u %*s %u:%u:%u"),&poll_data.equipment_time_d,&poll_data.equipment_time_h,&poll_data.equipment_time_m,&poll_data.equipment_time_s);
		//printf("%u days %02u:%02u:%02u\r\n",poll_data.equipment_time_d,poll_data.equipment_time_h,poll_data.equipment_time_m,poll_data.equipment_time_s);
	}
	else if (strncmp_P(poll_return,PSTR("Highest RMS voltage"),19) == 0){
		poll_data.voltage_max_value = get_signed_pseudo_float_3(poll_return+24);
		sscanf_P(poll_return+24,PSTR("%*s %*c %*s %u %3s %u %u:%u:%u"),&poll_data.voltage_max_date_y,&poll_data.voltage_max_date_m,&poll_data.voltage_max_date_d,&poll_data.voltage_max_time_h,&poll_data.voltage_max_time_m,&poll_data.voltage_max_time_s);
		poll_data.voltage_max_date_m[3]='\0';
	//	printf("Max Voltage: %ld.%03ld at %u %s %02u %02u:%02u:%02u\r\n",poll_data.voltage_max_value/1000, (poll_data.voltage_max_value <0 ) ? ((poll_data.voltage_max_value % 1000)*-1) : (poll_data.voltage_max_value %1000), poll_data.voltage_max_date_y, poll_data.voltage_max_date_m, poll_data.voltage_max_date_d,poll_data.voltage_max_time_h, poll_data.voltage_max_time_m, poll_data.voltage_max_time_s);

	}
	else if (strncmp_P(poll_return,PSTR("Highest RMS current"),19) == 0){
		poll_data.current_max_value = get_signed_pseudo_float_3(poll_return+24);

		sscanf_P(poll_return+24,PSTR("%*s %*c %*s %u %3s %u %u:%u:%u"),&poll_data.current_max_date_y,&poll_data.current_max_date_m,&poll_data.current_max_date_d,&poll_data.current_max_time_h,&poll_data.current_max_time_m,&poll_data.current_max_time_s);
		poll_data.current_max_date_m[3]='\0';
//		printf("Max Current: %ld.%03ld at %u %s %02u %02u:%02u:%02u\r\n",poll_data.current_max_value/1000, (poll_data.current_max_value <0 ) ? ((poll_data.current_max_value % 1000)*-1) : (poll_data.current_max_value %1000), poll_data.current_max_date_y, poll_data.current_max_date_m, poll_data.current_max_date_d,poll_data.current_max_time_h, poll_data.current_max_time_m, poll_data.current_max_time_s);

	}
	else if (strncmp_P(poll_return,PSTR("Highest wattage"),15) == 0){
		poll_data.watts_max_value = get_signed_pseudo_float_3(poll_return+20);
		sscanf_P(poll_return+20,PSTR("%*s %*c %*s %u %3s %u %u:%u:%u"),&poll_data.watts_max_date_y,&poll_data.watts_max_date_m,&poll_data.watts_max_date_d,&poll_data.watts_max_time_h,&poll_data.watts_max_time_m,&poll_data.watts_max_time_s);
		poll_data.watts_max_date_m[3]='\0';
//		printf("Max Wattage: %ld.%03ld at %u %s %02u %02u:%02u:%02u\r\n",poll_data.watts_max_value/1000, (poll_data.watts_max_value <0 ) ? ((poll_data.watts_max_value % 1000)*-1) : (poll_data.watts_max_value %1000), poll_data.watts_max_date_y, poll_data.watts_max_date_m, poll_data.watts_max_date_d,poll_data.watts_max_time_h, poll_data.watts_max_time_m, poll_data.watts_max_time_s);

	}
	else if (strncmp_P(poll_return,PSTR("Timers are currently enabled"),29) == 0){
		poll_data.activated = true;
	}
	else if (strncmp_P(poll_return,PSTR("Timers are currently disabled"),30) == 0){
		poll_data.activated = false;
	}

	else if (strncmp_P(poll_return,PSTR("Tarrif 0 Cost"),13) == 0){ //Tarrif is not a typo. Plogg returns Tarrif in this case
		sscanf_P(poll_return+16,PSTR("%u"),&poll_data.tariff0_rate);
	}

	else if (strncmp_P(poll_return,PSTR("Tarrif 1 Cost"),13) == 0){//Tarrif is not a typo. Plogg returns Tarrif in this case
		sscanf_P(poll_return+16,PSTR("%u"),&poll_data.tariff1_rate);
	}
	else if (strncmp_P(poll_return,PSTR("Current tarrif zone"),19)==0){
		sscanf_P(poll_return+22,PSTR("%d"),&poll_data.tariff_zone);
	}
	else if (strncmp_P(poll_return,PSTR("Tariff 0 from"),13) ==0){
		sscanf_P(poll_return+22,PSTR("%d%*c%d"),&poll_data.tariff0_start,&poll_data.tariff0_end);
	}
	else if (strncmp_P(poll_return,PSTR("Tariff0 :"),9)==0){
		poll_data.tariff0_consumed = get_unsigned_pseudo_float_3(poll_return+12);
		char* cost = strstr_P(poll_return,PSTR("Cost"));
		poll_data.tariff0_cost = get_unsigned_pseudo_float_3(cost+5);
	//	printf("Tariff 0: %lu.%03lukWh Cost: %lu.%02lu\r\n",poll_data.tariff0_consumed/1000, poll_data.tariff0_consumed % 1000,poll_data.tariff0_cost/1000, (poll_data.tariff0_cost % 1000) / 10 );

	}
	else if (strncmp_P(poll_return,PSTR("Tariff1 :"),9)==0){
		poll_data.tariff1_consumed = get_unsigned_pseudo_float_3(poll_return+12);
		char* cost = strstr_P(poll_return,PSTR("Cost"));
		poll_data.tariff1_cost = get_unsigned_pseudo_float_3(cost+5);
	}

	else if (strncmp_P(poll_return,PSTR("Timer 0"),7)==0){
		sscanf_P(poll_return+16,PSTR("%d%*c%d"),&poll_data.timer0_start,&poll_data.timer0_end);
	}
	else if (strncmp_P(poll_return,PSTR("Timer 1"),7)==0){
		sscanf_P(poll_return+16,PSTR("%d%*c%d"),&poll_data.timer1_start,&poll_data.timer1_end);
	}
	else if (strncmp_P(poll_return,PSTR("Timer 2"),7)==0){
		sscanf_P(poll_return+16,PSTR("%d%*c%d"),&poll_data.timer2_start,&poll_data.timer2_end);
	}
	else if (strncmp_P(poll_return,PSTR("Timer 3"),7)==0){
		sscanf_P(poll_return+16,PSTR("%d%*c%d"),&poll_data.timer3_start,&poll_data.timer3_end);
	}

}




static void ATInterpreterProcessCommand(char* command)
{
	// HACK: dummy commands we need to accept
	if ((strcmp_P(command, PSTR("ATS01=31f4")) == 0) ||
			(strcmp_P(command, PSTR("ATS00=0001")) == 0) ||
			(strcmp_P(command, PSTR("at+dassl")) == 0))
	{
		printf("%s\r\n", AT_RESPONSE_OK);
	}
	// we need to process the join command
	else if (strcmp_P(command, PSTR("at+jn")) == 0)
	{
		printf_P(PSTR("JPAN:11,31F4\r\n"));
		printf("%s\r\n", AT_RESPONSE_OK);
	}
	// check if the command starts with "AT". if not, we return ERROR
	else if (strlen(command) < strlen(AT_COMMAND_PREFIX) ||
			strncmp(command, AT_COMMAND_PREFIX, strlen(AT_COMMAND_PREFIX)) != 0)
	{
		printf("%s\r\n", AT_RESPONSE_ERROR);
	}
	// check if we have an simple "AT" command
	else if (strlen(command) == strlen(AT_COMMAND_PREFIX) &&
			strncmp(command, AT_COMMAND_PREFIX, strlen(AT_COMMAND_PREFIX)) == 0)
	{
		printf("%s\r\n", AT_RESPONSE_OK);
	}
	// check if we have an simple "ATI" command
	else if (strcmp(command, AT_COMMAND_INFO) == 0)
	{
		printf_P(PSTR("I am an AVR Raven Jackdaw pretending to be an ETRX2 Zigbee module.\r\n%s\r\n"), AT_RESPONSE_OK);
	}
	// check if we have an simple "ATZ" command
	else if (strncmp(command, AT_COMMAND_RESET, strlen(command)) == 0)
	{
		printf_P(PSTR("Resetting...\r\n%s\r\n"), AT_RESPONSE_OK);
	}
	// check if we have a unicast command
	else if (strncmp(command, AT_COMMAND_UNICAST, strlen(AT_COMMAND_UNICAST)) == 0)
	{
		// we need to parse the remaining part (payload points  after AT+UCAST:0021ED000004699D,)
		char* address = &command[strlen(AT_COMMAND_UNICAST)];
		char* payload = address;
		while ( *payload != ',' && *payload != '\0' )
		{
			payload++;
		}
		if ( *payload == ',' )
		{
			*payload = '\0';
			payload++;
		}

		// trim CR/LF at the end of the payload
		int len = strlen(payload);
		if (len > 0 && (payload[len-1] == '\r' || payload[len-1] == '\n'))
		{
			payload[len-1] = '\0';
			if (len > 1 && (payload[len-2] == '\r' || payload[len-2] == '\n'))
			{
				payload[len-2] = '\0';
			}
		}

		uint8_t length = strlen(poll_return);
		strcpy(poll_return+length,payload);
		while (strstr(poll_return,"~~") != NULL){
			char * start= strstr(poll_return,"~~");
			start[0]='\0';
			parse_Poll();
			uint8_t rest_length = strlen(start+2);
			memmove(poll_return,start+2,rest_length+1);
		}


	/*	// replace = ~~ with \r\n
		int idx = 0;
		while (payload[idx] != '\0')
		{
			if (payload[idx] == '~')
			{
				if (idx > 0 && payload[idx-1] == '\r')
				{
					payload[idx] = '\n';
				}
				else
				{
					payload[idx] = '\r';
				}
			}
			idx++;
		}
*/
		printf_P(PSTR("+UCAST:00\r\n%s\r\n"), AT_RESPONSE_OK);

//		telnet( payload );
		// HOST is waiting for "ACK:00" or NACK
		printf_P(PSTR("ACK:00\r\n"));
	}
	else
	{
	  // default: we return ERROR
	  printf("%s\r\n", AT_RESPONSE_ERROR);
	}
}


/*---------------------------------------------------------------------------*/
PROCESS_THREAD(plogg_process, ev, data)
{
  static struct etimer etimer;
  int rx;
  unsigned int buf_pos;
  char buf[128];

  PROCESS_BEGIN();

  ringbuf_init(&uart_buf, uart_buf_data, sizeof(uart_buf_data));
  rs232_set_input(RS232_PORT_0, uart_get_char);
  Led1_on(); // red

  etimer_set(&etimer, CLOCK_SECOND * 10);

  while (1) {
    PROCESS_WAIT_EVENT();
    if(ev == PROCESS_EVENT_TIMER) {
      etimer_reset(&etimer);
			if (poll_number == 0){
	      printf_P(PSTR("UCAST:0021ED000004699D=SV\r\n"));
			}
			else if (poll_number == 1){
	      printf_P(PSTR("UCAST:0021ED000004699D=SS\r\n"));
			}
			else if (poll_number == 2){
	      printf_P(PSTR("UCAST:0021ED000004699D=ST\r\n"));
			}
			else if (poll_number == 3){
	      printf_P(PSTR("UCAST:0021ED000004699D=SC\r\n"));
			}
			else if (poll_number == 4){
	      printf_P(PSTR("UCAST:0021ED000004699D=SM\r\n"));
			}
			else if (poll_number == 5){
	      printf_P(PSTR("UCAST:0021ED000004699D=SE\r\n"));
			}
			else if (poll_number == 6){
	      printf_P(PSTR("UCAST:0021ED000004699D=SO\r\n"));
			}
			poll_number = (poll_number+1) % 7;

    } else if (ev == PROCESS_EVENT_MSG) {
      buf_pos = 0;
      while ((rx=ringbuf_get(&uart_buf))!=-1) {
        if (buf_pos<126 && (char)rx=='\r') {
          rx = ringbuf_get(&uart_buf);
          if ((char)rx=='\n') {
            buf[buf_pos] = '\0';
            //printf("%s\r\n", buf);
            ATInterpreterProcessCommand(buf);
            buf_pos = 0;
            continue;
          } else {
            buf[buf_pos++] = '\r';
            buf[buf_pos++] = (char)rx;
          }
        } else {
          buf[buf_pos++] = (char)rx;
        }
        if (buf_pos==127) {
          buf[buf_pos] = 0;
//          telnet("ERROR: RX buffer overflow\r\n");

          buf_pos = 0;
        }
      } // while
    } // events
  }
  PROCESS_END();
}

/*_______________________________COAP________________________________________*/
/*---------------------------------------------------------------------------*/


/****************************** Reset ****************************************/ 
RESOURCE(reset, METHOD_POST, "reset", "Reset");

void
reset_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){
	char temp[50];
	const char* string=NULL;
	bool success=true;


	int len = REST.get_post_variable(request, "section", &string);
	if(len == 0){ 
		success = false;
	}
	else{
		if (strncmp_P(string, PSTR("cost"),MAX(len,4))==0){
			printf_P(PSTR("UCAST:0021ED000004699D=SC 1\r\n"));
		}
		else if(strncmp_P(string, PSTR("max"),MAX(len,3))==0){
			printf_P(PSTR("UCAST:0021ED000004699D=SM 1\r\n"));
		}

		else if(strncmp_P(string, PSTR("acc"),MAX(len,3))==0){
			printf_P(PSTR("UCAST:0021ED000004699D=SR\r\n"));
		}
		else{
			success=false;
		}
	}
  if(!success){
		sprintf_P(temp,PSTR("Payload: section=[acc,cost,max]"));
  	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
  	REST.set_response_payload(response, (uint8_t *) temp , strlen(temp));
 	 	REST.set_response_status(response, REST.status.BAD_REQUEST);
	}
}

/**************************** Max Values *************************************/
RESOURCE(max, METHOD_GET, "max", "Max");

void
max_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[150];
	int index=0;


	index += sprintf_P(temp+index,PSTR("Max Voltage: %ld.%03ldV at %u %s %02u %02u:%02u:%02u\n"),poll_data.voltage_max_value/1000, (poll_data.voltage_max_value <0 ) ? ((poll_data.voltage_max_value % 1000)*-1) : (poll_data.voltage_max_value %1000), poll_data.voltage_max_date_y, poll_data.voltage_max_date_m, poll_data.voltage_max_date_d,poll_data.voltage_max_time_h, poll_data.voltage_max_time_m, poll_data.voltage_max_time_s);

	index += sprintf_P(temp+index,PSTR("Max Current: %ld.%03ldA at %u %s %02u %02u:%02u:%02u\n"),poll_data.current_max_value/1000, (poll_data.current_max_value <0 ) ? ((poll_data.current_max_value % 1000)*-1) : (poll_data.current_max_value %1000), poll_data.current_max_date_y, poll_data.current_max_date_m, poll_data.current_max_date_d,poll_data.current_max_time_h, poll_data.current_max_time_m, poll_data.current_max_time_s);

	index += sprintf_P(temp+index,PSTR("Max Wattage: %ld.%03ldW at %u %s %02u %02u:%02u:%02u\n"),poll_data.watts_max_value/1000, (poll_data.watts_max_value <0 ) ? ((poll_data.watts_max_value % 1000)*-1) : (poll_data.watts_max_value %1000), poll_data.watts_max_date_y, poll_data.watts_max_date_m, poll_data.watts_max_date_d,poll_data.watts_max_time_h, poll_data.watts_max_time_m, poll_data.watts_max_time_s);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(max_voltage, METHOD_GET, "max/voltage", "Max Voltage");

void
max_voltage_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;


	index += sprintf_P(temp+index,PSTR("%ld.%03ldV at %u %s %02u %02u:%02u:%02u\n"),poll_data.voltage_max_value/1000, (poll_data.voltage_max_value <0 ) ? ((poll_data.voltage_max_value % 1000)*-1) : (poll_data.voltage_max_value %1000), poll_data.voltage_max_date_y, poll_data.voltage_max_date_m, poll_data.voltage_max_date_d,poll_data.voltage_max_time_h, poll_data.voltage_max_time_m, poll_data.voltage_max_time_s);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(max_current, METHOD_GET, "max/current", "Max Current");

void
max_current_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ldA at %u %s %02u %02u:%02u:%02u\n"),poll_data.current_max_value/1000, (poll_data.current_max_value <0 ) ? ((poll_data.current_max_value % 1000)*-1) : (poll_data.current_max_value %1000), poll_data.current_max_date_y, poll_data.current_max_date_m, poll_data.current_max_date_d,poll_data.current_max_time_h, poll_data.current_max_time_m, poll_data.current_max_time_s);

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(max_wattage, METHOD_GET, "max/wattage", "Max Wattage");

void
max_wattage_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ldW at %u %s %02u %02u:%02u:%02u\n"),poll_data.watts_max_value/1000, (poll_data.watts_max_value <0 ) ? ((poll_data.watts_max_value % 1000)*-1) : (poll_data.watts_max_value %1000), poll_data.watts_max_date_y, poll_data.watts_max_date_m, poll_data.watts_max_date_d,poll_data.watts_max_time_h, poll_data.watts_max_time_m, poll_data.watts_max_time_s);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}



/************************* Enable/disable Timers ******************************/
RESOURCE(activate, METHOD_GET | METHOD_POST, "activate", "Activate");

void
activate_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){
  
	const char* string=NULL;
  bool success = true;
	char temp[50];

	if (REST.get_method_type(request) == METHOD_GET){
		if (poll_data.activated){
	  	sprintf_P(temp,PSTR("Timers are enabled\n"));
		}
		else{
	  	sprintf_P(temp,PSTR("Timers are disabled\n"));
		}
  	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 		REST.set_response_payload(response, (uint8_t *)temp, strlen(temp));

	}
	else{
		int len=REST.get_post_variable(request, "state", &string);
		if(len == 0){
			success = false;
		}
		else{
			if(strncmp_P(string,PSTR("on"),MAX(len,2))==0){
				printf_P(PSTR("UCAST:0021ED000004699D=SE 1\r\n"));
				poll_data.activated=true;
			}
			else if(strncmp_P(string, PSTR("off"),MAX(len,2))==0){
				printf_P(PSTR("UCAST:0021ED000004699D=SE 0\r\n"));
				poll_data.activated=false;
			}
 			else{
	 	   	success = false;
			}
		}
 	 	if (!success){
			sprintf_P(temp,PSTR("Payload: state=[in,off]"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
  		REST.set_response_payload(response, (uint8_t *) temp , strlen(temp));
 	 	  REST.set_response_status(response, REST.status.BAD_REQUEST);
 	 	}
	}
}

/***************************** Date & Time ***********************************/
RESOURCE(clock, METHOD_GET | METHOD_POST, "clock", "Clock");

void
clock_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[100];
	int index = 0;
	index += sprintf_P(temp+index,PSTR("%02u:%02u:%02u\n"), poll_data.time_h,poll_data.time_m,poll_data.time_s);
	index += sprintf_P(temp+index, PSTR("%02u %s %u\n"),poll_data.date_d,poll_data.date_m,poll_data.date_y);
	
  REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
  REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(time, METHOD_GET | METHOD_POST, "clock/time", "Time");

void
time_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[100];
	bool success = true;
	const char* string=NULL;
	int hour, min,sec;

	if (REST.get_method_type(request) == METHOD_GET){
		sprintf_P(temp,PSTR("%02u:%02u:%02u\n"), poll_data.time_h,poll_data.time_m,poll_data.time_s);
  	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 		REST.set_response_payload(response,(uint8_t *) temp , strlen(temp));
	}
	else{
		int len = REST.get_post_variable(request, "value", &string);
		if (len == 5 || len ==8){
				hour = atoi(&string[0]);
				min = atoi(&string[3]);
			 	sec=(len==5)?0:atoi(&string[6]);

				if (len==8 && ! (isdigit(string[6]) && isdigit(string[7]))){
					success = 0;
				}

				if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]))){
					success = false;
				}
				else if (!( 0<= hour && hour<=23 && 0<=min && min <=59 && 0<=sec && sec<=59)){
					success = false; 
				}
		}
		else{
			success = false;
		}
	 	if (success){
			printf_P(PSTR("UCAST:0021ED000004699D=rtt%02d.%02d.%02d\r\n"),hour,min,sec);
		}
		else{
			sprintf_P(temp, PSTR("Payload: value=hh:mm[:ss]\n"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 			REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
 		 	REST.set_response_status(response, REST.status.BAD_REQUEST);
		}
	}
}


RESOURCE(date, METHOD_GET | METHOD_POST, "clock/date", "Date");

void
date_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){
		
	char temp[100];
	bool success = true;
	const char* string = NULL;
	int month, day, year;

	if (REST.get_method_type(request) == METHOD_GET){
		sprintf_P(temp, PSTR("%02u %s %u\n"),poll_data.date_d,poll_data.date_m,poll_data.date_y);
  	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 		REST.set_response_payload(response,(uint8_t *)temp , strlen(temp));
	}
	else{

		int len = REST.get_post_variable(request,"value",&string);
		if (len==8){
				day = atoi(&string[0]);
				month = atoi(&string[3]);
				year = atoi(&string[6]);
				if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]) && isdigit(string[6]) && isdigit(string[7]))){
					success=false;
				} 
				else if (!(0<=year && year <=99 && 1<=month && month<=12 && 1<=day )){
					success=false;
				}
				else if( (month==4 || month ==6 || month==9 || month==11) && day>30){
					success=false;
				}
				else if( month==2 && !((year%4)==0) && day > 28) {
					success=false;
				}
				else if( month==2 && day>29){
					success=false;
				}
				else if( day > 31){
					success=false;
				}
		}
		else{
			success= false;	
		}
	 	if (success){
			printf_P(PSTR("UCAST:0021ED000004699D=rtd%02i.%02i.%02i\r\n"),year,month,day);
		}
		else{
			sprintf_P(temp, PSTR("Payload: value=dd.mm.yy\n"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 			REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
 		  REST.set_response_status(response, REST.status.BAD_REQUEST);
		}
	}
}
/**************************** Current Values **********************************/

RESOURCE(state_watts, METHOD_GET, "state/watts", "Watts");

void
state_watts_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld W\n"),poll_data.watts_total/1000, (poll_data.watts_total <0 ) ? ((poll_data.watts_total % 1000)*-1) : (poll_data.watts_total %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_watts_gen, METHOD_GET, "state/watts_gen", "Accumulated Watts (Gen)");

void
state_watts_gen_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%lu.%03lu kWh\n"),poll_data.watts_gen/1000, poll_data.watts_gen %1000);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_watts_con, METHOD_GET, "state/watts_con", "Accumulated Watts (Con)");

void
state_watts_con_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%lu.%03lu kWh\n"),poll_data.watts_con/1000, poll_data.watts_con %1000);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_power, METHOD_GET, "state/power", "Total Reactive Power");

void
state_power_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld VAR\n"),poll_data.power_total/1000, (poll_data.power_total <0 ) ? ((poll_data.power_total % 1000)*-1) : (poll_data.power_total %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(state_power_gen, METHOD_GET, "state/power_gen", "Accumulated Reactive Power (Gen)");

void
state_power_gen_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%lu.%03lu kVARh\n"),poll_data.power_gen/1000, poll_data.power_gen %1000);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_power_con, METHOD_GET, "state/power_con", "Accumulated Reactive Power (Con)");

void
state_power_con_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%lu.%03lu kVARh\n"),poll_data.power_con/1000, poll_data.power_con %1000);
  	
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_phase, METHOD_GET, "state/phase", "Phase Angle");

void
state_phase_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld Degrees\n"),poll_data.phase_angle/1000, (poll_data.phase_angle <0 ) ? ((poll_data.phase_angle % 1000)*-1) : (poll_data.phase_angle %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(state_frequency, METHOD_GET, "state/frquency", "Frequency");

void
state_frequency_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld Hz\n"),poll_data.frequency/1000, (poll_data.frequency <0 ) ? ((poll_data.frequency% 1000)*-1) : (poll_data.frequency %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(state_voltage, METHOD_GET, "state/voltage", "Voltage");

void
state_voltage_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld V\n"),poll_data.voltage/1000, (poll_data.voltage <0 ) ? ((poll_data.voltage % 1000)*-1) : (poll_data.voltage %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(state_current, METHOD_GET, "state/current", "Current");

void
state_current_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%ld.%03ld A\n"),poll_data.current/1000, (poll_data.current <0 ) ? ((poll_data.current % 1000)*-1) : (poll_data.current %1000));

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(state_plogg_ontime, METHOD_GET, "state/plogg_ontime", "Plogg on time");

void
state_plogg_ontime_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%u days %u:%02u:%02u \n"),poll_data.plogg_time_d, poll_data.plogg_time_h, poll_data.plogg_time_m, poll_data.plogg_time_s);

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}


RESOURCE(state_equipment_ontime, METHOD_GET, "state/equipment_ontime", "Equipment on time");

void
state_equipment_ontime_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset){

	char temp[50];
	int index=0;

	index += sprintf_P(temp+index,PSTR("%u days %u:%02u:%02u \n"),poll_data.equipment_time_d, poll_data.equipment_time_h, poll_data.equipment_time_m, poll_data.equipment_time_s);

	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

/***************************** Tariff Timer ************************************/

RESOURCE(tariff0_timer,METHOD_GET | METHOD_POST, "tariff0/timer", "Tariff 0 Timer");

void
tariff0_timer_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	char temp[50];
	int index=0;
	const char* string=NULL;
	bool success= true;

	if (REST.get_method_type(request) == METHOD_GET){
		index += sprintf_P(temp+index,PSTR("%02u:%02u-%02u:%02u\n"),poll_data.tariff0_start/100,poll_data.tariff0_start % 100,poll_data.tariff0_end/100,poll_data.tariff0_end % 100);
		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
		REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
	}
	else{
		int len = REST.get_post_variable(request, "start", &string);
		int start_hour=0;
		int start_min=0;
		if (len == 5){
			start_hour = atoi(&string[0]);
			start_min = atoi(&string[3]);
			if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]))){
				success = false;
			}
			else if (!( 0<= start_hour && start_hour<=23 && 0<=start_min && start_min <=59)){
				success = false; 
			}
		}
		else{
			success = false;
		}
		len = REST.get_post_variable(request, "end", &string);
		int end_hour=0;
		int end_min=0;
		if (len == 5){
			end_hour = atoi(&string[0]);
			end_min = atoi(&string[3]);
			if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]))){
				success = false;
			}
			else if (!( 0<= end_hour && end_hour<=23 && 0<=end_min && end_min <=59)){
				success = false; 
			}
		}
		else{
			success = false;
		}
	 	if (success){
			poll_data.tariff0_start=start_hour*100+start_min;
			poll_data.tariff0_end=end_hour*100+end_min;
			printf_P(PSTR("UCAST:0021ED000004699D=ST %04u-%04u\r\n"),poll_data.tariff0_start,poll_data.tariff0_end);
		}
		else{
			sprintf_P(temp, PSTR("Payload: start=hh:mm&end=hh:mm\n"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 			REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
 			REST.set_response_status(response, REST.status.BAD_REQUEST);
		}
		
	}

}



/***************************** Tariff Rate ************************************/
void
handleTariff_rate(uint8_t tariff,void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	char temp[50];
	int index=0;
	const char* string=NULL;
	bool success= true;
	uint16_t rate=0;

	if (REST.get_method_type(request) == METHOD_GET){
		switch (tariff){
			case 0: rate=poll_data.tariff0_rate; break;
			case 1: rate=poll_data.tariff1_rate; break;
		}
		index += sprintf_P(temp+index,PSTR("%u pence/kWh\n"),rate);
		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
		REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
	}
	else{
		int len = REST.get_post_variable(request, "value", &string);
		if (len==0){
			success=false;
		}
		else{
			rate = atoi(&string[0]);
			if (!(isdigit(string[0]))){
				success = false;
			}
			else if (!( 0<= rate && rate < 1000)){
				success = false; 
			}
		}
	 	if (success){
			switch (tariff){
				case 0: poll_data.tariff0_rate=rate; break;
				case 1: poll_data.tariff1_rate=rate; break;
			}
			printf_P(PSTR("UCAST:0021ED000004699D=SS %u %u\r\n"),tariff,rate);
		}
		else{
			sprintf_P(temp, PSTR("Payload: value=ppp\n"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 			REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
 			REST.set_response_status(response, REST.status.BAD_REQUEST);
		}
	}
}

RESOURCE(tariff0_rate,METHOD_GET | METHOD_POST, "tariff0/rate", "Tariff 0 Rate");
RESOURCE(tariff1_rate,METHOD_GET | METHOD_POST, "tariff1/rate", "Tariff 1 Rate");
void
tariff0_rate_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTariff_rate(0,request,response,buffer,preffered_size,offset);
}
void
tariff1_rate_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTariff_rate(1,request,response,buffer,preffered_size,offset);
}


/****************************** Costs ************************************/

void
handleCost(uint8_t tariff,void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	char temp[50];
	int index=0;
	unsigned long cost=0;

	switch (tariff){
		case 0: cost=poll_data.tariff0_cost; break;
		case 1: cost=poll_data.tariff1_cost; break;
	}
	index += sprintf_P(temp+index,PSTR("%lu.%02lu\n"), cost / 1000, (cost %1000)/10);
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(tariff0_cost,METHOD_GET, "cost/tariff0", "Cost Tarrif 0 ");
RESOURCE(tariff1_cost,METHOD_GET, "cost/tariff1", "Cost Tariff 1");
void
tariff0_cost_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleCost(0,request,response,buffer,preffered_size,offset);
}
void
tariff1_cost_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleCost(1,request,response,buffer,preffered_size,offset);
}


/****************************** Tariff Consumed ************************************/

void
handleConsumed(uint8_t tariff,void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	char temp[50];
	int index=0;
	unsigned long consumed=0;

	switch (tariff){
		case 0: consumed=poll_data.tariff0_consumed; break;
		case 1: consumed=poll_data.tariff1_consumed; break;
	}
	index += sprintf_P(temp+index,PSTR("%lu.%03lu kWh\n"), consumed / 1000, (consumed %1000));
	REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
	REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
}

RESOURCE(tariff0_consumed,METHOD_GET, "tariff0/consumed", "Tariff 0 Consumed");
RESOURCE(tariff1_consumed,METHOD_GET, "tariff1/consumed", "Tariff 1 Consumed");
void
tariff0_consumed_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleConsumed(0,request,response,buffer,preffered_size,offset);
}
void
tariff1_consumed_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleConsumed(1,request,response,buffer,preffered_size,offset);
}

/********************************** Timers *************************************/


void
handleTimer(uint8_t timer, void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	char temp[50];
	int index=0;
	const char* string=NULL;
	bool success= true;
	uint16_t start_time=0;
	uint16_t end_time=0;

	if (REST.get_method_type(request) == METHOD_GET){
		switch (timer){
			case 0:
				start_time = poll_data.timer0_start;
				end_time = poll_data.timer0_end;
				break;
			case 1:
				start_time = poll_data.timer1_start;
				end_time = poll_data.timer1_end;
				break;	
			case 2:
				start_time = poll_data.timer2_start;
				end_time = poll_data.timer2_end;
				break;	
			case 3:
				start_time = poll_data.timer3_start;
				end_time = poll_data.timer3_end;
				break;	
		}

		index += sprintf_P(temp+index,PSTR("%02u:%02u-%02u:%02u\n"),start_time/100,start_time % 100,end_time/100,end_time % 100);
		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
		REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
	}
	else{
		int len = REST.get_post_variable(request, "start", &string);
		int start_hour=0;
		int start_min=0;
		if (len == 5){
			start_hour = atoi(&string[0]);
			start_min = atoi(&string[3]);
			if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]))){
				success = false;
			}
			else if (!( 0<= start_hour && start_hour<=23 && 0<=start_min && start_min <=59)){
				success = false; 
			}
		}
		else{
			success = false;
		}
		len = REST.get_post_variable(request, "end", &string);
		int end_hour=0;
		int end_min=0;
		if (len == 5){
			end_hour = atoi(&string[0]);
			end_min = atoi(&string[3]);
			if (!(isdigit(string[0]) &&  isdigit(string[1]) && isdigit(string[3]) && isdigit(string[4]))){
				success = false;
			}
			else if (!( 0<= end_hour && end_hour<=23 && 0<=end_min && end_min <=59)){
				success = false; 
			}
		}
		else{
			success = false;
		}
	 	if (success){
			start_time=start_hour*100+start_min;
			end_time=end_hour*100+end_min;
			switch (timer){
				case 0:
					poll_data.timer0_start = start_time;
					poll_data.timer0_end = end_time;
					break;
				case 1:
					poll_data.timer1_start = start_time;
					poll_data.timer1_end = end_time;
					break;	
				case 2:
					poll_data.timer2_start = start_time;
					poll_data.timer2_end = end_time;
					break;	
				case 3:
					poll_data.timer3_start = start_time;
					poll_data.timer3_end = start_time;
					break;	
			}
			printf_P(PSTR("UCAST:0021ED000004699D=SO %u %04u-%04u\r\n"),timer,start_time,end_time);
		}
		else{
			sprintf_P(temp, PSTR("Payload: start=hh:mm&end=hh:mm\n"));
  		REST.set_header_content_type(response, REST.type.TEXT_PLAIN);
 			REST.set_response_payload(response, (uint8_t *)temp , strlen(temp));
 			REST.set_response_status(response, REST.status.BAD_REQUEST);
		}
		
	}

}


RESOURCE(timer0,METHOD_GET|METHOD_POST, "timer0", "Timer 0");
RESOURCE(timer1,METHOD_GET|METHOD_POST, "timer1", "Timer 1");
RESOURCE(timer2,METHOD_GET|METHOD_POST, "timer2", "Timer 2");
RESOURCE(timer3,METHOD_GET|METHOD_POST, "timer3", "Timer 3");

void
timer0_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTimer(0,request,response,buffer,preffered_size,offset);
}
void
timer1_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTimer(1,request,response,buffer,preffered_size,offset);
}
void
timer2_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTimer(2,request,response,buffer,preffered_size,offset);
}
void
timer3_handler(void* request, void* response, uint8_t *buffer, uint16_t preffered_size, int32_t *offset){
	handleTimer(3,request,response,buffer,preffered_size,offset);
}


/****************************** Coap Process ***********************************/

PROCESS_THREAD(coap_process, ev, data)
{
  PROCESS_BEGIN();

  rest_init_framework();
  
  rest_activate_resource(&resource_clock);
  rest_activate_resource(&resource_time);
  rest_activate_resource(&resource_date);
	
	rest_activate_resource(&resource_activate);
  
	rest_activate_resource(&resource_max);
  rest_activate_resource(&resource_max_voltage);
  rest_activate_resource(&resource_max_current);
  rest_activate_resource(&resource_max_wattage);
  
	rest_activate_resource(&resource_reset);
  
	rest_activate_resource(&resource_state_watts);
  rest_activate_resource(&resource_state_watts_gen);
  rest_activate_resource(&resource_state_watts_con);
  rest_activate_resource(&resource_state_power);
  rest_activate_resource(&resource_state_power_gen);
  rest_activate_resource(&resource_state_power_con);
  rest_activate_resource(&resource_state_phase);
  rest_activate_resource(&resource_state_frequency);
  rest_activate_resource(&resource_state_voltage);
  rest_activate_resource(&resource_state_current);
	rest_activate_resource(&resource_state_plogg_ontime);
	rest_activate_resource(&resource_state_equipment_ontime);
	
	rest_activate_resource(&resource_tariff0_timer);
	rest_activate_resource(&resource_tariff0_rate);
	rest_activate_resource(&resource_tariff0_cost);
	rest_activate_resource(&resource_tariff0_consumed);
	rest_activate_resource(&resource_tariff1_rate);
	rest_activate_resource(&resource_tariff1_cost);
	rest_activate_resource(&resource_tariff1_consumed);

	rest_activate_resource(&resource_timer0);
	rest_activate_resource(&resource_timer1);
	rest_activate_resource(&resource_timer2);
	rest_activate_resource(&resource_timer3);

	strcpy_P(poll_return,PSTR("\0"));
	poll_data.activated=false;

 	poll_data.date_y=0;
	strcpy_P(poll_data.date_m,PSTR("\0"));
 	poll_data.date_d=0;
	poll_data.time_h=0;
	poll_data.time_m=0;
  poll_data.time_s=0;

	poll_data.plogg_time_d=0;
	poll_data.plogg_time_h=0;
	poll_data.plogg_time_m=0;
	poll_data.plogg_time_s=0;
	poll_data.equipment_time_d=0;
	poll_data.equipment_time_h=0;
	poll_data.equipment_time_m=0;
	poll_data.equipment_time_s=0;

	poll_data.watts_total=0;
	poll_data.watts_con=0;
	poll_data.watts_gen=0;
	poll_data.phase_angle=0;

	poll_data.frequency=0;
	poll_data.current=0;
	poll_data.voltage=0;

	poll_data.power_total=0;
	poll_data.power_gen=0;
	poll_data.power_con=0;

	poll_data.current_max_value=0;
	poll_data.current_max_date_y=0;
	strcpy_P(poll_data.current_max_date_m,PSTR("\0"));
 	poll_data.current_max_date_d=0;
	poll_data.current_max_time_h=0;
	poll_data.current_max_time_m=0;
	poll_data.current_max_time_s=0;

	poll_data.voltage_max_value=0;
 	poll_data.voltage_max_date_y=0;
	strcpy_P(poll_data.voltage_max_date_m,PSTR("\0"));
 	poll_data.voltage_max_date_d=0;
	poll_data.voltage_max_time_h=0;
	poll_data.voltage_max_time_m=0;
  poll_data.voltage_max_time_s=0;
	
	poll_data.watts_max_value=0;
 	poll_data.watts_max_date_y=0;
	strcpy_P(poll_data.watts_max_date_m,PSTR("\0"));
 	poll_data.watts_max_date_d=0;
	poll_data.watts_max_time_h=0;
	poll_data.watts_max_time_m=0;
  poll_data.watts_max_time_s=0;

	poll_data.tariff_zone=0;
	poll_data.tariff0_start=0;
	poll_data.tariff0_end=0;
	
	poll_data.tariff0_rate=0;
	poll_data.tariff1_rate=0;
	poll_data.tariff0_consumed=0;
	poll_data.tariff0_cost=0;
	poll_data.tariff1_consumed=0;
	poll_data.tariff1_cost=0;


	poll_data.timer0_start=0;
	poll_data.timer0_end=0;
	poll_data.timer1_start=0;
	poll_data.timer1_end=0;
	poll_data.timer2_start=0;
	poll_data.timer2_end=0;
	poll_data.timer3_start=0;
	poll_data.timer3_end=0;


  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
AUTOSTART_PROCESSES(&coap_process, &plogg_process);
/*---------------------------------------------------------------------------*/