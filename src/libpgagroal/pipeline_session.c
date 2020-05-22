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
#include <message.h>
#include <pipeline.h>
#include <shmem.h>
#include <worker.h>

#define ZF_LOG_TAG "pipeline_session"
#include <zf_log.h>

/* system */
#include <errno.h>
#include <ev.h>
#include <stdlib.h>

static int  session_initialize(void*, void**, size_t*);
static void session_start(struct worker_io*);
static void session_client(struct ev_loop *loop, struct ev_io *watcher, int revents);
static void session_server(struct ev_loop *loop, struct ev_io *watcher, int revents);
static void session_stop(struct worker_io*);
static void session_destroy(void*, size_t);
static void session_periodic(void*, void*);

#define CLIENT_INIT   0
#define CLIENT_IDLE   1
#define CLIENT_ACTIVE 2
#define CLIENT_CHECK  3

struct client_session
{
   atomic_schar state; /**< The state */
   time_t timestamp;   /**< The last used timestamp */
};

static void client_active(int, void*);
static void client_inactive(int, void*);

struct pipeline session_pipeline()
{
   struct pipeline pipeline;

   pipeline.initialize = &session_initialize;
   pipeline.start = &session_start;
   pipeline.client = &session_client;
   pipeline.server = &session_server;
   pipeline.stop = &session_stop;
   pipeline.destroy = &session_destroy;
   pipeline.periodic = &session_periodic;

   return pipeline;
}

static int
session_initialize(void* shmem, void** pipeline_shmem, size_t* pipeline_shmem_size)
{
   void* session_shmem = NULL;
   size_t session_shmem_size;
   struct client_session* client;
   struct configuration* config;

   config = (struct configuration*)shmem;

   if (config->disconnect_client > 0)
   {
      session_shmem_size = config->max_connections * sizeof(struct client_session);
      session_shmem = pgagroal_create_shared_memory(session_shmem_size);
      memset(session_shmem, 0, session_shmem_size);

      for (int i = 0; i < config->max_connections; i++)
      {
         client = session_shmem + (i * sizeof(struct client_session));

         atomic_init(&client->state, CLIENT_INIT);
         client->timestamp = time(NULL);
      }

      *pipeline_shmem = session_shmem;
      *pipeline_shmem_size = session_shmem_size;
   }

   return 0;
}

static void
session_start(struct worker_io* w)
{
   struct client_session* client;

   if (w->pipeline_shmem != NULL)
   {
      client = w->pipeline_shmem + (w->slot * sizeof(struct client_session));

      atomic_store(&client->state, CLIENT_IDLE);
      client->timestamp = time(NULL);
   }
}

static void
session_stop(struct worker_io* w)
{
   struct client_session* client;

   if (w->pipeline_shmem != NULL)
   {
      client = w->pipeline_shmem + (w->slot * sizeof(struct client_session));

      atomic_store(&client->state, CLIENT_INIT);
      client->timestamp = time(NULL);
   }
}

static void
session_destroy(void* pipeline_shmem, size_t pipeline_shmem_size)
{
   if (pipeline_shmem != NULL)
   {
      pgagroal_destroy_shared_memory(pipeline_shmem, pipeline_shmem_size);
   }
}

static void
session_periodic(void* shmem, void* pipeline_shmem)
{
   signed char idle;
   time_t now;
   struct client_session* client;
   struct configuration* config;

   config = (struct configuration*)shmem;

   if (config->disconnect_client > 0 && pipeline_shmem != NULL)
   {
      now = time(NULL);

      for (int i = 0; i < config->max_connections; i++)
      {
         client = pipeline_shmem + (i * sizeof(struct client_session));

         idle = CLIENT_IDLE;

         if (atomic_compare_exchange_strong(&client->state, &idle, CLIENT_CHECK))
         {
            if (difftime(now, client->timestamp) > config->disconnect_client)
            {
               if (config->connections[i].pid != 0)
               {
                  ZF_LOGI("Disconnect client %s/%s using slot %d (%d)",
                          config->connections[i].database, config->connections[i].username,
                          i, config->connections[i].pid);
                  kill(config->connections[i].pid, SIGQUIT);
               }
            }
            else
            {
               atomic_store(&client->state, CLIENT_IDLE);
            }
         }
      }
   }

   exit(0);
}

