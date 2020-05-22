/*
 * Copyright (C) 2020 Red Hat
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgagroal */
#include <pgagroal.h>
#include <configuration.h>
#include <logging.h>
#include <pipeline.h>
#include <security.h>
#include <utils.h>

#define ZF_LOG_TAG "configuration"
#include <zf_log.h>

/* system */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_LENGTH 512

static void extract_key_value(char* str, char** key, char** value);
static int as_int(char* str);
static bool as_bool(char* str);
static int as_logging_type(char* str);
static int as_logging_level(char* str);
static int as_validation(char* str);
static int as_pipeline(char* str);
static int extract_value(char* str, int offset, char** value);
static void extract_hba(char* str, char** type, char** database, char** user, char** address, char** method);
static void extract_limit(char* str, int server_max, char** database, char** user, int* max_size, int* initial_size, int* min_size);

/**
 *
 */
int
pgagroal_init_configuration(void* shmem, size_t size)
{
   struct configuration* config;

   config = (struct configuration*)shmem;
   memset(config, 0, size);

   atomic_init(&config->active_connections, 0);
   
   for (int i = 0; i < NUMBER_OF_SERVERS; i++)
   {
      config->servers[i].primary = SERVER_NOTINIT;
   }

   config->tls = false;

   config->gracefully = false;

   config->pipeline = PIPELINE_AUTO;

   config->blocking_timeout = 30;
   config->idle_timeout = 0;
   config->validation = VALIDATION_OFF;
   config->background_interval = 300;
   config->max_retries = 5;
   config->authentication_timeout = 5;
   config->disconnect_client = 0;

   config->buffer_size = DEFAULT_BUFFER_SIZE;
   config->keep_alive = true;
   config->nodelay = true;
   config->non_blocking = true;
   config->backlog = -1;

   config->log_type = PGAGROAL_LOGGING_TYPE_CONSOLE;
   config->log_level = PGAGROAL_LOGGING_LEVEL_INFO;
   config->log_connections = false;
   config->log_disconnections = false;

   config->max_connections = 100;
   config->allow_unknown_users = true;

   for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
   {
      atomic_init(&config->prometheus.session_time[i], 0);
   }
   atomic_init(&config->prometheus.session_time_sum, 0);

   atomic_init(&config->prometheus.connection_error, 0);
   atomic_init(&config->prometheus.connection_kill, 0);
   atomic_init(&config->prometheus.connection_remove, 0);
   atomic_init(&config->prometheus.connection_timeout, 0);
   atomic_init(&config->prometheus.connection_return, 0);
   atomic_init(&config->prometheus.connection_invalid, 0);
   atomic_init(&config->prometheus.connection_get, 0);
   atomic_init(&config->prometheus.connection_idletimeout, 0);
   atomic_init(&config->prometheus.connection_flush, 0);
   atomic_init(&config->prometheus.connection_success, 0);

   atomic_init(&config->prometheus.auth_user_success, 0);
   atomic_init(&config->prometheus.auth_user_bad_password, 0);
   atomic_init(&config->prometheus.auth_user_error, 0);

   return 0;
}

/**
 *
 */
