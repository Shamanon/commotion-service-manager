/**
 *       @file  main.c
 *      @brief  Entry point and commands for CSM daemon
 *
 *     @author  Dan Staples (dismantl), danstaples@opentechinstitute.org
 *
 * This file is part of Commotion, Copyright (c) 2013, Josh King 
 * 
 * Commotion is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 * 
 * Commotion is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Commotion.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>

#include <avahi-common/error.h>
#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-core/core.h>
#include <avahi-core/lookup.h>
#ifdef CLIENT
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#endif

#include "commotion/obj.h"
#include "commotion/cmd.h"
#include "commotion/msg.h"
#include "commotion/list.h"
#include "commotion/tree.h"
#include "commotion/socket.h"
#include "commotion/util.h"
#include "commotion.h"

#include "defs.h"
#include "util.h"
#include "service.h"
#include "browse.h"
#include "publish.h"
#include "debug.h"
#include "commotion-service-manager.h"

#define REQUEST_MAX 1024
#define RESPONSE_MAX 1024

extern co_socket_t unix_socket_proto;
extern ServiceInfo *services;

csm_config config;
static int pid_filehandle;
static co_socket_t *csm_socket = NULL;

AvahiSimplePoll *simple_poll = NULL;
TYPE_BROWSER *stb = NULL;
#ifdef CLIENT
AvahiClient *client = NULL;
#else
AvahiServer *server = NULL;
#endif

static co_obj_t *
_cmd_help_i(co_obj_t *data, co_obj_t *current, void *context) 
{
  char *cmd_name = NULL;
  size_t cmd_len = 0;
  CHECK((cmd_len = co_obj_data(&cmd_name, ((co_cmd_t *)current)->name)) > 0, "Failed to read command name.");
  DEBUG("Command: %s, Length: %d", cmd_name, (int)cmd_len);
  co_tree_insert((co_obj_t *)context, cmd_name, cmd_len, ((co_cmd_t *)current)->usage);
  return NULL;
  error:
  return NULL;
}

CMD(help)
{
  *output = co_tree16_create();
  if (params != NULL && co_list_length(params) > 0)
  {
    co_obj_t *cmd = co_list_element(params, 0);
    if (cmd != NULL && IS_STR(cmd))
    {
      char *cstr = NULL;
      size_t clen = co_obj_data(&cstr, cmd);
      if (clen > 0)
      {
	co_tree_insert(*output, cstr, clen, co_cmd_desc(cmd));
	return 1;
      }
    }
    else return 0;
  }
  return co_cmd_process(_cmd_help_i, (void *)*output);
}

/** Add OR update a service */
CMD(commit_service) {
  CHECK(IS_LIST(params),"Received invalid params");
  
  co_obj_t *service = co_list_element(params,0);
  CHECK(IS_TREE(service),"Received invalid service");

  co_obj_t *name_obj = co_tree_find(service,"name",sizeof("name"));
  co_obj_t *description_obj = co_tree_find(service,"description",sizeof("description"));
  co_obj_t *uri_obj = co_tree_find(service,"uri",sizeof("uri"));
  co_obj_t *icon_obj = co_tree_find(service,"icon",sizeof("icon"));
  co_obj_t *ttl_obj = co_tree_find(service,"ttl",sizeof("ttl"));
  co_obj_t *lifetime_obj = co_tree_find(service,"lifetime",sizeof("lifetime"));
  co_obj_t *categories_obj = co_tree_find(service,"categories",sizeof("categories"));
  co_obj_t *key_obj = co_tree_find(service,"key",sizeof("key"));
  
  /* Check required fields */
  CHECK(name_obj && description_obj && uri_obj && icon_obj,
	"Service missing required fields");
  
  ServiceInfo *i = NULL;
  
  if (key_obj) {
    char *key = NULL;
    size_t key_len = co_obj_data(&key, key_obj);
    CHECK(key_len == FINGERPRINT_LEN, "Invalid key");
    char uuid[UUID_LEN + 1] = {0};
    CHECK(get_uuid(key,key_len,uuid,UUID_LEN + 1) == UUID_LEN, "Failed to get UUID");
    i = find_service(uuid);
    CHECK(i, "Failed to find service");
    CHECK(i->key && isValidFingerprint(i->key,strlen(i->key)),"Found service missing or invalid key");
    if (i->signature)
      h_free(i->signature); // free signature so a new one is created in process_service
  } else {
    // create ServiceInfo
    i = add_service(NULL, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, NULL, "_commotion._tcp", "mesh.local");
  }
  
  /* NOTE: hostname might change during life of program, so don't add it to local services */
//   char hostname[HOST_NAME_MAX] = {0};
//   CHECK(gethostname(hostname,HOST_NAME_MAX) == 0, "Failed to get hostname");
  
  // set fields
  CSM_SET(i, version, "1.1");
//   CSM_SET(i, host_name, hostname);
  CSM_SET(i, name, co_obj_data_ptr(name_obj));
  CSM_SET(i, description, co_obj_data_ptr(description_obj));
  CSM_SET(i, uri, co_obj_data_ptr(uri_obj));
  CSM_SET(i, icon, co_obj_data_ptr(icon_obj));
  if (ttl_obj && IS_INT(ttl_obj))
    i->ttl = (int)(*co_obj_data_ptr(ttl_obj));
  if (lifetime_obj && IS_INT(lifetime_obj))
    i->lifetime = (long)(*co_obj_data_ptr(lifetime_obj));
  if (categories_obj
      && IS_LIST(categories_obj)
      && co_list_length(categories_obj) > 0) {
    i->cat_len = co_list_length(categories_obj);
    i->categories = h_realloc(i->categories,(i->cat_len)*sizeof(char*));
    CHECK_MEM(i->categories);
    hattach(i->categories, i);
    for (int j = 0; j < i->cat_len; j++)
      CSM_SET(i, categories[j], _LIST_ELEMENT(categories_obj,j));
  }
  
  if (process_service(i)) {
    i->uptodate = 0; // flag used to indicate need to re-register w/ avahi server
    // TODO call register_service
    // send back success, key, signature
    CHECK(i->key && i->signature && i->uuid, "Failed to get key and signature");
    CMD_OUTPUT("success",co_bool_create(true,0));
    CMD_OUTPUT("key",co_str8_create(i->key,strlen(i->key)+1,0));
    CMD_OUTPUT("signature",co_str8_create(i->signature,strlen(i->signature)+1,0));
  } else {
    // remove service, send back failure
    remove_service(NULL, i);
    CMD_OUTPUT("success",co_bool_create(false,0));
  }
  
  return 1;
error:
  return 0;
}

