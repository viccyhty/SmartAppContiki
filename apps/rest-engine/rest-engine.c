#include "contiki.h"
#include <string.h> /*for string operations in match_addresses*/
#include <stdio.h> /*for sprintf in rest_set_header_**/

#include "rest-engine.h"

#define DEBUG 0
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINT6ADDR(addr) PRINTF(" %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x ", ((u8_t *)addr)[0], ((u8_t *)addr)[1], ((u8_t *)addr)[2], ((u8_t *)addr)[3], ((u8_t *)addr)[4], ((u8_t *)addr)[5], ((u8_t *)addr)[6], ((u8_t *)addr)[7], ((u8_t *)addr)[8], ((u8_t *)addr)[9], ((u8_t *)addr)[10], ((u8_t *)addr)[11], ((u8_t *)addr)[12], ((u8_t *)addr)[13], ((u8_t *)addr)[14], ((u8_t *)addr)[15])
#define PRINTLLADDR(lladdr) PRINTF(" %02x:%02x:%02x:%02x:%02x:%02x ",(lladdr)->addr[0], (lladdr)->addr[1], (lladdr)->addr[2], (lladdr)->addr[3],(lladdr)->addr[4], (lladdr)->addr[5])
#else
#define PRINTF(...)
#define PRINT6ADDR(addr)
#define PRINTLLADDR(addr)
#endif

PROCESS_NAME(rest_manager_process);

LIST(restful_services);
LIST(restful_periodic_services);


#ifdef WITH_HTTP

char *
rest_to_http_max_age(uint32_t age)
{
  /* Cache-Control: max-age=age for HTTP */
  static char temp_age[19];
  snprintf(temp_age, sizeof(temp_age), "max-age=%lu", age);
  return temp_age;
}

char *
rest_to_http_etag(uint8_t *etag, uint8_t etag_len)
{
  static char temp_etag[17];
  int index = 0;

  for (index = 0; index<sizeof(temp_etag) && index<etag_len; ++index) {
    snprintf(temp_etag+2*index, sizeof(temp_etag), "%02x", etag[index]);
  }
  temp_etag[2*index] = '\0';

  return temp_etag;
}
#endif /*WITH_COAP*/


void
rest_init_framework(void)
{
  list_init(restful_services);

  REST.set_service_callback(rest_invoke_restful_service);

  /* Start the RESTful server implementation. */
  REST.init();

  /*Start rest manager process*/
  process_start(&rest_manager_process, NULL);
}

void
rest_activate_resource(resource_t* resource)
{
  /*add it to the restful web service link list*/
  PRINTF("Activating: %s", resource->url);
  list_add(restful_services, resource);
}

void
rest_activate_periodic_resource(periodic_resource_t* periodic_resource)
{
  list_add(restful_periodic_services, periodic_resource);
  rest_activate_resource(periodic_resource->resource);

  rest_set_post_handler(periodic_resource->resource, REST.subscription_handler);
}

list_t
rest_get_resources(void)
{
  return restful_services;
}


void*
rest_get_user_data(resource_t* resource)
{
  return resource->user_data;
}

void
rest_set_user_data(resource_t* resource, void* user_data)
{
  resource->user_data = user_data;
}

void
rest_set_pre_handler(resource_t* resource, restful_pre_handler pre_handler)
{
  resource->pre_handler = pre_handler;
}

void
rest_set_post_handler(resource_t* resource, restful_post_handler post_handler)
{
  resource->post_handler = post_handler;
}

int
rest_invoke_restful_service(void* request, void* response, uint8_t *buffer, uint16_t buffer_size, int32_t *offset)
{
  uint8_t found = 0;
  uint8_t allowed = 0;

  PRINTF("rest_invoke_restful_service url /%.*s -->\n", url_len, url);

  resource_t* resource = NULL;
  const char *url = NULL;

  for (resource = (resource_t*)list_head(restful_services); resource; resource = resource->next)
  {
    /*if the web service handles that kind of requests and urls matches*/
    if (REST.get_url(request, &url)==strlen(resource->url) && strncmp(resource->url, url, strlen(resource->url)) == 0)
    {
      /* The resource URL is '\0'-terminated: a much better handle, e.g., for observing */
      REST.set_url(request, (char *) resource->url);

      found = 1;
      rest_method_t method = REST.get_method_type(request);

      PRINTF("method %u, resource->methods_to_handle %u\n", (uint16_t)method, resource->methods_to_handle);

      if (resource->methods_to_handle & method)
      {
        allowed = 1;

        /*call pre handler if it exists*/
        if (!resource->pre_handler || resource->pre_handler(request, response))
        {
          /* call handler function*/
          resource->handler(request, response, buffer, buffer_size, offset);

          /*call post handler if it exists*/
          if (resource->post_handler)
          {
            resource->post_handler(request, response);
          }
        }
      } else {
        REST.set_response_status(response, REST.status.METHOD_NOT_ALLOWED);
      }
      break;
    }
  }

  if (!found) {
    REST.set_response_status(response, REST.status.NOT_FOUND);
  }

  return found & allowed;
}
/*-----------------------------------------------------------------------------------*/

PROCESS(rest_manager_process, "Rest Process");

PROCESS_THREAD(rest_manager_process, ev, data)
{
  PROCESS_BEGIN();

  PROCESS_PAUSE();

  /* Initialize the PERIODIC_RESOURCE timers, which will be handled by this process. */
  periodic_resource_t* periodic_resource = NULL;
  for (periodic_resource = (periodic_resource_t*) list_head(restful_periodic_services); periodic_resource; periodic_resource = periodic_resource->next) {
    if (periodic_resource->period) {
      PRINTF("Periodic: Set timer for %s to %lu\n", periodic_resource->resource->url, periodic_resource->period);
      etimer_set(&periodic_resource->periodic_timer, periodic_resource->period);
    }
  }

  while (1) {
    PROCESS_WAIT_EVENT();
    if (ev == PROCESS_EVENT_TIMER) {
      for (periodic_resource = (periodic_resource_t*)list_head(restful_periodic_services);periodic_resource;periodic_resource = periodic_resource->next) {
        if (periodic_resource->period && etimer_expired(&periodic_resource->periodic_timer)) {

          PRINTF("Periodic: etimer expired for /%s (period: %lu)\n", periodic_resource->resource->url, periodic_resource->period);

          /* Call the periodic_handler function if it exists. */
          if (periodic_resource->periodic_handler) {
            (periodic_resource->periodic_handler)(periodic_resource->resource);
          }
          etimer_reset(&periodic_resource->periodic_timer);
        }
      }
    }
  }

  PROCESS_END();
}