int
pgagroal_read_configuration(char* filename, void* shmem)
{
   FILE* file;
   char section[LINE_LENGTH];
   char line[LINE_LENGTH];
   char* key = NULL;
   char* value = NULL;
   char* ptr = NULL;
   size_t max;
   struct configuration* config;
   int idx_server = -1;
   struct server srv;

   file = fopen(filename, "r");

   if (!file)
      return 1;
    
   memset(&section, 0, LINE_LENGTH);
   config = (struct configuration*)shmem;

   while (fgets(line, sizeof(line), file))
   {
      if (strcmp(line, ""))
      {
         if (line[0] == '[')
         {
            ptr = strchr(line, ']');
            if (ptr)
            {
               memset(&section, 0, LINE_LENGTH);
               max = ptr - line - 1;
               if (max > MISC_LENGTH - 1)
                  max = MISC_LENGTH - 1;
               memcpy(&section, line + 1, max);
               if (strcmp(section, "pgagroal"))
               {
                  idx_server++;

                  if (idx_server > 0 && idx_server < NUMBER_OF_SERVERS)
                  {
                     memcpy(&(config->servers[idx_server]), &srv, sizeof(struct server));
                  }
                  else if (idx_server >= NUMBER_OF_SERVERS)
                  {
                     printf("Maximum number of servers exceeded\n");
                  }

                  memset(&srv, 0, sizeof(struct server));
                  memcpy(&srv.name, &section, strlen(section));
               }
            }
         }
         else if (line[0] == '#' || line[0] == ';')
         {
            /* Comment, so ignore */
         }
         else
         {
            extract_key_value(line, &key, &value);

            if (key && value)
            {
               bool unknown = false;

               /* printf("|%s|%s|\n", key, value); */

               if (!strcmp(key, "host"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->host, value, max);
                  }
                  else if (strlen(section) > 0)
                  {
                     max = strlen(section);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.name, section, max);
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.host, value, max);
                     srv.primary = SERVER_NOTINIT;
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "port"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->port = as_int(value);
                  }
                  else if (strlen(section) > 0)
                  {
                     memcpy(&srv.name, section, strlen(section));
                     srv.port = as_int(value);
                     srv.primary = SERVER_NOTINIT;
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "primary"))
               {
                  if (strcmp(section, "pgagroal") && strlen(section) > 0)
                  {
                     if (as_bool(value))
                     {
                        srv.primary = SERVER_NOTINIT_PRIMARY;
                     }
                     else
                     {
                        srv.primary = SERVER_NOTINIT;
                     }
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "metrics"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->metrics = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "management"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->management = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "pipeline"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->pipeline = as_pipeline(value);

                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "tls"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->tls = as_bool(value);
                  }
                  else if (strlen(section) > 0)
                  {
                     srv.tls = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "tls_ca_file"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->tls_ca_file, value, max);
                  }
                  else if (strlen(section) > 0)
                  {
                     max = strlen(section);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.name, section, max);
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.tls_ca_file, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "tls_cert_file"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->tls_cert_file, value, max);
                  }
                  else if (strlen(section) > 0)
                  {
                     max = strlen(section);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.name, section, max);
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.tls_cert_file, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "tls_key_file"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->tls_key_file, value, max);
                  }
                  else if (strlen(section) > 0)
                  {
                     max = strlen(section);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.name, section, max);
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(&srv.tls_key_file, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "blocking_timeout"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->blocking_timeout = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "idle_timeout"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->idle_timeout = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "validation"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->validation = as_validation(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "background_interval"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->background_interval = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "max_retries"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->max_retries = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "authentication_timeout"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->authentication_timeout = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "disconnect_client"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->disconnect_client = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "allow_unknown_users"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->allow_unknown_users = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "log_type"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->log_type = as_logging_type(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "log_level"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->log_level = as_logging_level(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "log_path"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->log_path, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "log_connections"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->log_connections = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "log_disconnections"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->log_disconnections = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "max_connections"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->max_connections = as_int(value);
                     if (config->max_connections > MAX_NUMBER_OF_CONNECTIONS)
                     {
                        config->max_connections = MAX_NUMBER_OF_CONNECTIONS;
                     }
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "unix_socket_dir"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->unix_socket_dir, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "libev"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     max = strlen(value);
                     if (max > MISC_LENGTH - 1)
                        max = MISC_LENGTH - 1;
                     memcpy(config->libev, value, max);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "buffer_size"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->buffer_size = as_int(value);

                     if (config->buffer_size > MAX_BUFFER_SIZE)
                     {
                        config->buffer_size = MAX_BUFFER_SIZE;
                     }
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "keep_alive"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->keep_alive = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "nodelay"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->nodelay = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "non_blocking"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->non_blocking = as_bool(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else if (!strcmp(key, "backlog"))
               {
                  if (!strcmp(section, "pgagroal"))
                  {
                     config->backlog = as_int(value);
                  }
                  else
                  {
                     unknown = true;
                  }
               }
               else
               {
                  unknown = true;
               }

               if (unknown)
               {
                  printf("Unknown: Section=%s, Key=%s, Value=%s\n", strlen(section) > 0 ? section : "<unknown>", key, value);
               }

               free(key);
               free(value);
               key = NULL;
               value = NULL;
            }
         }
      }
   }

   if (idx_server != -1 && strlen(srv.name) > 0)
   {
      memcpy(&(config->servers[idx_server]), &srv, sizeof(struct server));
      idx_server++;
   }

   config->number_of_servers = idx_server;

   fclose(file);

   return 0;
}

