/**
 * collectd - src/pinba.c
 * Copyright (C) 2010       Phoenix Kayo
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Phoenix Kayo <kayo.k11.4 at gmail.com>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

/*
 *  Service declaration section
 */

#ifndef PINBA_UDP_BUFFER_SIZE
#define PINBA_UDP_BUFFER_SIZE 65536
#endif

typedef struct _pinba_statres_ pinba_statres;
struct _pinba_statres_ {
  const char *name;
  float req_per_sec;
  float req_time;
  float ru_utime;
  float ru_stime;
  float doc_size;
  float mem_peak;
};

/*
 *  Fetch collection datas
 *
 *  Return value: id of next data, zero if no data.
 */
static unsigned int
service_statnode_collect(pinba_statres *res,
			 unsigned int id);
static void
service_statnode_begin();
static void
service_statnode_end();
static void
service_statnode_free();
static void
service_statnode_init();
static void
service_statnode_add(const char *name,
		     const char *host,
		     const char *server,
		     const char *script);

static void
service_config(const char* address,
	       unsigned int port);
static int
service_start();
static int
service_stop();

/*
 *  Service implementation section
 */

#include"pinba.pb-c.h"

#include<stdarg.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>

#include<sys/types.h>
#include<math.h>
#include<pthread.h>

#include<sys/time.h>
#include<event.h>

#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/socket.h>

typedef struct _pinba_socket_ pinba_socket;
struct _pinba_socket_ {
  int listen_sock;
  struct event *accept_event;
};

typedef double pinba_time;
typedef uint32_t pinba_size;

static pinba_time
now(){
  static struct timeval tv;
  static struct timezone tz;
  
  gettimeofday(&tv, &tz);
  
  return (double)tv.tv_sec+((double)tv.tv_usec/(double)1000000);
}

static pthread_rwlock_t
temp_lock;

static struct event_base *
temp_base=NULL;

static pinba_socket *
temp_sock=NULL;

static pthread_t
temp_thrd;

typedef struct _pinba_statnode_ pinba_statnode;
struct _pinba_statnode_{
  /* collector name */
  char* name;
  /* query data */
  char *host;
  char *server;
  char *script;
  /* collected data */
  pinba_time last_coll;
  pinba_size req_count;
  pinba_time req_time;
  pinba_time ru_utime;
  pinba_time ru_stime;
  pinba_size doc_size;
  pinba_size mem_peak;
};

static unsigned int
stat_nodes_count=0;

static pinba_statnode *
stat_nodes=NULL;

static void
service_statnode_reset(pinba_statnode *node){
  node->last_coll=now();
  node->req_count=0;
  node->req_time=0.0;
  node->ru_utime=0.0;
  node->ru_stime=0.0;
  node->doc_size=0;
  node->mem_peak=0;
}

static inline void
strdel(char **str){
  if(*str)free(*str);
  *str=NULL;
}

static inline void
strset(char **str, const char *new){
  strdel(str);
  new && (*str=strdup(new));
}

static void
service_statnode_add(const char *name,
		     const char *host,
		     const char *server,
		     const char *script){
  pinba_statnode *node;
  DEBUG("adding node `%s' to collector { %s, %s, %s }", name, host?host:"", server?server:"", script?script:"");
  
  stat_nodes=realloc(stat_nodes, sizeof(pinba_statnode)*(stat_nodes_count+1));
  if(!stat_nodes){
    ERROR("Realloc failed!");
    exit(-1);
  }
  
  node=&stat_nodes[stat_nodes_count];
  
  /* reset stat data */
  service_statnode_reset(node);
  
  /* reset strings */
  node->name=NULL;
  node->host=NULL;
  node->server=NULL;
  node->script=NULL;
  
  /* fill query data */
  strset(&node->name, name);
  strset(&node->host, host);
  strset(&node->server, server);
  strset(&node->script, script);
  
  /* increment counter */
  stat_nodes_count++;
}

static void
service_statnode_free(){
  if(stat_nodes_count){
    DEBUG("shutdowning collector..");
    unsigned int i;
    
    for(i=0;i<stat_nodes_count;i++){
      /* free strings */
      strdel(&stat_nodes[i].name);
      strdel(&stat_nodes[i].host);
      strdel(&stat_nodes[i].server);
      strdel(&stat_nodes[i].script);
    }
    
    free(stat_nodes);
    stat_nodes_count=0;
    
    pthread_rwlock_destroy(&temp_lock);
  }
}