CMD(remove_service) {
  CHECK(IS_LIST(params),"Received invalid params");
  
  co_obj_t *key_obj = co_list_element(params,0);
  CHECK(IS_STR(key_obj),"Received invalid key");
  
  char *key = NULL;
  size_t key_len = co_obj_data(&key,key_obj);
  
  CHECK(isValidFingerprint(key,key_len),"Received invalid key");
  
  ServiceInfo *i = services;
  
  for (; i; i = i->info_next) {
    if (strcasecmp(key,i->key) == 0)
      break;
  }
  
  if (i) {
    // TODO call unregister_service
    remove_service(NULL, i);
    CMD_OUTPUT("success",co_bool_create(true,0));
  } else {
    CMD_OUTPUT("success",co_bool_create(false,0));
  }
  
  return 1;
error:
  return 0;
}

CMD(list_services) {
  // if we have more than UINT16_MAX services, we have more important problems
  co_obj_t *service_list = co_list16_create();
  CHECK_MEM(service_list);
  
  ServiceInfo *i = services;
  if (!i) {
    CMD_OUTPUT("success",co_bool_create(false,0));
    return 1;
  }
  
  for (; i; i = i->info_next) {
    // make sure service is resolved before inserting
    if (i->resolved) {
      co_obj_t *service = co_tree16_create();
      CHECK_MEM(service);
      
      SERVICE_SET_STR(service,"name",i->name);
      SERVICE_SET_STR(service,"description",i->description);
      SERVICE_SET_STR(service,"uri",i->uri);
      SERVICE_SET_STR(service,"icon",i->icon);
      SERVICE_SET(service,"ttl",co_uint8_create(i->ttl,0));
      SERVICE_SET(service,"lifetime",co_uint32_create(i->lifetime,0));
      SERVICE_SET_STR(service,"key",i->key);
      SERVICE_SET_STR(service,"signature",i->signature);
      SERVICE_SET_STR(service,"version",i->version);
      co_obj_t *category_list = co_list16_create();
      CHECK_MEM(category_list);
      for (int j = 0; j < i->cat_len; j++) {
	CHECK(co_list_append(category_list,co_str8_create(i->categories[j],strlen(i->categories[j])+1,0)),"Failed to insert category");
      }
      SERVICE_SET(service,"categories",category_list);
      
      CHECK(co_list_append(service_list,service),"Failed to append service");
    }
  }
  
  CMD_OUTPUT("services",service_list);
  CMD_OUTPUT("success",co_bool_create(true,0));
  
  return 1;
error:
  return 0;
}

