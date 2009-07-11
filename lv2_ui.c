/* -*- Mode: C ; c-basic-offset: 2 -*- */
/*****************************************************************************
 *
 * Copyright (C) 2009 Nedko Arnaudov <nedko@arnaudov.name>
 *
 * LV2 UI bundle shared library for communicating with a DSSI UI
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307, USA.
 *
 *****************************************************************************/

#define UI_EXECUTABLE "ui"
#define UI_URI        "http://nedko.aranaudov.org/soft/filter/2/gui"

//#define FORK_TIME_MEASURE

#define USE_VFORK
//#define USE_CLONE
//#define USE_CLONE2

#if defined(USE_VFORK)
#define FORK vfork
#define FORK_STR "vfork"
#elif defined(USE_CLONE)
#define FORK_STR "clone"
#elif defined(USE_CLONE2)
#define FORK_STR "clone2"
#else
#define FORK fork
#define FORK_STR "fork"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#if defined(FORK_TIME_MEASURE)
# include <sys/time.h>
#endif
#include <unistd.h>
#if defined(USE_CLONE) || defined(USE_CLONE2)
# include <sched.h>
#endif
#include <fcntl.h>
#include <locale.h>
#include <errno.h>

#include <lv2.h>
#include "lv2_ui.h"
#include "lv2_external_ui.h"

struct control
{
  struct lv2_external_ui virt;  /* WARNING: code assumes this is the first struct member */

  LV2UI_Controller controller;
  LV2UI_Write_Function write_function;
  void (* ui_closed)(LV2UI_Controller controller);

  bool running;              /* true if UI launched and 'exiting' not received */
  bool visible;              /* true if 'show' sent */

  int send_pipe;             /* the pipe end that is used for sending messages to UI */
  int recv_pipe;             /* the pipe end that is used for receiving messages from UI */
};

static
char *
read_line(
  struct control * control_ptr)
{
  ssize_t ret;
  char ch;
  char buf[100];
  char * ptr;

  ptr = buf;

loop:
  ret = read(control_ptr->recv_pipe, &ch, 1);
  if (ret == 1 && ch != '\n')
  {
    *ptr++ = ch;
    goto loop;
  }

  if (ptr != buf)
  {
    *ptr = 0;
    //printf("recv: \"%s\"\n", buf);
    return strdup(buf);
  }

  return NULL;
}

#define control_ptr ((struct control *)_this_)

static
void
run(
  struct lv2_external_ui * _this_)
{
  char * msg;
  char * port_index_str;
  char * port_value_str;
  int port;
  float value;
  char * locale;

  //printf("run() called\n");

  msg = read_line(control_ptr);
  if (msg == NULL)
  {
    return;
  }

  locale = strdup(setlocale(LC_NUMERIC, NULL));
  setlocale(LC_NUMERIC, "POSIX");

  if (!strcmp(msg, "port_value"))
  {
    port_index_str = read_line(control_ptr);
    port_value_str = read_line(control_ptr);

    port = atoi(port_index_str);
    if (sscanf(port_value_str, "%f", &value) == 1)
    {
      //printf("port %d = %f\n", port, value);
      control_ptr->write_function(control_ptr->controller, (uint32_t)port, sizeof(float), 0, &value);
    }
    else
    {
      fprintf(stderr, "failed to convert \"%s\" to float\n", port_value_str);
    }

    free(port_index_str);
    free(port_value_str);
  }
  else if (!strcmp(msg, "exiting"))
  {
    //printf("got UI exit notification\n");
    control_ptr->running = false;
    control_ptr->visible = false;
    control_ptr->ui_closed(control_ptr->controller);
  }
  else
  {
    printf("unknown message: \"%s\"\n", msg);
  }

  setlocale(LC_NUMERIC, locale);
  free(locale);

  free(msg);
}

static
void
show(
  struct lv2_external_ui * _this_)
{
  //printf("show() called\n");