static void
service_statnode_init(){
  /* only total info collect by default */
  service_statnode_free();
  
  DEBUG("initializing collector..");
  pthread_rwlock_init(&temp_lock, 0);
}

static void
service_statnode_begin(){
  service_statnode_init();
  pthread_rwlock_wrlock(&temp_lock);
  
  service_statnode_add("total", NULL, NULL, NULL);
}

static void
service_statnode_end(){
  pthread_rwlock_unlock(&temp_lock);
}

static unsigned int
service_statnode_collect(pinba_statres *res, unsigned int i){
  pinba_statnode* node;
  
  if(stat_nodes_count==0) return 0;
  
  /* begin collecting */
  if(i==0){
    pthread_rwlock_wrlock(&temp_lock);
  }
  
  /* find non-empty node */
  //for(node=stat_nodes+i; node->req_count==0 && ++i<stat_nodes_count; node=stat_nodes+i);
  
  /* end collecting */
  if(i>=stat_nodes_count){
    pthread_rwlock_unlock(&temp_lock);
    return 0;
  }
  
  node=stat_nodes+i;
  
  pinba_time delta=now()-node->last_coll;
  
  res->name=node->name;
  
  res->req_per_sec=node->req_count/delta;
  
  if(node->req_count==0)node->req_count=1;
  res->req_time=node->req_time/node->req_count;
  res->ru_utime=node->ru_utime/node->req_count;
  res->ru_stime=node->ru_stime/node->req_count;
  res->ru_stime=node->ru_stime/node->req_count;
  res->doc_size=node->doc_size/node->req_count;
  res->mem_peak=node->mem_peak/node->req_count;
  
  service_statnode_reset(node);
  return ++i;
}

static void
service_statnode_process(pinba_statnode *node,
			 Pinba__Request* request){
  node->req_count++;
  node->req_time+=request->request_time;
  node->ru_utime+=request->ru_utime;
  node->ru_stime+=request->ru_stime;
  node->doc_size+=request->document_size;
  node->mem_peak+=request->memory_peak;
}

static void
service_process_request(Pinba__Request* request){
  unsigned int i;
  pthread_rwlock_wrlock(&temp_lock);
  
  DEBUG("Process request..");
  for(i=0;i<stat_nodes_count;i++){
    if(stat_nodes[i].host && strcmp(request->hostname, stat_nodes[i].host))continue;
    if(stat_nodes[i].server && strcmp(request->server_name, stat_nodes[i].server))continue;
    if(stat_nodes[i].script && strcmp(request->script_name, stat_nodes[i].script))continue;
    service_statnode_process(&stat_nodes[i], request);
  }
  
  pthread_rwlock_unlock(&temp_lock);
}

char
service_status=0;

char *
service_address=PINBA_DEFAULT_ADDRESS;

unsigned int
service_port=PINBA_DEFAULT_PORT;

static void
service_config(const char* address,
	       unsigned int port){
  char need_restart=0;
  
  if(address && (service_address && strcmp(service_address, address))){
    strset(&service_address, address);
    need_restart++;
  }
  
  if(port>0 && port<65536 && service_port!=port){
    service_port=port;
    need_restart++;
  }
  
  if(service_status && need_restart){
    service_stop();
    service_start();
  }
}

static void
pinba_socket_free (pinba_socket *socket);

static pinba_socket *
pinba_socket_open (const char *ip,
		   int listen_port);

static void *
pinba_main(void *arg){
  DEBUG("entering listen-loop..");
  
  service_status=1;
  event_base_dispatch(temp_base);
  
  /* unreachable */
  return NULL;
}

static int
service_cleanup(){
  DEBUG("closing socket..");
  if(temp_sock){
    pthread_rwlock_wrlock(&temp_lock);
    pinba_socket_free(temp_sock);
    pthread_rwlock_unlock(&temp_lock);
  }
  
  DEBUG("shutdowning event..");
  event_base_free(temp_base);
  
  DEBUG("shutting down..");
}

static int
service_start(){
  DEBUG("starting up..");
  
  DEBUG("initializing event..");
  temp_base = event_base_new();
  
  DEBUG("opening socket..");
  
  temp_sock = pinba_socket_open(service_address, service_port);
  
  if (!temp_sock) {
    service_cleanup();
    return 1;
  }
  
  if (pthread_create(&temp_thrd, NULL, pinba_main, NULL)) {
    service_cleanup();
    return 1;
  }
  
  return 0;
}