static void socket_send(int fd, char const *str, size_t len) {
  unsigned int sent = 0;
  unsigned int remaining = len;
  int n;
  while(sent < len) {
    n = send(fd, str+sent, remaining, 0);
    if (n < 0) break;
    sent += n;
    remaining -= n;
  }
  DEBUG("Sent %d bytes.", sent);
}

static void request_handler(AvahiWatch *w, int fd, AvahiWatchEvent events, void *userdata) {
  char reqbuf[REQUEST_MAX], respbuf[RESPONSE_MAX];
  ssize_t reqlen = 0;
  size_t resplen = 0;
  co_obj_t *request = NULL, *ret = NULL, *nil = co_nil_create(0);
  uint8_t *type = NULL;
  uint32_t *id = NULL;
  
  memset(reqbuf, '\0', sizeof(reqbuf));
  memset(respbuf, '\0', sizeof(respbuf));
  
  if (events & AVAHI_WATCH_HUP) {
    close(fd);
    avahi_simple_poll_get(simple_poll)->watch_free(w);
    DEBUG("HUP from %d",fd);
    return;
  }
  
  INFO("Received connection from %d", fd);
  if (csm_socket->fd->fd == fd) {
    int rfd;
    DEBUG("Accepting connection (fd=%d).", fd);
    CHECK((rfd = accept(fd, NULL, NULL)) != -1, "Failed to accept connection.");
    DEBUG("Accepted connection (fd=%d).", rfd);
    co_obj_t *new_rfd = co_fd_create((co_obj_t*)csm_socket,rfd);
    CHECK(co_list_append(csm_socket->rfd_lst,new_rfd),"Failed to append rfd");
    int flags = fcntl(rfd, F_GETFL, 0);
    fcntl(rfd, F_SETFL, flags | O_NONBLOCK); //Set non-blocking.
    avahi_simple_poll_get(simple_poll)->watch_new(avahi_simple_poll_get(simple_poll), rfd, AVAHI_WATCH_IN | AVAHI_WATCH_IN, request_handler, NULL);
    return;
  }
  reqlen = recv(fd,reqbuf,sizeof(reqbuf),0);
  DEBUG("Received %d bytes from %d", (int)reqlen,fd);
  if (reqlen < 0) {
    INFO("Connection recvd() -1");
    close(fd);
    avahi_simple_poll_get(simple_poll)->watch_free(w);
    return;
  }
  
  /* If it's a commotion message type, parse the header, target and payload */
  CHECK(co_list_import(&request, reqbuf, reqlen) > 0, "Failed to import request.");
  co_obj_data((char **)&type, co_list_element(request, 0));
  CHECK(*type == 0, "Not a valid request.");
  CHECK(co_obj_data((char **)&id, co_list_element(request, 1)) == sizeof(uint32_t), "Not a valid request ID.");
  
  /* Run command */
  if(co_cmd_exec(co_list_element(request, 2), &ret, co_list_element(request, 3))) {
    resplen = co_response_alloc(respbuf, sizeof(respbuf), *id, nil, ret);
    socket_send(fd,respbuf,resplen);
  } else {
    if(ret == NULL) {
      ret = co_tree16_create();
      co_tree_insert(ret, "error", sizeof("error"), co_str8_create("Incorrect command.", sizeof("Incorrect command."), 0));
    }
    resplen = co_response_alloc(respbuf, sizeof(respbuf), *id, ret, nil);
    socket_send(fd,respbuf,resplen);
  }
  
error:
  if (request) co_obj_free(request);
}