/**
 *
 */
int
pgagroal_validate_configuration(void* shmem)
{
   bool tls;
   struct configuration* config;

   tls = false;

   config = (struct configuration*)shmem;

   if (strlen(config->host) == 0)
   {
      ZF_LOGF("pgagroal: No host defined");
      return 1;
   }

   if (config->port <= 0)
   {
      ZF_LOGF("pgagroal: No port defined");
      return 1;
   }

   if (strlen(config->unix_socket_dir) == 0)
   {
      ZF_LOGF("pgagroal: No unix_socket_dir defined");
      return 1;
   }

   if (config->backlog <= 0)
   {
      config->backlog = MAX(config->max_connections / 4, 16);
   }

   if (config->authentication_timeout <= 0)
   {
      config->authentication_timeout = 5;
   }

   if (config->disconnect_client <= 0)
   {
      config->disconnect_client = 0;
   }

   if (config->number_of_servers <= 0)
   {
      ZF_LOGF("pgagroal: No servers defined");
      return 1;
   }

   for (int i = 0; i < config->number_of_servers; i++)
   {
      if (strlen(config->servers[i].host) == 0)
      {
         ZF_LOGF("pgagroal: No host defined for %s", config->servers[i].name);
         return 1;
      }

      if (config->servers[i].port == 0)
      {
         ZF_LOGF("pgagroal: No port defined for %s", config->servers[i].name);
         return 1;
      }

      if (config->servers[i].tls && (strlen(config->servers[i].tls_cert_file) > 0 || strlen(config->servers[i].tls_key_file) > 0))
      {
         tls = true;
      }
   }

   if (config->pipeline == PIPELINE_AUTO)
   {
      if (config->tls && (strlen(config->tls_cert_file) > 0 || strlen(config->tls_key_file) > 0))
      {
         tls = true;
      }

      if (tls || config->disconnect_client > 0)
      {
         config->pipeline = PIPELINE_SESSION;
      }
      else
      {
         config->pipeline = PIPELINE_PERFORMANCE;
      }
   }
   else if (config->pipeline == PIPELINE_PERFORMANCE)
   {
      if (config->tls && (strlen(config->tls_cert_file) > 0 || strlen(config->tls_key_file) > 0))
      {
         tls = true;
      }

      if (tls)
      {
         ZF_LOGF("pgagroal: Performance pipeline does not support TLS");
         return 1;
      }

      if (config->disconnect_client > 0)
      {
         ZF_LOGF("pgagroal: Performance pipeline does not support disconnect_client");
         return 1;
      }
   }

   return 0;
}

/**
 *
 */
