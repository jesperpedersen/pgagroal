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
#include <logging.h>
#include <management.h>
#include <memory.h>
#include <message.h>
#include <network.h>
#include <pipeline.h>
#include <pool.h>
#include <prometheus.h>
#include <security.h>
#include <worker.h>
#include <utils.h>

#define ZF_LOG_TAG "worker"
#include <zf_log.h>

/* system */
#include <ev.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <openssl/ssl.h>

volatile int running = 1;
volatile int exit_code = WORKER_FAILURE;

static void signal_cb(struct ev_loop *loop, ev_signal *w, int revents);

void
pgagroal_worker(int client_fd, char* address, void* shmem, void* pipeline_shmem)
{
   struct ev_loop *loop = NULL;
   struct signal_info signal_watcher;
   struct worker_io client_io;
   struct worker_io server_io;
   time_t start_time;
   bool started = false;
   int auth_status;
   struct configuration* config;
   struct pipeline p;
   int32_t slot = -1;
   SSL* client_ssl = NULL;
   SSL* server_ssl = NULL;

   pgagroal_start_logging(shmem);
   pgagroal_memory_init(shmem);

   config = (struct configuration*)shmem;

   memset(&client_io, 0, sizeof(struct worker_io));
   memset(&server_io, 0, sizeof(struct worker_io));

   start_time = time(NULL);

   /* Authentication */
   auth_status = pgagroal_authenticate(client_fd, address, shmem, &slot, &client_ssl, &server_ssl);
   if (auth_status == AUTH_SUCCESS)
   {
      ZF_LOGD("pgagroal_worker: Slot %d (%d -> %d)", slot, client_fd, config->connections[slot].fd);

      if (config->log_connections)
      {
         ZF_LOGI("connect: user=%s database=%s address=%s", config->connections[slot].username,
                 config->connections[slot].database, address);
      }

      pgagroal_pool_status(shmem);

      if (config->nodelay)
      {
         if (pgagroal_tcp_nodelay(client_fd, shmem))
         {
            ZF_LOGW("pgagroal_worker: TCP_NODELAY failed for %d", client_fd);
         }
      }
      
      if (config->non_blocking)
      {
         if (pgagroal_socket_nonblocking(client_fd, true))
         {
            ZF_LOGW("pgagroal_worker: O_NONBLOCK failed for %d", client_fd);
         }
      }
      
      if (pgagroal_socket_buffers(client_fd, shmem))
      {
         ZF_LOGW("pgagroal_worker: SO_RCVBUF/SO_SNDBUF failed for %d", client_fd);
      }
      
      if (config->pipeline == PIPELINE_PERFORMANCE)
      {
         p = performance_pipeline();
      }
      else if (config->pipeline == PIPELINE_SESSION)
      {
         p = session_pipeline();
      }
      else
      {
         ZF_LOGE("pgagroal_worker: Unknown pipeline %d", config->pipeline);
         p = session_pipeline();
      }

      ev_io_init((struct ev_io*)&client_io, p.client, client_fd, EV_READ);
      client_io.client_fd = client_fd;
      client_io.server_fd = config->connections[slot].fd;
      client_io.slot = slot;
      client_io.client_ssl = client_ssl;
      client_io.server_ssl = server_ssl;
      client_io.shmem = shmem;
      client_io.pipeline_shmem = pipeline_shmem;
      
      ev_io_init((struct ev_io*)&server_io, p.server, config->connections[slot].fd, EV_READ);
      server_io.client_fd = client_fd;
      server_io.server_fd = config->connections[slot].fd;
      server_io.slot = slot;
      server_io.client_ssl = client_ssl;
      server_io.server_ssl = server_ssl;
      server_io.shmem = shmem;
      server_io.pipeline_shmem = pipeline_shmem;
      
      loop = ev_loop_new(pgagroal_libev(config->libev));

      ev_signal_init((struct ev_signal*)&signal_watcher, signal_cb, SIGQUIT);
      signal_watcher.shmem = shmem;
      signal_watcher.slot = slot;
      ev_signal_start(loop, (struct ev_signal*)&signal_watcher);

      p.start(&client_io);
      started = true;

      ev_io_start(loop, (struct ev_io*)&client_io);
      ev_io_start(loop, (struct ev_io*)&server_io);

      while (running)
      {
         ev_loop(loop, 0);
      }
   }
   else
   {
      if (config->log_connections)
      {
         ZF_LOGI("connect: address=%s", address);
      }
   }

   if (config->log_disconnections)
   {
      if (auth_status == AUTH_SUCCESS)
      {
         ZF_LOGI("disconnect: user=%s database=%s address=%s", config->connections[slot].username,
                 config->connections[slot].database, address);
      }
      else
      {
         ZF_LOGI("disconnect: address=%s", address);
      }
   }

   /* Return to pool */
   if (slot != -1)
   {
      if (started)
      {
         p.stop(&client_io);

         pgagroal_prometheus_session_time(difftime(time(NULL), start_time), shmem);
      }

      if ((auth_status == AUTH_SUCCESS || auth_status == AUTH_BAD_PASSWORD) &&
          (exit_code == WORKER_SUCCESS || exit_code == WORKER_CLIENT_FAILURE ||
           (exit_code == WORKER_FAILURE && config->connections[slot].has_security != SECURITY_INVALID)))
      {
         pgagroal_return_connection(shmem, slot, server_ssl);
      }
      else if (exit_code == WORKER_SERVER_FAILURE || exit_code == WORKER_SERVER_FATAL || exit_code == WORKER_SHUTDOWN ||
               (exit_code == WORKER_FAILURE && config->connections[slot].has_security == SECURITY_INVALID))
      {
         pgagroal_kill_connection(shmem, slot);
      }
      else
      {
         if (pgagroal_socket_isvalid(config->connections[slot].fd) &&
             pgagroal_connection_isvalid(config->connections[slot].fd) &&
             config->connections[slot].has_security != SECURITY_INVALID)
         {
            pgagroal_return_connection(shmem, slot, server_ssl);
         }
         else
         {
            pgagroal_kill_connection(shmem, slot, server_ssl);
         }
      }
   }

   if (client_ssl != NULL)
   {
      int res;
      SSL_CTX* ctx = SSL_get_SSL_CTX(client_ssl);
      res = SSL_shutdown(client_ssl);
      if (res == 0)
      {
         SSL_shutdown(client_ssl);
      }
      SSL_free(client_ssl);
      SSL_CTX_free(ctx);
   }

   ZF_LOGD("client disconnect: %d", client_fd);
   pgagroal_disconnect(client_fd);

   pgagroal_pool_status(shmem);
   ZF_LOGD("After client: PID %d Slot %d (%d)", getpid(), slot, exit_code);

   if (loop)
   {
      ev_io_stop(loop, (struct ev_io*)&client_io);
      ev_io_stop(loop, (struct ev_io*)&server_io);

      ev_signal_stop(loop, (struct ev_signal*)&signal_watcher);

      ev_loop_destroy(loop);
   }

   free(address);

   pgagroal_memory_destroy();
   pgagroal_stop_logging(shmem);

   exit(exit_code);
}

static void
signal_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
   struct signal_info* si;

   si = (struct signal_info*)w;

   ZF_LOGD("pgagroal: signal for slot %d", si->slot);

   exit_code = WORKER_SHUTDOWN;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
}