static void
session_client(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
   int status = MESSAGE_STATUS_ERROR;
   struct worker_io* wi = NULL;
   struct message* msg = NULL;

   wi = (struct worker_io*)watcher;

   client_active(wi->slot, wi->pipeline_shmem);

   if (wi->client_ssl == NULL)
   {
      status = pgagroal_read_socket_message(wi->client_fd, &msg);
   }
   else
   {
      status = pgagroal_read_ssl_message(wi->client_ssl, &msg);
   }
   if (likely(status == MESSAGE_STATUS_OK))
   {
      if (likely(msg->kind != 'X'))
      {
         if (wi->server_ssl == NULL)
         {
            status = pgagroal_write_socket_message(wi->server_fd, msg);
         }
         else
         {
            status = pgagroal_write_ssl_message(wi->server_ssl, msg);
         }
         if (unlikely(status != MESSAGE_STATUS_OK))
         {
            goto server_error;
         }
      }
      else if (msg->kind == 'X')
      {
         exit_code = WORKER_SUCCESS;
         running = 0;
      }
   }
   else
   {
      goto client_error;
   }

   client_inactive(wi->slot, wi->pipeline_shmem);

   ev_break (loop, EVBREAK_ONE);
   return;

client_error:
   ZF_LOGD("client_fd %d - %s (%d)", wi->client_fd, strerror(errno), status);

   client_inactive(wi->slot, wi->pipeline_shmem);

   exit_code = WORKER_CLIENT_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

server_error:
   ZF_LOGD("server_fd %d - %s (%d)", wi->server_fd, strerror(errno), status);

   client_inactive(wi->slot, wi->pipeline_shmem);

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;
}

static void
session_server(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
   int status = MESSAGE_STATUS_ERROR;
   bool fatal = false;
   struct worker_io* wi = NULL;
   struct message* msg = NULL;

   wi = (struct worker_io*)watcher;

   client_active(wi->slot, wi->pipeline_shmem);

   if (wi->server_ssl == NULL)
   {
      status = pgagroal_read_socket_message(wi->server_fd, &msg);
   }
   else
   {
      status = pgagroal_read_ssl_message(wi->server_ssl, &msg);
   }

   if (likely(status == MESSAGE_STATUS_OK))
   {
      if (wi->client_ssl == NULL)
      {
         status = pgagroal_write_socket_message(wi->client_fd, msg);
      }
      else
      {
         status = pgagroal_write_ssl_message(wi->client_ssl, msg);
      }
      if (unlikely(status != MESSAGE_STATUS_OK))
      {
         goto client_error;
      }

      if (unlikely(msg->kind == 'E'))
      {
         fatal = false;

         if (!strncmp(msg->data + 6, "FATAL", 5) || !strncmp(msg->data + 6, "PANIC", 5))
            fatal = true;

         if (fatal)
         {
            exit_code = WORKER_SERVER_FATAL;
            running = 0;
         }
      }
   }
   else
   {
      goto server_error;
   }

   client_inactive(wi->slot, wi->pipeline_shmem);

   ev_break(loop, EVBREAK_ONE);
   return;

client_error:
   ZF_LOGD("client_fd %d - %s (%d)", wi->client_fd, strerror(errno), status);

   client_inactive(wi->slot, wi->pipeline_shmem);

   exit_code = WORKER_CLIENT_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

server_error:
   ZF_LOGD("server_fd %d - %s (%d)", wi->server_fd, strerror(errno), status);

   client_inactive(wi->slot, wi->pipeline_shmem);

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;
}

static void
client_active(int slot , void* pipeline_shmem)
{
   struct client_session* client;

   if (pipeline_shmem != NULL)
   {
      client = pipeline_shmem + (slot * sizeof(struct client_session));
      atomic_store(&client->state, CLIENT_ACTIVE);
      client->timestamp = time(NULL);
   }
}

static void
client_inactive(int slot, void* pipeline_shmem)
{
   struct client_session* client;

   if (pipeline_shmem != NULL)
   {
      client = pipeline_shmem + (slot * sizeof(struct client_session));
      atomic_store(&client->state, CLIENT_IDLE);
      client->timestamp = time(NULL);
   }
}