static void csm_shutdown(int signal) {
      DEBUG("Received %s, goodbye!", signal == SIGINT ? "SIGINT" : "SIGTERM");
      avahi_simple_poll_quit(simple_poll);
}

/**
 * Starts the daemon
 * @param pidfile name of lock file (stores process id)
 * @warning ensure that there is only one copy 
 * @note if compiled with Syslog support, sets up syslog logging log
 */
static void daemon_start(char *pidfile) {
  int pid, sid, i;
  char str[10];
  
  /*
   * Check if parent process id is set
   * 
   * If PPID exists, we are already a daemon
   */
  if (getppid() == 1) {
    return;
  }
  
  pid = fork(); /* Fork parent process */
  
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  
  if (pid > 0) {
    /* Child created correctly */
    printf("Child process created: %d\n", pid);
    exit(EXIT_SUCCESS); /* exit parent process */
  }
  
  /* Child continues from here */
  
  /* 
   * Set file permissions to 750 
   * -- Owner may read/write/execute
   * -- Group may read/write
   * -- World has no permissions
   */
  umask(027);
  
#ifdef USESYSLOG
  openlog("Commotion",LOG_PID,LOG_USER); 
#endif
  
  /* Get a new process group */
  sid = setsid();
  
  if (sid < 0) {
    exit(EXIT_FAILURE);
  }
  
  /* Close all descriptors */
  for (i = getdtablesize(); i >=0; --i) {
    close(i);
  }
  
  /* Route i/o connections */
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  
  if ((chdir("/")) < 0) {
    exit(EXIT_FAILURE);
  }
  
  /*
   * Open lock file
   * Ensure that there is only one copy
   */
  pid_filehandle = open(pidfile, O_RDWR|O_CREAT, 0644);
  
  if(pid_filehandle == -1) {
    /* Couldn't open lock file */
    ERROR("Could not lock PID lock file %s, exiting", pidfile);
    exit(EXIT_FAILURE);
  }
  
  /* Get and format PID */
  sprintf(str, "%d\n", getpid());
  
  /* Write PID to lockfile */
  write(pid_filehandle, str, strlen(str));
  
}

static int
create_service_browser(void)
{
  /* Create the service browser */
  CHECK((stb = TYPE_BROWSER_NEW(AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "mesh.local", 0, browse_type_callback)),
	"Failed to create service browser: %s", AVAHI_ERROR);
  return 1;
  error:
  return 0;
}