  if (control_ptr->visible)
  {
    return;
  }

  write(control_ptr->send_pipe, "show\n", 5);
  control_ptr->visible = true;
}

static
void
hide(
  struct lv2_external_ui * _this_)
{
  //printf("hide() called\n");

  if (!control_ptr->visible)
  {
    return;
  }

  write(control_ptr->send_pipe, "hide\n", 5);
  control_ptr->visible = false;
}

#undef control_ptr

#if defined(FORK_TIME_MEASURE)
static
uint64_t
get_current_time()
{
   struct timeval time;

   if (gettimeofday(&time, NULL) != 0)
       return 0;

   return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_usec;
}

#define FORK_TIME_MEASURE_VAR_NAME  ____t

#define FORK_TIME_MEASURE_VAR       uint64_t FORK_TIME_MEASURE_VAR_NAME
#define FORK_TIME_MEASURE_BEGIN     FORK_TIME_MEASURE_VAR_NAME = get_current_time()
#define FORK_TIME_MEASURE_END(msg)                                                       \
  {                                                                                      \
    FORK_TIME_MEASURE_VAR_NAME = get_current_time() - FORK_TIME_MEASURE_VAR_NAME;        \
    fprintf(stderr, msg ": %llu us\n", (unsigned long long)FORK_TIME_MEASURE_VAR_NAME);  \
  }

#else

#define FORK_TIME_MEASURE_VAR
#define FORK_TIME_MEASURE_BEGIN
#define FORK_TIME_MEASURE_END(msg)

#endif

#if defined(USE_CLONE) || defined(USE_CLONE2)

static int clone_fn(void * context)
{
    execvp(*(const char **)context, (char **)context);
    return -1;
}

#endif