static int
service_stop(){
  pthread_cancel(temp_thrd);
  pthread_join(temp_thrd, NULL);
  service_status=0;
  DEBUG("terminating listen-loop..");
  
  service_cleanup();
  
  return 0;
}

static int
pinba_process_stats_packet (const unsigned char *buf,
			    int buf_len) {
  Pinba__Request *request;  
  
  request = pinba__request__unpack(NULL, buf_len, buf);
  
  if (!request) {
    return P_FAILURE;
  } else {
    service_process_request(request);
    
    pinba__request__free_unpacked(request, NULL);
    
    return P_SUCCESS;
  }
}

static void
pinba_udp_read_callback_fn (int sock,
			    short event,
			    void *arg) {
  if (event & EV_READ) {
    int ret;
    unsigned char buf[PINBA_UDP_BUFFER_SIZE];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(struct sockaddr_in);
    
    ret = recvfrom(sock, buf, PINBA_UDP_BUFFER_SIZE-1, MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
    if (ret > 0) {
      if (pinba_process_stats_packet(buf, ret) != P_SUCCESS) {
	DEBUG("failed to parse data received from %s", inet_ntoa(from.sin_addr));
      }
    } else if (ret < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
	return;
      }
      WARNING("recv() failed: %s (%d)", strerror(errno), errno);
    } else {
      WARNING("recv() returned 0");
    }
  }
}

static void
pinba_socket_free (pinba_socket *socket) {
  if (!socket) return;
  
  if (socket->listen_sock >= 0) {
    close(socket->listen_sock);
    socket->listen_sock = -1;
  }
  
  if (socket->accept_event) {
    event_del(socket->accept_event);
    free(socket->accept_event);
    socket->accept_event = NULL;
  }
  
  free(socket);
}

static pinba_socket *
pinba_socket_open (const char *ip,
		   int listen_port) {
  struct sockaddr_in addr;
  pinba_socket *s;
  int sfd, flags, yes = 1;
  
  if ((sfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
    ERROR("socket() failed: %s (%d)", strerror(errno), errno);
    return NULL;
  }
  
  if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 || fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(sfd);
    return NULL;
  }
  
  if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
    close(sfd);
    return NULL;
  }
  
  s = (pinba_socket *)calloc(1, sizeof(pinba_socket));
  if (!s) {
    return NULL;
  }
  s->listen_sock = sfd;
  
  memset(&addr, 0, sizeof(addr));
  
  addr.sin_family = AF_INET;
  addr.sin_port = htons(listen_port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  if (ip && *ip) {
    struct in_addr tmp;
    
    if (inet_aton(ip, &tmp)) {
      addr.sin_addr.s_addr = tmp.s_addr;
    } else {
      WARNING("inet_aton(%s) failed, listening on ANY IP-address", ip);
    }
  }
  
  if (bind(s->listen_sock, (struct sockaddr *)&addr, sizeof(addr))) {
    pinba_socket_free(s);
    ERROR("bind() failed: %s (%d)", strerror(errno), errno);
    return NULL;
  }
  
  s->accept_event = (struct event *)calloc(1, sizeof(struct event));
  if (!s->accept_event) {
    ERROR("calloc() failed: %s (%d)", strerror(errno), errno);
    pinba_socket_free(s);
    return NULL;
  }
  
  event_set(s->accept_event, s->listen_sock, EV_READ | EV_PERSIST, pinba_udp_read_callback_fn, s);
  event_base_set(temp_base, s->accept_event);
  event_add(s->accept_event, NULL);
  
  return s;
}

/*
 * Plugin declaration section
 */

#include<stdint.h>

static int
config_set (char **var, const char *value) {
  /* code from nginx plugin for collectd */
  if (*var != NULL) {
    free (*var);
    *var = NULL;
  }
  
  if ((*var = strdup (value)) == NULL) return (1);
  else return (0);
}