#ifdef CLIENT
static void client_callback(AvahiClient *c, AvahiClientState state, AVAHI_GCC_UNUSED void * userdata) {
    assert(c);

    /* Called whenever the client or server state changes */
    switch (state) {
      case AVAHI_CLIENT_S_RUNNING:
	/* The server has startup successfully and registered its host
	 * name on the network, so it's time to create our services */
	if (!stb) {
	  if (!create_service_browser()) {
	    ERROR("Failed to create service type browser");
	    avahi_simple_poll_quit(simple_poll);
	  }
	}
	register_all(c);
	break;
      case AVAHI_CLIENT_CONNECTING:
	/* Avahi daemon is not currently running */
	// make sure nothing registers apps during this state
	// see: avahi-commotion/defs.h
	sleep(1);
	break;
      case AVAHI_CLIENT_S_COLLISION:
	/* Let's drop our registered services. When the server is back
	 * in AVAHI_CLIENT_S_RUNNING state we will register them
	 * again with the new host name. */
	
	/* drop through */
      case AVAHI_CLIENT_S_REGISTERING:
	/* The server records are now being established. This
	 * might be caused by a host name change. We need to wait
	 * for our own records to register until the host name is
	 * properly esatblished. */
	unregister_all();
	break;
      case AVAHI_CLIENT_FAILURE:
	if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
	  /* Remove all local services */
	  unregister_all();
	  
	  /* Free service type browser */
	  if (stb)
	    TYPE_BROWSER_FREE(stb);
	  
	  /* Free client */
	  FREE_AVAHI();
	  
	  /* Create new client */
	  int error;
	  client = avahi_client_new(avahi_simple_poll_get(simple_poll), AVAHI_CLIENT_NO_FAIL, client_callback, NULL, &error);
	  if (!client) {
	    ERROR("Failed to create client: %s", avahi_strerror(error));
	    avahi_simple_poll_quit(simple_poll);
	  }
	} else {
	  ERROR("Server connection failure: %s", avahi_strerror(avahi_client_errno(c)));
	  avahi_simple_poll_quit(simple_poll);
	}
	break;
      default:
	;
    }
}
#else
static void server_callback(AvahiServer *s, AvahiServerState state, AVAHI_GCC_UNUSED void * userdata) {
  assert(s);

  /* Called whenever the server state changes */
  switch (state) {
    case AVAHI_SERVER_RUNNING:
      /* The serve has startup successfully and registered its host
       * name on the network, so it's time to create our services */
      register_all(s);
      break;
    case AVAHI_SERVER_COLLISION: {
      char *n;
      int r;
      /* A host name collision happened. Let's pick a new name for the server */
      n = avahi_alternative_host_name(avahi_server_get_host_name(s));
      ERROR("Host name collision, retrying with '%s'", n);
      r = avahi_server_set_host_name(s, n);
      avahi_free(n);
      if (r < 0) {
	ERROR("Failed to set new host name: %s", avahi_strerror(r));
	avahi_simple_poll_quit(simple_poll);
	return;
      }
    }
      /* Fall through */
    case AVAHI_SERVER_REGISTERING:
      /* Let's drop our registered services. When the server is back
       * in AVAHI_SERVER_RUNNING state we will register them
       * again with the new host name. */
      unregister_all();
      break;
    case AVAHI_SERVER_FAILURE:
      /* Terminate on failure */
      ERROR("Server failure: %s", avahi_strerror(avahi_server_errno(s)));
      avahi_simple_poll_quit(simple_poll);
      break;
    default:
      ;
  }
}
#endif