static
LV2UI_Handle
instantiate(
  const struct _LV2UI_Descriptor * descriptor,
  const char * plugin_uri,
  const char * bundle_path,
  LV2UI_Write_Function write_function,
  LV2UI_Controller controller,
  LV2UI_Widget * widget,
  const LV2_Feature * const * features)
{
  struct control * control_ptr;
  struct lv2_external_ui_host * ui_host_ptr;
  char * filename;
  int pipe1[2]; /* written by host process, read by plugin UI process */
  int pipe2[2]; /* written by plugin UI process, read by host process */
  char ui_recv_pipe[100];
  char ui_send_pipe[100];
  int oldflags;
  FORK_TIME_MEASURE_VAR;
  const char * argv[8];
  int ret;

  //printf("instantiate('%s', '%s') called\n", plugin_uri, bundle_path);

  ui_host_ptr = NULL;
  while (*features != NULL)
  {
    if (strcmp((*features)->URI, LV2_EXTERNAL_UI_URI) == 0)
    {
      ui_host_ptr = (*features)->data;
    }

    features++;
  }

  if (ui_host_ptr == NULL)
  {
    goto fail;
  }

  control_ptr = malloc(sizeof(struct control));
  if (control_ptr == NULL)
  {
    goto fail;
  }

  control_ptr->virt.run = run;
  control_ptr->virt.show = show;
  control_ptr->virt.hide = hide;

  control_ptr->controller = controller;
  control_ptr->write_function = write_function;
  control_ptr->ui_closed = ui_host_ptr->ui_closed;

  if (pipe(pipe1) != 0)
  {
    fprintf(stderr, "pipe1 creation failed.");
  }

  if (pipe(pipe2) != 0)
  {
    fprintf(stderr, "pipe2 creation failed.");
  }

  snprintf(ui_recv_pipe, sizeof(ui_recv_pipe), "%d", pipe1[0]); /* [0] means reading end */
  snprintf(ui_send_pipe, sizeof(ui_send_pipe), "%d", pipe2[1]); /* [1] means writting end */

  filename = malloc(strlen(bundle_path) + strlen(UI_EXECUTABLE) + 1);
  if (filename == NULL)
  {
    goto fail_free_control;
  }

  strcpy(filename, bundle_path);
  strcat(filename, UI_EXECUTABLE);

  control_ptr->running = false;
  control_ptr->visible = false;

  argv[0] = "python";
  argv[1] = filename;
  argv[2] = plugin_uri;
  argv[3] = bundle_path;
  argv[4] = ui_host_ptr->plugin_human_id != NULL ? ui_host_ptr->plugin_human_id : "";
  argv[5] = ui_recv_pipe;       /* reading end */
  argv[6] = ui_send_pipe;       /* writting end */
  argv[7] = NULL;

  FORK_TIME_MEASURE_BEGIN;

#if defined(USE_CLONE)
  {
    int stack[8000];

    ret = clone(clone_fn, stack + 4000, CLONE_VFORK, argv);
    if (ret == -1)
    {
      fprintf(stderr, "clone() failed: %s\n", strerror(errno));
      goto fail_free_control;
    }
  }
#elif defined(USE_CLONE2)
  fprintf(stderr, "clone2() exec not implemented yet\n");
  goto fail_free_control;
#else
  ret = FORK();
  switch (ret)
  {
  case 0:                       /* child process */
    /* fork duplicated the handles, close pipe ends that are used by parent process */
#if !defined(USE_VFORK)
    /* it looks we cant do this for vfork() */
    close(pipe1[1]);
    close(pipe2[0]);
#endif

    execvp(argv[0], (char **)argv);
    fprintf(stderr, "exec of UI failed: %s", strerror(errno));
    exit(1);
  case -1:
    fprintf(stderr, "fork() failed to create new process for plugin UI");
    goto fail_free_control;
  }

#endif

  FORK_TIME_MEASURE_END(FORK_STR "() time");

  /* fork duplicated the handles, close pipe ends that are used by the child process */
  close(pipe1[0]);
  close(pipe2[1]);

  control_ptr->send_pipe = pipe1[1]; /* [1] means writting end */
  control_ptr->recv_pipe = pipe2[0]; /* [0] means reading end */

  oldflags = fcntl(control_ptr->recv_pipe, F_GETFL);
  fcntl(control_ptr->recv_pipe, F_SETFL, oldflags | O_NONBLOCK);

  *widget = (LV2UI_Widget)control_ptr;

  return (LV2UI_Handle)control_ptr;

fail_free_control:
  free(control_ptr);

fail:
  return NULL;
}

#define control_ptr ((struct control *)ui)

static
void
cleanup(
  LV2UI_Handle ui)
{
  //printf("cleanup() called\n");
  free(control_ptr);
}

static
void
port_event(
  LV2UI_Handle ui,
  uint32_t port_index,
  uint32_t buffer_size,
  uint32_t format,
  const void * buffer)
{
  char buf[100];
  int len;
  char * locale;

  //printf("port_event(%u, %f) called\n", (unsigned int)port_index, *(float *)buffer);

  locale = strdup(setlocale(LC_NUMERIC, NULL));
  setlocale(LC_NUMERIC, "POSIX");

  write(control_ptr->send_pipe, "port_value\n", 11);
  len = sprintf(buf, "%u\n", (unsigned int)port_index);
  write(control_ptr->send_pipe, buf, len);
  len = sprintf(buf, "%.10f\n", *(float *)buffer);
  write(control_ptr->send_pipe, buf, len);
  fsync(control_ptr->send_pipe);

  setlocale(LC_NUMERIC, locale);
  free(locale);
}

#undef control_ptr

static LV2UI_Descriptor descriptors[] =
{
  {UI_URI, instantiate, cleanup, port_event, NULL}
};

const LV2UI_Descriptor *
lv2ui_descriptor(
  uint32_t index)
{
  //printf("lv2ui_descriptor(%u) called\n", (unsigned int)index);

  if (index >= sizeof(descriptors) / sizeof(descriptors[0]))
  {
    return NULL;
  }

  return descriptors + index;
}