static int
plugin_config (oconfig_item_t *ci) {
  unsigned int i, o;
  int pinba_port = 0;
  char *pinba_address = NULL;
  
  INFO("Pinba Configure..");
  
  service_statnode_begin();
  
  /* Set default values */
  config_set(&pinba_address, PINBA_DEFAULT_ADDRESS);
  pinba_port = PINBA_DEFAULT_PORT;
  
  for (i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp ("Address", child->key) == 0) {
      if ((child->values_num != 1) || (child->values[0].type != OCONFIG_TYPE_STRING)){
	WARNING ("pinba plugin: `Address' needs exactly one string argument.");
	return (-1);
      }
      config_set(&pinba_address, child->values[0].value.string);
    } else if (strcasecmp ("Port", child->key) == 0) {
      if ((child->values_num != 1) || (child->values[0].type != OCONFIG_TYPE_NUMBER)){
	WARNING ("pinba plugin: `Port' needs exactly one number argument.");
	return (-1);
      }
      pinba_port=child->values[0].value.number;
    } else if (strcasecmp ("View", child->key) == 0) {
      const char *name=NULL, *host=NULL, *server=NULL, *script=NULL;
      if ((child->values_num != 1) || (child->values[0].type != OCONFIG_TYPE_STRING) || strlen(child->values[0].value.string)==0){
	WARNING ("pinba plugin: `View' needs exactly one non-empty string argument.");
	return (-1);
      }
      name = child->values[0].value.string;
      for(o=0; o<child->children_num; o++){
	oconfig_item_t *node = child->children + o;
	if (strcasecmp ("Host", node->key) == 0) {
	  if ((node->values_num != 1) || (node->values[0].type != OCONFIG_TYPE_STRING) || strlen(node->values[0].value.string)==0){
	    WARNING ("pinba plugin: `View->Host' needs exactly one non-empty string argument.");
	    return (-1);
	  }
	  host = node->values[0].value.string;
	} else if (strcasecmp ("Server", node->key) == 0) {
	  if ((node->values_num != 1) || (node->values[0].type != OCONFIG_TYPE_STRING) || strlen(node->values[0].value.string)==0){
	    WARNING ("pinba plugin: `View->Server' needs exactly one non-empty string argument.");
	    return (-1);
	  }
	  server = node->values[0].value.string;
	} else if (strcasecmp ("Script", node->key) == 0) {
	  if ((node->values_num != 1) || (node->values[0].type != OCONFIG_TYPE_STRING) || strlen(node->values[0].value.string)==0){
	    WARNING ("pinba plugin: `View->Script' needs exactly one non-empty string argument.");
	    return (-1);
	  }
	  script = node->values[0].value.string;
	} else {
	  WARNING ("pinba plugin: In `<View>' context allowed only `Host', `Server' and `Script' options but not the `%s'.", node->key);
	  return (-1);
	}
      }
      /* add new statnode */
      service_statnode_add(name, host, server, script);
    } else {
      WARNING ("pinba plugin: In `<Plugin pinba>' context allowed only `Address', `Port' and `Observe' options but not the `%s'.", child->key);
      return (-1);
    }
  }
  
  service_statnode_end();
  
  service_config(pinba_address, pinba_port);
} /* int pinba_config */

static int
plugin_init (void) {
  INFO("Pinba Starting..");
  service_start();
  return 0;
}

static int
plugin_shutdown (void) {
  INFO("Pinba Stopping..");
  service_stop();
  service_statnode_free();
  return 0;
}

static int
plugin_submit (const char *plugin_instance,
	       const char *type,
	       const pinba_statres *res) {
  value_t values[6];
  value_list_t vl = VALUE_LIST_INIT;
  
  values[0].gauge = res->req_per_sec;
  values[1].gauge = res->req_time;
  values[2].gauge = res->ru_utime;
  values[3].gauge = res->ru_stime;
  values[4].gauge = res->doc_size;
  values[5].gauge = res->mem_peak;
  
  vl.values = values;
  vl.values_len = 6;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "pinba", sizeof (vl.plugin));
  sstrncpy (vl.plugin_instance, plugin_instance,
	    sizeof(vl.plugin_instance));
  sstrncpy (vl.type, type, sizeof (vl.type));
  INFO("Pinba Dispatch");
  plugin_dispatch_values (&vl);
}

static int
plugin_read (void) {
  DEBUG("Pinba Read..");
  unsigned int i=0;
  static pinba_statres res;
  
  for(; i=service_statnode_collect(&res, i); ){
    plugin_submit(res.name, "pinba_view", &res);
  }
  
  return 0;
}

void module_register (void) {
  plugin_register_complex_config ("pinba", plugin_config);
  plugin_register_init ("pinba", plugin_init);
  plugin_register_read ("pinba", plugin_read);
  plugin_register_shutdown ("pinba", plugin_shutdown);
} /* void module_register */