/** Parse commandline options */
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
  switch (key) {
    case 'b':
      config.co_sock = arg;
      break;
      #ifdef USE_UCI
    case 'u':
      config.uci = 1;
      break;
      #endif
    case 'o':
      config.output_file = arg;
      break;
    case 'n':
      config.nodaemon = 1;
      break;
    case 'p':
      config.pid_file = arg;
      break;
    case 's':
      config.sid = arg;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

int main(int argc, char*argv[]) {
#ifndef CLIENT
    AvahiServerConfig avahi_config;
#endif
    int error;
    int ret = 1;

    argp_program_version = "1.0";
    static char doc[] = "Commotion Service Manager";
    static struct argp_option options[] = {
      {"bind", 'b', "URI", 0, "commotiond management socket"},
      {"nodaemon", 'n', 0, 0, "Do not fork into the background" },
      {"out", 'o', "FILE", 0, "Output file to write services to when USR1 signal is received" },
      {"pid", 'p', "FILE", 0, "Specify PID file"},
#ifdef USE_UCI
      {"uci", 'u', 0, 0, "Store service cache in UCI" },
#endif
      {"sid", 's', "SID", 0, "SID to use to sign service announcements"},
      { 0 }
    };
    static struct argp argp = { options, parse_opt, NULL, doc };
    
    /* Set defaults */
    config.co_sock = COMMOTION_MANAGESOCK;
#ifdef USE_UCI
    config.uci = 0;
#endif
    config.nodaemon = 0;
    config.output_file = CSM_DUMPFILE;
    config.pid_file = CSM_PIDFILE;
    config.sid = NULL;
    
    /* Set Avahi allocator to use halloc */
    static AvahiAllocator hallocator = {
      .malloc = h_malloc,
      .free = h_free,
      .realloc = h_realloc,
      .calloc = h_calloc
    };
    avahi_set_allocator(&hallocator);
    
    argp_parse (&argp, argc, argv, 0, 0, &config);
    //fprintf(stdout,"uci: %d, out: %s\n",config.uci,config.output_file);
    
    if (!config.nodaemon)
      daemon_start(config.pid_file);
    
    /* Initialize socket pool for connecting to commotiond */
    CHECK(co_init(),"Failed to initialize Commotion client");
    
    /* Register signal handlers */
    struct sigaction sa = {{0}};
//     sa.sa_handler = print_services;
//     CHECK(sigaction(SIGUSR1,&sa,NULL) == 0, "Failed to set signal handler");
    sa.sa_handler = csm_shutdown;
    CHECK(sigaction(SIGINT,&sa,NULL) == 0, "Failed to set signal handler");
    CHECK(sigaction(SIGTERM,&sa,NULL) == 0, "Failed to set signal handler");

    /* Initialize the psuedo-RNG */
    srand(time(NULL));

    /* Allocate main loop object */
    CHECK((simple_poll = avahi_simple_poll_new()),"Failed to create simple poll object.");

#ifdef CLIENT
    /* Allocate a new client */
    client = avahi_client_new(avahi_simple_poll_get(simple_poll), AVAHI_CLIENT_NO_FAIL, client_callback, NULL, &error);
    CHECK(client,"Failed to create client: %s", avahi_strerror(error));
#else
    /* Do not publish any local records */
    avahi_server_config_init(&avahi_config);
    CHECK_MEM((avahi_config.host_name = calloc(HOST_NAME_MAX,sizeof(char))));
    CHECK(gethostname(avahi_config.host_name,HOST_NAME_MAX) == 0, "Failed to fetch hostname");
    avahi_config.publish_workstation = 0;

    /* Allocate a new server */
    server = avahi_server_new(avahi_simple_poll_get(simple_poll), &avahi_config, server_callback, NULL, &error);

    /* Free the configuration data */
    avahi_server_config_free(&avahi_config);

    /* Check wether creating the server object succeeded */
    CHECK(server,"Failed to create server: %s", avahi_strerror(error));
    
    CHECK(create_service_browser(),"Failed to create service type browser");
#endif
    
    /*TODO*/
    /* Register commands */
    co_cmds_init(16);
    CMD_REGISTER(help, "help <none>", "Print list of commands and usage information.");
    CMD_REGISTER(commit_service, "commit_service ........", "Add or update local service.");
    CMD_REGISTER(remove_service, "remove_service <key>", "Remove local service.");
    CMD_REGISTER(list_services, "list_services <none>", "List services on local Commotion network.");
    
    /* Set up CSM management socket */
    csm_socket = (co_socket_t*)NEW(co_socket, unix_socket);
    csm_socket->bind((co_obj_t*)csm_socket, CSM_MANAGESOCK);
    AvahiWatch *csm_watch = avahi_simple_poll_get(simple_poll)->watch_new(avahi_simple_poll_get(simple_poll), csm_socket->fd->fd, AVAHI_WATCH_IN | AVAHI_WATCH_HUP, request_handler, NULL);
    
    /* Run the main loop */
    avahi_simple_poll_loop(simple_poll);
    
    ret = 0;

error:

    /* Close commotiond socket connection */
    co_shutdown();

    /* Cleanup things */
    if (stb)
        TYPE_BROWSER_FREE(stb);

    /* Remove main socket watch */
    avahi_simple_poll_get(simple_poll)->watch_free(csm_watch);

    /* Free server/client */
    FREE_AVAHI();

    /* Free event loop */
    if (simple_poll)
        avahi_simple_poll_free(simple_poll);

    return ret;
}