int
pgagroal_read_hba_configuration(char* filename, void* shmem)
{
   FILE* file;
   char line[LINE_LENGTH];
   int index;
   char* type = NULL;
   char* database = NULL;
   char* username = NULL;
   char* address = NULL;
   char* method = NULL;
   struct configuration* config;

   file = fopen(filename, "r");

   if (!file)
      return 1;

   index = 0;
   config = (struct configuration*)shmem;

   while (fgets(line, sizeof(line), file))
   {
      if (strcmp(line, ""))
      {
         if (line[0] == '#' || line[0] == ';')
         {
            /* Comment, so ignore */
         }
         else
         {
            extract_hba(line, &type, &database, &username, &address, &method);

            if (type && database && username && address && method)
            {
               if (strlen(type) < MAX_TYPE_LENGTH &&
                   strlen(database) < MAX_DATABASE_LENGTH &&
                   strlen(username) < MAX_USERNAME_LENGTH &&
                   strlen(address) < MAX_ADDRESS_LENGTH &&
                   strlen(method) < MAX_ADDRESS_LENGTH)
               {
                  memcpy(&(config->hbas[index].type), type, strlen(type));
                  memcpy(&(config->hbas[index].database), database, strlen(database));
                  memcpy(&(config->hbas[index].username), username, strlen(username));
                  memcpy(&(config->hbas[index].address), address, strlen(address));
                  memcpy(&(config->hbas[index].method), method, strlen(method));

                  index++;

                  if (index >= NUMBER_OF_HBAS)
                  {
                     printf("pgagroal: Too many HBA entries (%d)\n", NUMBER_OF_HBAS);
                     fclose(file);
                     return 2;
                  }
               }
               else
               {
                  printf("pgagroal: Invalid HBA entry\n");
                  printf("%s\n", line);
               }
            }
            else
            {
               printf("pgagroal: Invalid HBA entry\n");
               printf("%s\n", line);
            }

            free(type);
            free(database);
            free(username);
            free(address);
            free(method);

            type = NULL;
            database = NULL;
            username = NULL;
            address = NULL;
            method = NULL;
         }
      }
   }

   config->number_of_hbas = index;

   fclose(file);

   return 0;
}

/**
 *
 */
int
pgagroal_validate_hba_configuration(void* shmem)
{
   struct configuration* config;

   config = (struct configuration*)shmem;

   if (config->number_of_hbas == 0)
   {
      ZF_LOGF("pgagroal: No HBA entry defined");
      return 1;
   }

   for (int i = 0; i < config->number_of_hbas; i++)
   {
      if (!strcasecmp("host", config->hbas[i].type) ||
          !strcasecmp("hostssl", config->hbas[i].type))
      {
         /* Ok */
      }
      else
      {
         ZF_LOGF("pgagroal: Unknown HBA type: %s", config->hbas[i].type);
         return 1;
      }

      if (!strcasecmp("trust", config->hbas[i].method) ||
          !strcasecmp("reject", config->hbas[i].method) ||
          !strcasecmp("password", config->hbas[i].method) ||
          !strcasecmp("md5", config->hbas[i].method) ||
          !strcasecmp("scram-sha-256", config->hbas[i].method) ||
          !strcasecmp("all", config->hbas[i].method))
      {
         /* Ok */
      }
      else
      {
         ZF_LOGF("pgagroal: Unknown HBA method: %s", config->hbas[i].method);
         return 1;
      }
   }

   return 0;
}

/**
 *
 */
int
pgagroal_read_limit_configuration(char* filename, void* shmem)
{
   FILE* file;
   char line[LINE_LENGTH];
   int index;
   char* database = NULL;
   char* username = NULL;
   int max_size;
   int initial_size;
   int min_size;
   struct configuration* config;

   file = fopen(filename, "r");

   if (!file)
      return 1;

   index = 0;
   config = (struct configuration*)shmem;

   while (fgets(line, sizeof(line), file))
   {
      if (strcmp(line, ""))
      {
         if (line[0] == '#' || line[0] == ';')
         {
            /* Comment, so ignore */
         }
         else
         {
            initial_size = 0;
            min_size = 0;

            extract_limit(line, config->max_connections, &database, &username, &max_size, &initial_size, &min_size);

            if (database && username && max_size > 0)
            {
               if (strlen(database) < MAX_DATABASE_LENGTH &&
                   strlen(username) < MAX_USERNAME_LENGTH)
               {
                  if (initial_size > max_size)
                  {
                     initial_size = max_size;
                  }
                  else if (initial_size < 0)
                  {
                     initial_size = 0;
                  }

                  if (min_size > max_size)
                  {
                     min_size = max_size;
                  }
                  else if (min_size < 0)
                  {
                     min_size = 0;
                  }

                  memcpy(&(config->limits[index].database), database, strlen(database));
                  memcpy(&(config->limits[index].username), username, strlen(username));
                  config->limits[index].max_size = max_size;
                  config->limits[index].initial_size = initial_size;
                  config->limits[index].min_size = min_size;
                  atomic_init(&config->limits[index].active_connections, 0);

                  index++;

                  if (index >= NUMBER_OF_LIMITS)
                  {
                     printf("pgagroal: Too many LIMIT entries (%d)\n", NUMBER_OF_LIMITS);
                     fclose(file);
                     return 2;
                  }
               }
               else
               {
                  printf("pgagroal: Invalid LIMIT entry\n");
                  printf("%s\n", line);
               }
            }
            else
            {
               printf("pgagroal: Invalid LIMIT entry\n");
               printf("%s\n", line);
            }

            free(database);
            free(username);

            database = NULL;
            username = NULL;
            max_size = 0;
         }
      }
   }

   config->number_of_limits = index;

   fclose(file);

   return 0;
}

