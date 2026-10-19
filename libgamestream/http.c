/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "http.h"
#include "errors.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <curl/curl.h>

static CURL *curl;
// There is one curl handle, and the box art loader uses it from its own thread
// while the menu may make requests too, so one request runs at a time
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;

static bool debug;

static size_t _write_curl(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  PHTTP_DATA mem = (PHTTP_DATA)userp;

  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL)
    return 0;

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

// Connecting to a PC on the LAN takes milliseconds, including the TLS
// handshake. A PC that is off or asleep never answers, so give up quickly
// rather than block the menu (waking PCs are polled separately).
#define HTTP_CONNECT_TIMEOUT 3

static char key_directory[4096];

static CURL *create_handle(const char* keyDirectory) {
  CURL *handle = curl_easy_init();
  if (!handle)
    return NULL;

  char certificateFilePath[4096];
  sprintf(certificateFilePath, "%s/%s", keyDirectory, CERTIFICATE_FILE_NAME);

  char keyFilePath[4096];
  sprintf(&keyFilePath[0], "%s/%s", keyDirectory, KEY_FILE_NAME);

  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(handle, CURLOPT_SSLENGINE_DEFAULT, 1L);
  curl_easy_setopt(handle, CURLOPT_SSLCERTTYPE,"PEM");
  curl_easy_setopt(handle, CURLOPT_SSLCERT, certificateFilePath);
  curl_easy_setopt(handle, CURLOPT_SSLKEYTYPE, "PEM");
  curl_easy_setopt(handle, CURLOPT_SSLKEY, keyFilePath);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, _write_curl);
  curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, (long) HTTP_CONNECT_TIMEOUT);
  // Connections and TLS sessions are kept for the next request: opening a PC
  // makes several, and each new TLS handshake is slow on the Vita. They are
  // dropped with the handle when the client certificate or pairing changes.

  return handle;
}

int http_init(const char* keyDirectory, int logLevel) {
  pthread_mutex_lock(&curl_lock);
  debug = logLevel >= 2;
  // gs_init runs on every connect: keep the handle, and with it the open
  // connections, unless this is another PC's certificate
  if (!curl || strcmp(key_directory, keyDirectory) != 0) {
    if (curl)
      curl_easy_cleanup(curl);
    snprintf(key_directory, sizeof(key_directory), "%s", keyDirectory);
    curl = create_handle(keyDirectory);
  }
  int ret = curl ? GS_OK : GS_FAILED;
  pthread_mutex_unlock(&curl_lock);
  return ret;
}

void http_reset_connections() {
  pthread_mutex_lock(&curl_lock);
  if (curl) {
    curl_easy_cleanup(curl);
    curl = create_handle(key_directory);
  }
  pthread_mutex_unlock(&curl_lock);
}

int http_request(char* url, PHTTP_DATA data) {
  return http_request_timeout(url, data, HTTP_REQUEST_TIMEOUT);
}

int http_request_timeout(char* url, PHTTP_DATA data, long timeout_seconds) {
  pthread_mutex_lock(&curl_lock);
  int ret = GS_OK;
  if (!curl) {
    gs_error = "HTTP client not initialised";
    ret = GS_FAILED;
    goto done;
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

  if (debug)
    printf("Request %s\n", url);

  if (data->size > 0) {
    free(data->memory);
    data->memory = malloc(1);
    if(data->memory == NULL) {
      ret = GS_OUT_OF_MEMORY;
      goto done;
    }

    data->size = 0;
  }
  CURLcode res = curl_easy_perform(curl);

  if(res != CURLE_OK) {
    gs_error = curl_easy_strerror(res);
    ret = GS_FAILED;
  } else if (data->memory == NULL) {
    ret = GS_OUT_OF_MEMORY;
  } else if (debug) {
    printf("Response:\n%s\n\n", data->memory);
  }

done:
  pthread_mutex_unlock(&curl_lock);
  return ret;
}

void http_cleanup() {
  pthread_mutex_lock(&curl_lock);
  curl_easy_cleanup(curl);
  curl = NULL;
  key_directory[0] = '\0';
  pthread_mutex_unlock(&curl_lock);
}

PHTTP_DATA http_create_data() {
  PHTTP_DATA data = malloc(sizeof(HTTP_DATA));
  if (data == NULL)
    return NULL;

  data->memory = malloc(1);
  if(data->memory == NULL) {
    free(data);
    return NULL;
  }
  data->size = 0;

  return data;
}

void http_free_data(PHTTP_DATA data) {
  if (data != NULL) {
    if (data->memory != NULL)
      free(data->memory);

    free(data);
  }
}
