/*
 * interhapd - Inter-connect hostapd Daemon
 *
 * Copyright (C) 2021      Eric Woudstra
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

// apt install libsystemd-dev

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdarg.h>
#include <dirent.h> 
#include <errno.h>
#include <net/if.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <arpa/inet.h>

#define USE_SYSTEMD 1 
#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define EVENT_BUF_LEN  32678
#define STRINGSIZE      1024

#define NDA_RTA(r) \
	((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))

char **                if_strings;
int                    if_strings_cnt;
int                    port = 11111;
int                    debuglevel = 1;
bool                   legacyfdb = false;
const char             *localsocketstr="/tmp/interhapd-%d";
char                   *runhostapdpath = "/run/hostapd";
char                   *lanbridge[1] ={"brlan"};
char                   *script ="./interhapd.py";
const char             *ihapd_intf_started =         "INTERHAPD INTERFACE STARTED";
const char             *ihapd_intf_stopped =         "INTERHAPD INTERFACE STOPPED";
const char             *ihapd_list_started =         "INTERHAPD LISTENING STARTED";
const char             *ihapd_list_stopped =         "INTERHAPD LISTENING STOPPED";
const char             *ihapd_list_started_noreply = "INTERHAPD LISTENING STARTED NOREPLY";
const char             *ihapd_list_interfaces =      "INTERHAPD LIST INTERFACES";
int                    sendtofd=-1, epollfd = -1, netlinkfd=-1, inotfd=-1;
int                    inotrunwd=-1, inotrunhostapdwd=-1;
int                    notifysystemd=0;
char                   hostname[HOST_NAME_MAX + 1];
char                   runpath[STRINGSIZE];
char                   buffer[EVENT_BUF_LEN];
struct iovec           iovbuffer = { buffer, sizeof(buffer) };
struct sockaddr_nl     sanlevent = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_IPV4_IFADDR};
struct sockaddr_nl     sanldump =  { .nl_family = AF_NETLINK };
bool                   should_exit = false;
bool                   ismainthread = true;

#define NODE_HOSTAPD          1
#define NODE_LOCALIP          2
#define NODE_REQUEST_DUMP     3
#define NODE_REMOTEIP         4
#define NODE_SCRIPT           5

#define LEVEL_EXIT       -32000

#if (ETH_ALEN*3) >  IF_NAMESIZE
#define NAMELEN      ETH_ALEN*3
#else
#define NAMELEN     IF_NAMESIZE
#endif           
typedef struct addr_node {
    struct    addr_node * next;
    int                   type;
    in_addr_t             addr;
      in_addr_t           bcaddr;
    int                   sockfd;
    int                   synchrfd;
    char                  name[NAMELEN];
    union {
      char                mac[ETH_ALEN];
      struct {
        pid_t             pidsh;
        FILE              *stdinprocess;
      };
    };
} addr_node_t;
addr_node_t * threadlist;

typedef struct myline {
    char * fromhost;
    char * fromsock;
    char * tohost;
    char * tosock;
    char * mytype;
    char * remain;
    char line[STRINGSIZE];
} myline_t;

int debugprintf(int level, const char *fmt, ...)
{
  if (level > debuglevel) return 0;
  va_list args;
  int printed=0;
  va_start(args, fmt);
  printed = vprintf(fmt, args);
  va_end(args);
  fflush(stdout);
  if (level != LEVEL_EXIT) return printed;
  else exit(EXIT_FAILURE);
}

void linebyline(char *buf, int bytes, void (*func)())  
{  
  char  *line = buf, *strfound;
  while (bytes > 0) {
    if ( ( strfound = strstr( line, "\n") ) != 0) {
      strfound[0] = 0;
      func(line);
      bytes -= strfound + 1 - line;
      line =   strfound + 1;
    }
    else {
      func(line);
      break;
    }
  }
}  

void broadcast(const char *text, addr_node_t * a) {
  if (a == NULL) return;
  struct sockaddr_in sa = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr.s_addr = a->bcaddr,
  };
  if (sendto(sendtofd, text, strlen(text), 0, (struct sockaddr*)&sa, sizeof(sa)) != -1) { 
    debugprintf(2, "Data broadcast to %s:%d string %s\n", inet_ntoa(sa.sin_addr), port, text);
  }
}

void send2ip(const char *text, addr_node_t * a) {
  if (a == NULL) return;
  struct sockaddr_in sa = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = a->addr,
  };
  if (sendto(sendtofd, text, strlen(text), 0, (struct sockaddr*)&sa, sizeof(sa)) != -1) { 
    debugprintf(2, "Data send to %s:%d string %s\n", inet_ntoa(sa.sin_addr), port, text);
  }
}

addr_node_t * node_from_name(char *name) {
  addr_node_t * current = threadlist;
  while (current != NULL) {
    if (strcmp(current->name, name) ==0) return current; 
    current = current->next;
  }
  return NULL; 
}

addr_node_t * node_from_addr(in_addr_t addr) {
  addr_node_t * current = threadlist;
  while (current != NULL) {
    if (current->addr==addr) return current; 
    current = current->next;
  }
  return NULL; 
}

void send2localscriptraw(char *line, addr_node_t * a) {
}

void send2script(addr_node_t * a, const char *fromhost, const char *fromsock, 
                                  const char *tohost, const char *tosock, 
                                  const char *mytype, const char *remain) {
  char s[STRINGSIZE*2], r[STRINGSIZE];
  strncpy(r, remain, STRINGSIZE-1); r[STRINGSIZE-1] = 0;
  for(int i=0;r[i]!='\0';i++) if(r[i]=='\n') r[i] = '^';
  snprintf(s, STRINGSIZE*2 -3,  "FROM=%s-%s TO=%s-%s %s=%s", 
        fromhost, fromsock, tohost, tosock, mytype, r);      // make sure there are 3 bytes left
  if ((a == NULL) || (strcmp(tohost, "broadcast") == 0)) {
    for (addr_node_t *aa = threadlist; aa; aa = aa->next)
      if (aa->type == NODE_LOCALIP) broadcast(s, aa);
  }
  else if (strcmp(tohost, hostname) == 0) {
    if (a->stdinprocess == NULL) return;
    int len = strlen(s);
    s[len]='\n';                                          // Needs 1 extra byte
    fwrite(s, sizeof(char), len+1, a->stdinprocess);
    fflush(a->stdinprocess);     // ?????????????
  }
  else send2ip(s, a);
}

addr_node_t * node_add(int type, char *name, in_addr_t addr, in_addr_t bcaddr) {
  addr_node_t * a;
  struct in_addr in_ad={0};
  struct sockaddr_un sa_local = {0}, sa_dest = {0};
  char *mac;

  in_ad.s_addr = addr;
  if ((a = node_from_name(name)) != NULL) return a;
  if ((a = (addr_node_t *) malloc(sizeof(addr_node_t))) == NULL) return NULL;
  a->next = threadlist;
  threadlist = a;
  a->type=type;
  a->addr=addr;
  a->bcaddr=bcaddr;
  a->sockfd=-1;
  a->synchrfd=-1;
  strncpy(a->name, name, NAMELEN-1); a->name[NAMELEN-1] = 0;
  switch (type) {
    case NODE_HOSTAPD:
      // Start listening on hostapd
      if ((a->sockfd = socket(PF_UNIX, SOCK_DGRAM, 0)) ==-1) break;
      sa_local.sun_family = AF_UNIX;
      snprintf(sa_local.sun_path, sizeof(sa_local.sun_path),  localsocketstr, a->sockfd);
      unlink(sa_local.sun_path); // Just in case we did not unlink it when exiting
      if (bind(a->sockfd, (struct sockaddr *) &sa_local, sizeof(sa_local)) ==-1) break;
      sa_dest.sun_family = AF_UNIX;
      snprintf(sa_dest.sun_path, sizeof(sa_dest.sun_path),  "%s/%s", runhostapdpath, a->name);
      int i; 
      for(i=0; i<50; i++) {
        if (connect(a->sockfd, (struct sockaddr *) &sa_dest, sizeof(sa_dest)) != -1) break;
        usleep(10000);
      } // wait for a maximum of 500 milliseconds
      if (i == 50) break;
      int flags = fcntl(a->sockfd, F_GETFL);
      fcntl(a->sockfd, F_SETFL, flags | O_NONBLOCK);
      send(a->sockfd, "DETACH", 6, 0); // // Just in case we did not send DETACH when exiting
      send(a->sockfd, "ATTACH", 6, 0);
      // Second socket
      memset(&sa_local, 0, sizeof(sa_local));
      memset(&sa_dest, 0, sizeof(sa_dest));
      if ((a->synchrfd = socket(PF_UNIX, SOCK_DGRAM, 0)) ==-1) break;
      sa_local.sun_family = AF_UNIX;
      snprintf(sa_local.sun_path, sizeof(sa_local.sun_path),  localsocketstr, a->synchrfd);
      unlink(sa_local.sun_path); // Just in case we did not unlink it when exiting
      if (bind(a->synchrfd, (struct sockaddr *) &sa_local, sizeof(sa_local)) ==-1) break;
      sa_dest.sun_family = AF_UNIX;
      snprintf(sa_dest.sun_path, sizeof(sa_dest.sun_path),  "%s/%s", runhostapdpath, a->name);
      for(i=0; i<50; i++) {
        if (connect(a->synchrfd, (struct sockaddr *) &sa_dest, sizeof(sa_dest)) != -1) break;
        usleep(10000);
      } // wait for a maximum of 500 milliseconds
      if (i == 50) break;
      addr_node_t * toa = node_from_name(hostname);
      debugprintf(1, "Started listening on %s\n", a->name);
      if (toa != NULL)
        send2script(toa, hostname, a->name, hostname, "event", "EVENT", ihapd_intf_started);
      break;
    case NODE_LOCALIP:
      // Start listening on ip
      if ((a->sockfd = socket(PF_INET, SOCK_DGRAM, 0)) ==-1) break;
      struct sockaddr_in si_me = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
      };
      int one = 1; setsockopt(a->sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); // DO WE NEED THIS?
      one = setsockopt(a->sockfd, SOL_SOCKET, SO_BINDTODEVICE, a->name, strlen(a->name));
      if (bind(a->sockfd, (struct sockaddr*)&si_me, sizeof(si_me)) ==-1) break;
      debugprintf(1, "Started listening on %s:%d\n", inet_ntoa(in_ad), port);
      send2script(NULL, hostname, "event", "broadcast", "event", "EVENT", ihapd_list_started);
      break;
    case NODE_REQUEST_DUMP:
      if ((mac = strstr(name, "CONNECTED ")) == NULL) mac = name; else mac += strlen("CONNECTED ");
      if (strlen(mac)<(ETH_ALEN*3-1)) break;
      strncpy(a->name, mac, ETH_ALEN*3-1); a->name[ETH_ALEN*3-1] = 0;
      if (sscanf(a->name, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", a->mac, a->mac+1, 
                                    a->mac+2, a->mac+3, a->mac+4, a->mac+5) != 6) break;
      if ((a->sockfd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE)) ==-1) break;
      if (bind(a->sockfd, (struct sockaddr *) &sanldump, sizeof(sanldump)) ==-1) break;
      struct ndmsg *ndm;
      struct nlmsghdr *nlh = (struct nlmsghdr *)calloc(1, NLMSG_SPACE(sizeof(*ndm)));
      nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ndm));
      nlh->nlmsg_pid = getpid();
      nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; // 
      nlh->nlmsg_type = RTM_GETNEIGH;
      ndm = (struct ndmsg *)NLMSG_DATA(nlh);
      ndm->ndm_family = PF_BRIDGE;
      ndm->ndm_ifindex = 0;
      struct iovec iov = { .iov_base=nlh, .iov_len=nlh->nlmsg_len };
      struct msghdr msg = {
        .msg_name = (void *)&sanldump,
        .msg_namelen = sizeof(sanldump),
        .msg_iov = &iov,
        .msg_iovlen = 1,
      };
      sendmsg(a->sockfd,&msg,0);
      free(nlh);
      debugprintf(2, "Send dump request, delete %s\n", a->name);
      break;
    case NODE_REMOTEIP:
      {} struct sockaddr_in sa = { .sin_addr.s_addr = a->addr };
      debugprintf(1, "Added remote IP with hostname: %s and ip: %s\n", a->name, inet_ntoa(sa.sin_addr));
      break;
    case NODE_SCRIPT:
      if( access( script, F_OK ) != -1 ) {
        int pipe_in_fd[2], pipe_out_fd[2];
        pipe(pipe_in_fd);
        pipe(pipe_out_fd);
        a->pidsh = fork();
        if (a->pidsh == 0) { // Child process
          ismainthread = false;
          close(pipe_in_fd[1]);
          close(pipe_out_fd[0]);
          dup2(pipe_in_fd[0], STDIN_FILENO);
          dup2(pipe_out_fd[1], STDOUT_FILENO);
          execl(script, script, (char*) NULL);
          debugprintf(LEVEL_EXIT, "Error starting child process: %s\n", strerror(errno));
        }
        close(pipe_in_fd[0]);
        close(pipe_out_fd[1]);
        a->synchrfd = pipe_in_fd[1];
        a->sockfd = pipe_out_fd[0];
        a->stdinprocess = fdopen(a->synchrfd, "w");
      }
      break;
  }
  if (a->sockfd != -1) {
    struct epoll_event ev={
      .events = EPOLLIN|EPOLLET,
      .data.fd = a->sockfd,
    };
    epoll_ctl(epollfd, EPOLL_CTL_ADD, a->sockfd, &ev);
  }
  return a;
}

void node_remove(addr_node_t *a) {
  if (a==NULL) return;
  struct in_addr in_ad = {0};
  in_ad.s_addr = a->addr;
  addr_node_t * current = threadlist, * temp_node = NULL;
  if (current == NULL) return;

  switch (a->type) {
    case NODE_HOSTAPD:
      // Stop listening on hostapd
      if (a->synchrfd != -1) {
        char localfile[32];
        snprintf(localfile, sizeof(localfile),  localsocketstr, a->synchrfd);
        unlink(localfile); // Not done if CTRL-C is pressed
      }
      if (a->sockfd != -1) {
        char localfile[32];
        send(a->sockfd, "DETACH", 6, 0);
        snprintf(localfile, sizeof(localfile),  localsocketstr, a->sockfd);
        unlink(localfile); // Not done if CTRL-C is pressed
      }
      addr_node_t * toa = node_from_name(hostname);
      if (toa != NULL)
        send2script(toa, hostname, a->name, hostname, "event", "EVENT", ihapd_intf_stopped);
      debugprintf(1, "Stopped listening on %s\n", a->name);
      break;
    case NODE_LOCALIP:
    // Stop listening on ip
      send2script(NULL, hostname, "event", "broadcast", "event", "EVENT", ihapd_list_stopped);
      debugprintf(1, "Stopped listening on %s:%d\n", inet_ntoa(in_ad), port);
      break;
    case NODE_REQUEST_DUMP:
      break;
    case NODE_REMOTEIP:
      debugprintf(1, "Removed remote IP with hostname: %s\n", a->name);
      break;
    case NODE_SCRIPT:
      send2script(a, hostname, "exit", hostname, "exit", "COMMAND", "EXIT!");
      for(int i=0; i<50; i++) {
        if (waitpid(a->pidsh, NULL, WNOHANG) > 0) break;
        usleep(100000);
      } // wait for a maximum of 5 seconds
      if (a->stdinprocess != NULL) fclose(a->stdinprocess);
      break;
  }
  if (a->sockfd != -1) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, a->sockfd, NULL);
    close(a->sockfd);
  }
  if (a->synchrfd != -1) close(a->synchrfd);
  if (current == a) {
    temp_node = (threadlist)->next;
    free(threadlist);
    threadlist = temp_node;
  } else {
    while (current->next != a) {
      if (current->next == NULL) return;
      current = current->next;
    }
    temp_node = current->next;
    current->next = current->next->next;
    free(temp_node);
  }
  return;
}

void exitfunc() {
  if (!ismainthread) return;
  while (threadlist) node_remove(threadlist);
  if (inotrunwd != -1) inotify_rm_watch( inotfd, inotrunwd );
  if (inotfd >=0) close(inotfd);
  if (netlinkfd >=0) close(netlinkfd);
  if (epollfd >=0) close(epollfd);
  if (sendtofd >=0) close(sendtofd);
  rmdir(runhostapdpath);
}

void intHandler(int signo) {
  switch (signo) {
    case SIGINT:  
      debugprintf(2, "intHandler received SIGINT\n");
      fflush(stdout);
      exit(EXIT_SUCCESS);
      break;
    case SIGTERM:  
      debugprintf(2, "intHandler received SIGTERM\n");
      break;
    case SIGQUIT:  
      debugprintf(2, "intHandler received SIGQUIT\n");
      break;
    case SIGTSTP:  
      debugprintf(2, "intHandler received SIGTSTP\n");
      break;
  }
  fflush(stdout);
  should_exit = true;
}

void sendsanldump() {
  struct sockaddr_nl sanldump = { .nl_family = AF_NETLINK };
  struct ifaddrmsg *ifa;
  struct nlmsghdr *nlh = (struct nlmsghdr *)calloc(1, NLMSG_SPACE(sizeof(*ifa)));
  nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
  nlh->nlmsg_pid = getpid();
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; // 
  nlh->nlmsg_type = RTM_GETADDR;
  ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
  ifa->ifa_family = AF_INET;
  ifa->ifa_index = 0;
  struct iovec iov = { .iov_base=nlh, .iov_len=nlh->nlmsg_len };
  struct msghdr msg = {
    .msg_name = (void *)&sanldump,
    .msg_namelen = sizeof(sanldump),
    .msg_iov = &iov,
    .msg_iovlen = 1,
  };
  sendmsg(netlinkfd,&msg,0);
  free(nlh);
}

void partition(char **p, char *string, char chr)
{
  p[0] = NULL; p[1] = NULL; p[2] = NULL;
  if (string == NULL) return;
  if (strlen(string) == 0) return;
  p[0] = string;
  char * found = strchr(string, chr);
  if (found != NULL) {
    found[0] = 0; // \0 terminate p[0]
    p[1] = found;
    p[2] = found + 1;
    return;
  }
}

void myline_f(myline_t * il, const char *line)
{
  char * part[3];
  strncpy(il->line, line, STRINGSIZE-1);
  partition(part, il->line, ' ');
  while (part[0] != NULL) {
    char * partpart[3], * partpartpart[3];
    partition(partpart, part[0], '=');
    if (strcmp(partpart[0], "FROM") == 0) {
      partition(partpartpart, partpart[2], '-');
      il->fromhost=partpartpart[0];
      il->fromsock=partpartpart[2];
    }  
    else if (strcmp(partpart[0], "TO") == 0) {
      partition(partpartpart, partpart[2], '-');
      il->tohost=partpartpart[0];
      il->tosock=partpartpart[2];
    }  
    else if ((strcmp(partpart[0], "COMMAND") == 0) || (strcmp(partpart[0], "RESPONSE") == 0) 
          || (strcmp(partpart[0], "EVENT") == 0)) {
      il->mytype=partpart[0];
      il->remain=partpart[2];
      if (part[1] != NULL) part[1][0] = ' ' ; // \0 back to space
      break;
    }
    partition(part, part[2], ' ');
  }
}

void process_inotify() {
  int length, i = 0;
  DIR *d;
  struct dirent *dir;
  length = read (inotfd, buffer, sizeof(buffer)); // inotfd is IN_NONBLOCK
  if (length <= 0) return;
  while (i < length) {
    struct inotify_event * event = (struct inotify_event *) & buffer[i];
    i += sizeof (struct inotify_event) + event->len;
    if (event->len == 0) continue;
    if ((event->wd == inotrunwd) && (strcmp(event->name, "hostapd") == 0) ){
      if (event->mask & IN_CREATE) {
        if ((d = opendir(runhostapdpath)) == NULL) return;
        inotrunhostapdwd = inotify_add_watch( inotfd, runhostapdpath, IN_CREATE | IN_DELETE  | IN_ONLYDIR);
        while ((dir = readdir(d)) != NULL)
          if (dir->d_type == DT_SOCK) node_add(NODE_HOSTAPD, dir->d_name, 0, 0);
        closedir(d);
      }
      else if (event->mask & IN_DELETE) {
        inotify_rm_watch( inotfd, inotrunhostapdwd );
        inotrunhostapdwd = -1;
      }            
    }
    else if (event->wd == inotrunhostapdwd) {
      if (event->mask & IN_CREATE) node_add(NODE_HOSTAPD, event->name, 0, 0);
      else if (event->mask & IN_DELETE) node_remove(node_from_name(event->name));
    }
  }
}


void process_netlink() {
  struct msghdr msg = { &sanlevent, sizeof(sanlevent), &iovbuffer, 1, NULL, 0, 0 };
  int len = recvmsg(netlinkfd, &msg, MSG_DONTWAIT);
  for (struct nlmsghdr *nh = (struct nlmsghdr *) buffer; NLMSG_OK (nh, len); nh = NLMSG_NEXT (nh, len)) {
    if (nh->nlmsg_type == NLMSG_DONE) break;
    if (nh->nlmsg_type == NLMSG_ERROR) continue;
    if ((nh->nlmsg_type!=RTM_NEWADDR) && (nh->nlmsg_type!=RTM_DELADDR) && (nh->nlmsg_type==RTM_GETADDR)) continue;
    //struct ifaddrmsg *ifaddr = (struct ifaddrmsg *)NLMSG_DATA(nh);
    //int index=ifaddr->ifa_index;
    struct rtattr *rta = IFA_RTA(NLMSG_DATA(nh));
    int rta_len = nh->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    in_addr_t addr=0, bcaddr=0xFFFFFFFF;
    char *name="";
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
      if (rta->rta_type == IFA_ADDRESS) {
        if (RTA_PAYLOAD(rta) == 4) addr=((struct in_addr *)RTA_DATA(rta))->s_addr;
      }
      else if (rta->rta_type == IFA_BROADCAST) {
        if (RTA_PAYLOAD(rta) == 4) bcaddr=((struct in_addr *)RTA_DATA(rta))->s_addr;
      }
      else if (rta->rta_type == IFA_LABEL) name = (char *)RTA_DATA(rta);
    }
    if ((addr==0x0100007f) && (notifysystemd >=0)) notifysystemd++;
    if ((nh->nlmsg_type == RTM_NEWADDR) || (nh->nlmsg_type == RTM_GETADDR)) {
      if (addr != 0) for (int i = 0 ; i<if_strings_cnt; i++)
        if (strcmp(if_strings[i], name) == 0)
          node_add(NODE_LOCALIP, name, addr, bcaddr);
    }
    else if (nh->nlmsg_type == RTM_DELADDR){
      node_remove(node_from_name(name));
    }
  }
}

void process_myline(myline_t * il, addr_node_t *a)
{
  // *** Do some processing for use in interhapd internally ***
  if (legacyfdb)
    if ( (strstr(il->remain, ">AP-STA-CONNECTED ") != NULL) 
          || (strstr(il->remain, ">AP-STA-DISCONNECTED ") != NULL) )
      node_add(NODE_REQUEST_DUMP, il->remain, 0, 0); // delete mac from fdb
  // Continue processing
  if (strcmp(il->tohost, hostname) == 0)
  {  // *** To localhost ***
    addr_node_t * toa = node_from_name(il->tosock);
    addr_node_t * fromhosta = node_from_name(il->fromhost);
    if (fromhosta == NULL) return; 
    if (toa != NULL) {
      if (toa->type == NODE_HOSTAPD) { // *** To local hostapd ***
        int recsize;
        char tempbuff[STRINGSIZE];
        char * part[3];
        partition(part, il->remain, '\n');
//          debugprintf(2, "***%s***\n", part[0]);
        send(toa->synchrfd, part[0], strlen(part[0]), 0);
        if ((recsize=recv(toa->synchrfd, tempbuff, sizeof(tempbuff)-1, 0)) >= 0) {  // flags ???
          tempbuff[recsize] = '\0';
          send2script(fromhosta, hostname, toa->name, il->fromhost, il->fromsock, "RESPONSE", tempbuff);
//          debugprintf(2, "***%s***\n", tempbuff);
        } 
      }
    } else {  // *** Unknow tosock, could be interhapd command or directed to local script ***
      if (strncmp(il->remain, ihapd_list_interfaces, strlen(ihapd_list_interfaces)) ==0) {
        char tempbuff[STRINGSIZE];
        tempbuff[0] = 0;
        int length = 0;
        for (addr_node_t *tempa = threadlist; tempa; tempa = tempa->next)
          if (tempa->type==NODE_HOSTAPD)
            length += snprintf(tempbuff+length, STRINGSIZE-length, "%s ", tempa->name);
        send2script(fromhosta, hostname, il->tosock, il->fromhost, il->fromsock, "RESPONSE", tempbuff);
      }
      else {  // *** To local script ***
        toa = node_from_name(hostname);
        if (toa != NULL)
          send2script(toa, il->fromhost, il->fromsock, il->tohost, il->tosock, il->mytype, il->remain);
      }
    }
  }
  else if (strlen(il->tohost) >0) {  // *** To remote host ***
    if (strcmp(il->tohost, "broadcast") == 0) {
      if (strcmp(il->fromhost, hostname) == 0) {
        // send original broadcast, only when originating from my script
        send2script(NULL, il->fromhost, il->fromsock, il->tohost, il->tosock, il->mytype, il->remain);
      }
      else {  // send incoming broadcast to my script
        addr_node_t * toa = node_from_name(hostname);
        if (toa != NULL)
          send2script(toa, il->fromhost, il->fromsock, hostname, il->tosock, il->mytype, il->remain);     
      }
    }
    else {  // send original message
      addr_node_t * toa = node_from_name(il->tohost);
      if (toa != NULL)
        send2script(toa, il->fromhost, il->fromsock, il->tohost, il->tosock, il->mytype, il->remain);
    }
  }
}

void process_script(addr_node_t *a) {
  int bytes = read(a->sockfd, buffer, sizeof(buffer)-1);
  char * part[3];
  buffer[bytes] = '\0';
  partition(part, buffer, '\n');
  while (part[0] != NULL) {
    if (part[0] != part[1]) {
      myline_t il = {"","","","","",""};
      myline_f(&il, part[0]);
      process_myline(&il, a);
    }
    partition(part, part[2], '\n');
  }
}

void process_ipsocket(addr_node_t *a) {
  int recsize;
  struct sockaddr_in si_other;
  socklen_t addr_size= sizeof(si_other);
  while ((recsize=recvfrom(a->sockfd, buffer, sizeof(buffer)-1, MSG_DONTWAIT, 
                           (struct sockaddr*)& si_other, &addr_size)) > 0) {
    buffer[recsize] = '\0';
    myline_t il = {"","","","","",""};
    myline_f(&il, buffer);
    addr_node_t *recva = node_from_addr(si_other.sin_addr.s_addr);
    if (recva) if (recva->type == NODE_LOCALIP) continue;  // Skip processing broadcast from myself
    if (strncmp(il.remain, ihapd_list_started, strlen(ihapd_list_started)) ==0) {
      if (recva == NULL) recva=node_add(NODE_REMOTEIP, il.fromhost, si_other.sin_addr.s_addr, 0);
      if (recva == NULL) continue;
      if (strncmp(il.remain, ihapd_list_started_noreply, strlen(ihapd_list_started_noreply)) !=0)
        send2script(recva, hostname, "event", il.fromhost, "event", "EVENT", ihapd_list_started_noreply);
    }  
    else if (strncmp(il.remain, ihapd_list_stopped, strlen(ihapd_list_stopped)) ==0) {
      if (recva == NULL) continue;
      node_remove(recva);
    }
    process_myline(&il, a);
  }
}

void process_hapdsocket(addr_node_t *a) {
  int recsize;
  while ((recsize=recvfrom(a->sockfd, buffer, sizeof(buffer)-1, MSG_DONTWAIT, 
                           NULL, NULL)) > 0) {
    if (buffer[0] != '<') continue;
    buffer[recsize] = '\0';
    myline_t il = {
        .fromhost=hostname,
        .fromsock=a->name,
        .tohost="",
        .tosock="",
        .mytype = "EVENT",
        .remain = buffer
    };
    for (addr_node_t *toa = threadlist; toa; toa = toa->next)          // Send to all scripts
      if ((toa->type == NODE_SCRIPT) || (toa->type == NODE_REMOTEIP))
        send2script(toa, il.fromhost, il.fromsock, toa->name, "event", il.mytype, il.remain);
    process_myline(&il, a);
  }
}

void process_request_socket(addr_node_t *a)
{
  struct msghdr msg = { &sanldump, sizeof(sanldump), &iovbuffer, 1, NULL, 0, 0 };
  int len = recvmsg(a->sockfd, &msg, 0);
  int reclen = len, count = 0; 
  debugprintf(2, "Received dump request, delete %s\n", a->name);
  for (struct nlmsghdr *nh = (struct nlmsghdr *) buffer; NLMSG_OK (nh, len); nh = NLMSG_NEXT (nh, len)) {
    if (nh->nlmsg_type == NLMSG_DONE) break;
    if (nh->nlmsg_type == NLMSG_ERROR) continue;
    if ((nh->nlmsg_type!=RTM_NEWNEIGH) && (nh->nlmsg_type!=RTM_DELNEIGH) && (nh->nlmsg_type!=RTM_GETNEIGH)) continue;
    bool found = false;
    unsigned char *ll = 0;
    struct ndmsg *ndm = (struct ndmsg *)NLMSG_DATA(nh);
    int index=ndm->ndm_ifindex;
    struct rtattr *rta = NDA_RTA(NLMSG_DATA(nh));
    int rta_len = nh->nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg));
    unsigned int master=0, vlan=0;
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
           if (rta->rta_type == NDA_LLADDR) ll = (unsigned char *)RTA_DATA(rta); 
      else if (rta->rta_type == NDA_MASTER) master=*(unsigned int*)RTA_DATA(rta);
      else if (rta->rta_type == NDA_VLAN) vlan=*(unsigned int*)RTA_DATA(rta);
    }
    if (ll) {
      if (memcmp(ll, a->mac, ETH_ALEN) == 0) {
        debugprintf(2, "Delete %02x:%02x:%02x:%02x:%02x:%02x index %d vlan %d master %d\n", 
            ll[0], ll[1], ll[2], ll[3], ll[4], ll[5], index, vlan, master);
        nh->nlmsg_pid = getpid();
        nh->nlmsg_flags = NLM_F_REQUEST;
        nh->nlmsg_type = RTM_DELNEIGH;
        ndm = (struct ndmsg *)NLMSG_DATA(nh);
        ndm->ndm_state = 0;
        count++; found = true;
      }
    }
    if (!found) nh->nlmsg_type = NLMSG_NOOP;
  }
  if (count) { // send the altered msg back
    struct iovec iov = { buffer, reclen };
    struct msghdr msg = {
      .msg_name = (void *)&sanldump,
      .msg_namelen = sizeof(sanldump),
      .msg_iov = &iov,
      .msg_iovlen = 1,
    };
    sendmsg(a->sockfd,&msg,0);
  }
  node_remove(a);
}

void mkdirp(char* dirpath) {
  char * path;
  char s[STRINGSIZE];
  int len=strlen(dirpath), i=0;
  while (len > 0) {
    snprintf(s, STRINGSIZE-1, "%s/", dirpath);
    while ( (path= strrchr(s, '/')) != NULL) {
      len = path - s;
      s[len] = 0;
      if (len > 0) if (mkdir(s, 0x0750) == 0) break;
      if (++i > 50) break;
    }
    if (++i > 50) break;
  }
  if (i > 50) debugprintf(LEVEL_EXIT, "Error: mkdir -p %s failed!\n", dirpath );
}

int main(int argc, char *argv[])
{
  int timeoutcnt=0;
  struct sigaction act={.sa_handler = intHandler};
  struct epoll_event rtev;
  DIR *d;
  struct dirent *dir;

  argv++; argc--; 
  if (argc > 0) {
    while (argv[0][0] == '-') {
      if (argc<1) debugprintf(LEVEL_EXIT, "Option error: %s\n", argv[0]);
      if (strlen(argv[0]) != 2) debugprintf(LEVEL_EXIT, "Unknown option: %s\n", argv[0]);
      switch (argv[0][1]) {
        case 'l':
          legacyfdb = true;
          argv+=1; argc-=1;
          break;
        case 'd':
          debuglevel = (int)strtol(argv[1], NULL, 10);
          argv+=2; argc-=2;
          break;
        case 'p':
          port = (int)strtol(argv[1], NULL, 10);
          argv+=2; argc-=2;
          break;
        case 's':
          script = argv[1];
          argv+=2; argc-=2;
          break;
        case 'h':
          runhostapdpath = argv[1];
          argv+=2; argc-=2;
          break;
        default:
          debugprintf(LEVEL_EXIT, "Unknown option: %s\n", argv[0]);
      }
      if (argc <= 0) break;
    }
  }
  if (argc < 0) debugprintf(LEVEL_EXIT, "Arguments error.\n");
  if (argc == 0) { 
    if_strings = lanbridge; if_strings_cnt = 1; 
  } else {
    if_strings = argv;      if_strings_cnt = argc; 
  }
  if (runhostapdpath[0] != '/') debugprintf(LEVEL_EXIT, "Option '-h %s' is not an absolute path\n", runhostapdpath );
  if (runhostapdpath[strlen(runhostapdpath)-1] == '/') runhostapdpath[strlen(runhostapdpath)-1] = 0;
  strncpy(runpath, runhostapdpath, STRINGSIZE-1); 
  runpath[strrchr(runhostapdpath, '/') - runhostapdpath] = 0; // there is always at least one '/'
  mkdirp(runpath);

  gethostname(hostname, HOST_NAME_MAX + 1);

  if (sigaction(SIGTERM, &act, NULL) == -1) debugprintf(LEVEL_EXIT, "Signal action SIGTERM error: %s\n", strerror(errno));
  if (sigaction(SIGTSTP, &act, NULL) == -1) debugprintf(LEVEL_EXIT, "Signal action SIGTSTP error: %s\n", strerror(errno));
  if (sigaction(SIGQUIT, &act, NULL) == -1) debugprintf(LEVEL_EXIT, "Signal action SIGQUIT error: %s\n", strerror(errno));
  if (sigaction(SIGINT,  &act, NULL) == -1) debugprintf(LEVEL_EXIT, "Signal action SIGINT error: %s\n", strerror(errno));

  atexit(&exitfunc);

  epollfd = epoll_create(sizeof(epollfd));
  if (epollfd < 0) debugprintf(LEVEL_EXIT,"epoll_create failed: %s\n", strerror(errno));

  sendtofd = socket(PF_INET, SOCK_DGRAM, 0);
  if (sendtofd < 0) debugprintf(LEVEL_EXIT, "send socket() error: %s\n", strerror(errno));
  int bcPermission = 1;
  if (setsockopt(sendtofd, SOL_SOCKET, SO_BROADCAST, (void *) &bcPermission, sizeof(bcPermission)) < 0) 
    debugprintf(LEVEL_EXIT, "setsockopt() SO_BROADCAST error: %s\n", strerror(errno));

  node_add(NODE_SCRIPT, hostname, 0, 0);

  netlinkfd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
  if (netlinkfd < 0) debugprintf(LEVEL_EXIT, "netlink socket() error: %s\n", strerror(errno));
  bind(netlinkfd, (struct sockaddr *) &sanlevent, sizeof(sanlevent));       
  {struct epoll_event ev = {
      .events = EPOLLIN|EPOLLET,
      .data.fd = netlinkfd,
    };
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, netlinkfd, &ev) < 0) 
      debugprintf(LEVEL_EXIT,"Could not add netlink to epoll: %s\n", strerror(errno));
  }
  sendsanldump();
      
  if ((inotfd = inotify_init1(IN_NONBLOCK)) < 0) 
    debugprintf(LEVEL_EXIT, "inotify_init() error %s\n", strerror(errno));
  {struct epoll_event ev = {
      .events = EPOLLIN|EPOLLET,
      .data.fd = inotfd,
    };
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, inotfd, &ev) < 0) 
      debugprintf(LEVEL_EXIT,"Could not add inotify to epoll %s\n", strerror(errno));
  }
  if ((inotrunwd = inotify_add_watch( inotfd, runpath, IN_CREATE | IN_DELETE | IN_ONLYDIR )) == -1) 
    debugprintf(LEVEL_EXIT, "inotify_add_watch error: %s\n", strerror(errno));
  if ((d = opendir(runhostapdpath)) != NULL) {
    inotrunhostapdwd = inotify_add_watch( inotfd, runhostapdpath, IN_CREATE | IN_DELETE | IN_ONLYDIR );
    while ((dir = readdir(d)) != NULL)
      if (dir->d_type == DT_SOCK) node_add(NODE_HOSTAPD, dir->d_name, 0, 0);
    closedir(d);
  }    

  while (should_exit == false)
  {
    switch (epoll_wait(epollfd, &rtev, 1, 1000)) { // the only place in the code we wait
      case -1: // epoll_wait failed
        continue;
      case 0:  // epoll_wait timeout
        if (timeoutcnt++ >= 3600) {
          timeoutcnt = 0;
          debugprintf(2,"Timeout 1 hour\n");
          for (addr_node_t *a = threadlist; a; a = a->next) {
            switch (a->type) {
              case NODE_LOCALIP:
                send2script(NULL, hostname, "event", "broadcast", "event", "EVENT", ihapd_list_started_noreply);
                break;
            }
          }
        }
        continue;
      default:
        if (rtev.data.fd==inotfd) {
          process_inotify();
        }
        else if (rtev.data.fd==netlinkfd) {
         process_netlink();          
         if (notifysystemd > 0) { // after receiving 127.0.0.1, we are ready
           #ifdef USE_SYSTEMD
             sd_notify(0, "READY=1");
           #endif
           notifysystemd=-99999;
         }
        }
        else for (addr_node_t *a = threadlist; a; a = a->next) {
          if (rtev.data.fd != a->sockfd) continue;
          switch (a->type) {
            case NODE_HOSTAPD:
              process_hapdsocket(a);
              break;
            case NODE_LOCALIP:
              process_ipsocket(a);
              break;
            case NODE_SCRIPT:
              process_script(a);
              break;
            case NODE_REQUEST_DUMP:
              process_request_socket(a);
              break;
          }
          break; // becasue fd was found and threadlist may be altered
        }
        break;
    }
  }
  #ifdef USE_SYSTEMD
    sd_notify(0, "STOPPING=1");
  #endif
  
  exit(EXIT_SUCCESS);
}