/**
 *
 */
int
pgagroal_validate_limit_configuration(void* shmem)
{
   int total_connections;
   struct configuration* config;

   total_connections = 0;
   config = (struct configuration*)shmem;

   for (int i = 0; i < config->number_of_limits; i++)
   {
      total_connections += config->limits[i].max_size;

      if (config->limits[i].initial_size > 0 || config->limits[i].min_size > 0)
      {
         bool user_found = false;

         for (int j = 0; j < config->number_of_users; j++)
         {
            if (!strcmp(config->limits[i].username, config->users[j].username))
            {
               user_found = true;
            }
         }

         if (!user_found)
         {
            ZF_LOGW("Unknown user (%s) for limit entry (%d)", config->limits[i].username, i);
         }

         if (config->limits[i].initial_size < config->limits[i].min_size)
         {
            ZF_LOGW("initial_size smaller than min_size for limit entry (%d)", i);
            config->limits[i].initial_size = config->limits[i].min_size;
         }
      }
   }

   if (total_connections > config->max_connections)
   {
      ZF_LOGF("pgagroal: LIMIT: Too many connections defined %d (max %d)", total_connections, config->max_connections);
      return 1;
   }

   return 0;
}

/**
 *
 */
int
pgagroal_read_users_configuration(char* filename, void* shmem)
{
   FILE* file;
   char line[LINE_LENGTH];
   int index;
   char* master_key = NULL;
   char* username = NULL;
   char* password = NULL;
   char* decoded = NULL;
   int decoded_length = 0;
   char* ptr = NULL;
   struct configuration* config;

   file = fopen(filename, "r");

   if (!file)
   {
      goto error;
   }

   if (pgagroal_get_master_key(&master_key))
   {
      goto masterkey;
   }

   index = 0;
   config = (struct configuration*)shmem;

   while (fgets(line, sizeof(line), file))
   {
      if (strcmp(line, ""))
      {
         if (line[0] == '#' || line[0] == ';')
         {
            /* Comment, so ignore */
         }
         else
         {
            ptr = strtok(line, ":");

            username = ptr;

            ptr = strtok(NULL, ":");

            if (pgagroal_base64_decode(ptr, strlen(ptr), &decoded, &decoded_length))
            {
               goto error;
            }

            if (pgagroal_decrypt(decoded, decoded_length, master_key, &password))
            {
               goto error;
            }

            if (strlen(username) < MAX_USERNAME_LENGTH &&
                strlen(password) < MAX_PASSWORD_LENGTH)
            {
               memcpy(&config->users[index].username, username, strlen(username));
               memcpy(&config->users[index].password, password, strlen(password));
            }
            else
            {
               printf("pgagroal: Invalid USER entry\n");
               printf("%s\n", line);
            }

            free(password);
            free(decoded);

            password = NULL;
            decoded = NULL;

            index++;
         }
      }
   }

   config->number_of_users = index;

   if (config->number_of_users > NUMBER_OF_USERS)
   {
      goto above;
   }

   free(master_key);

   fclose(file);

   return 0;

error:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 1;

masterkey:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 2;

above:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 3;
}

/**
 *
 */
int
pgagroal_validate_users_configuration(void* shmem)
{
   return 0;
}

/**
 *
 */
int
pgagroal_read_admins_configuration(char* filename, void* shmem)
{
   FILE* file;
   char line[LINE_LENGTH];
   int index;
   char* master_key = NULL;
   char* username = NULL;
   char* password = NULL;
   char* decoded = NULL;
   int decoded_length = 0;
   char* ptr = NULL;
   struct configuration* config;

   file = fopen(filename, "r");

   if (!file)
   {
      goto error;
   }

   if (pgagroal_get_master_key(&master_key))
   {
      goto masterkey;
   }

   index = 0;
   config = (struct configuration*)shmem;

   while (fgets(line, sizeof(line), file))
   {
      if (strcmp(line, ""))
      {
         if (line[0] == '#' || line[0] == ';')
         {
            /* Comment, so ignore */
         }
         else
         {
            ptr = strtok(line, ":");

            username = ptr;

            ptr = strtok(NULL, ":");

            if (pgagroal_base64_decode(ptr, strlen(ptr), &decoded, &decoded_length))
            {
               goto error;
            }

            if (pgagroal_decrypt(decoded, decoded_length, master_key, &password))
            {
               goto error;
            }

            if (strlen(username) < MAX_USERNAME_LENGTH &&
                strlen(password) < MAX_PASSWORD_LENGTH)
            {
               memcpy(&config->admins[index].username, username, strlen(username));
               memcpy(&config->admins[index].password, password, strlen(password));
            }
            else
            {
               printf("pgagroal: Invalid ADMIN entry\n");
               printf("%s\n", line);
            }

            free(password);
            free(decoded);

            password = NULL;
            decoded = NULL;

            index++;
         }
      }
   }

   config->number_of_admins = index;

   if (config->number_of_admins > NUMBER_OF_ADMINS)
   {
      goto above;
   }

   free(master_key);

   fclose(file);

   return 0;

error:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 1;

masterkey:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 2;

above:

   free(master_key);
   free(password);
   free(decoded);

   if (file)
   {
      fclose(file);
   }

   return 3;
}

/**
 *
 */
int
pgagroal_validate_admins_configuration(void* shmem)
{
   struct configuration* config;

   config = (struct configuration*)shmem;

   if (config->management > 0 && config->number_of_admins == 0)
   {
      ZF_LOGW("pgagroal: Remote management enabled, but no admins are defined");
   }

   return 0;
}

static void
extract_key_value(char* str, char** key, char** value)
{
   int c = 0;
   int offset = 0;
   int length = strlen(str);
   char* k;
   char* v;

   while (str[c] != ' ' && str[c] != '=' && c < length)
      c++;

   if (c < length)
   {
      k = malloc(c + 1);
      memset(k, 0, c + 1);
      memcpy(k, str, c);
      *key = k;

      while ((str[c] == ' ' || str[c] == '\t' || str[c] == '=') && c < length)
         c++;

      offset = c;

      while (str[c] != ' ' && str[c] != '\r' && str[c] != '\n' && c < length)
         c++;

      if (c < length)
      {
         v = malloc((c - offset) + 1);
         memset(v, 0, (c - offset) + 1);
         memcpy(v, str + offset, (c - offset));
         *value = v;
      }
   }
}

static int as_int(char* str)
{
   return atoi(str);
}

static bool as_bool(char* str)
{
   if (!strcasecmp(str, "true"))
      return true;

   if (!strcasecmp(str, "on"))
      return true;

   if (!strcasecmp(str, "1"))
      return true;

   if (!strcasecmp(str, "false"))
      return false;

   if (!strcasecmp(str, "off"))
      return false;

   if (!strcasecmp(str, "0"))
      return false;

   return false;
}

static int as_logging_type(char* str)
{
   if (!strcasecmp(str, "console"))
      return PGAGROAL_LOGGING_TYPE_CONSOLE;

   if (!strcasecmp(str, "file"))
      return PGAGROAL_LOGGING_TYPE_FILE;

   if (!strcasecmp(str, "syslog"))
      return PGAGROAL_LOGGING_TYPE_SYSLOG;

   return 0;
}

static int as_logging_level(char* str)
{
   if (!strcasecmp(str, "debug5"))
      return PGAGROAL_LOGGING_LEVEL_DEBUG5;

   if (!strcasecmp(str, "debug4"))
      return PGAGROAL_LOGGING_LEVEL_DEBUG4;

   if (!strcasecmp(str, "debug3"))
      return PGAGROAL_LOGGING_LEVEL_DEBUG3;

   if (!strcasecmp(str, "debug2"))
      return PGAGROAL_LOGGING_LEVEL_DEBUG2;

   if (!strcasecmp(str, "debug1"))
      return PGAGROAL_LOGGING_LEVEL_DEBUG1;

   if (!strcasecmp(str, "info"))
      return PGAGROAL_LOGGING_LEVEL_INFO;

   if (!strcasecmp(str, "warn"))
      return PGAGROAL_LOGGING_LEVEL_WARN;

   if (!strcasecmp(str, "error"))
      return PGAGROAL_LOGGING_LEVEL_ERROR;

   if (!strcasecmp(str, "fatal"))
      return PGAGROAL_LOGGING_LEVEL_FATAL;

   return PGAGROAL_LOGGING_LEVEL_INFO;
}

static int as_validation(char* str)
{
   if (!strcasecmp(str, "off"))
      return VALIDATION_OFF;

   if (!strcasecmp(str, "foreground"))
      return VALIDATION_FOREGROUND;

   if (!strcasecmp(str, "background"))
      return VALIDATION_BACKGROUND;

   return VALIDATION_OFF;
}

static int
as_pipeline(char* str)
{
   if (!strcasecmp(str, "auto"))
      return PIPELINE_AUTO;

   if (!strcasecmp(str, "performance"))
      return PIPELINE_PERFORMANCE;

   if (!strcasecmp(str, "session"))
      return PIPELINE_SESSION;

   return PIPELINE_AUTO;
}

static void
extract_hba(char* str, char** type, char** database, char** user, char** address, char** method)
{
   int offset = 0;
   int length = strlen(str);

   offset = extract_value(str, offset, type);

   if (offset == -1 || offset >= length)
      return;

   offset = extract_value(str, offset, database);

   if (offset == -1 || offset >= length)
      return;

   offset = extract_value(str, offset, user);

   if (offset == -1 || offset >= length)
      return;

   offset = extract_value(str, offset, address);

   if (offset == -1 || offset >= length)
      return;

   extract_value(str, offset, method);
}

static void
extract_limit(char* str, int server_max, char** database, char** user, int* max_connections, int* initial_size, int* min_size)
{
   int offset = 0;
   int length = strlen(str);
   char* value = NULL;

   offset = extract_value(str, offset, database);

   if (offset == -1 || offset >= length)
      return;

   offset = extract_value(str, offset, user);

   if (offset == -1 || offset >= length)
      return;

   offset = extract_value(str, offset, &value);

   if (offset == -1)
      return;

   if (!strcmp("all", value))
   {
      *max_connections = server_max;
   }
   else
   {
      *max_connections = atoi(value);
   }

   free(value);
   value = NULL;

   offset = extract_value(str, offset, &value);

   if (offset == -1)
      return;

   *initial_size = atoi(value);

   free(value);
   value = NULL;

   offset = extract_value(str, offset, &value);

   if (offset == -1)
      return;

   *min_size = atoi(value);

   free(value);
}

static int
extract_value(char* str, int offset, char** value)
{
   int from;
   int to;
   int length = strlen(str);
   char* v = NULL;

   while ((str[offset] == ' ' || str[offset] == '\t') && offset < length)
      offset++;

   if (offset < length)
   {
      from = offset;

      while ((str[offset] != ' ' && str[offset] != '\t' && str[offset] != '\r' && str[offset] != '\n') && offset < length)
         offset++;

      if (offset < length)
      {
         to = offset;

         v = malloc(to - from + 1);
         memset(v, 0, to - from + 1);
         memcpy(v, str + from, to - from);
         *value = v;

         return offset;
      }
   }

   return -1;
}
