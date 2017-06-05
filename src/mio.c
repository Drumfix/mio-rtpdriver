/*
 *  Copyright (c) by Ralf Beck <ralfbeck1@gmx.de>
 *  Userspace Ethernet driver for the iConnectivity Mio/Midi+ (tm) devices
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <endian.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <signal.h>

#include "rtp.h"

#define BUFLEN 4096

typedef void (*sighandler_t)(int);

static sighandler_t handle_signal (int sig_nr, sighandler_t signalhandler) {

   struct sigaction new_sig, old_sig;
   new_sig.sa_handler = signalhandler;
   sigemptyset (&new_sig.sa_mask);
   new_sig.sa_flags = SA_RESTART;

   if (sigaction (sig_nr, &new_sig, &old_sig) < 0)
      return SIG_ERR;

   return old_sig.sa_handler;
}

static void start_daemon (const char *log_name, int facility) {
   int i;
   pid_t pid;

   if ((pid = fork ()) != 0)
      exit (EXIT_FAILURE);

   if (setsid() < 0) {
      printf("%s cant get session leader!\n", log_name);
      exit (EXIT_FAILURE);
   }

   handle_signal (SIGHUP, SIG_IGN);

   if ((pid = fork ()) != 0)
      exit (EXIT_FAILURE);

   chdir ("/");
   umask (0);

   for (i = sysconf (_SC_OPEN_MAX); i > 0; i--)
      close (i);

   openlog ( log_name, 
             LOG_PID | LOG_CONS| LOG_NDELAY, facility );
}

// remote rtpmidi partner address and port

#define INVITE_CONTROL       1
#define WAIT_INVITE_CONTROL  2

#define INVITE_DATA          3
#define WAIT_INVITE_DATA     4

#define SYNC                 3
#define SYNC_OK              4
#define ONLINE               5

int state = 1;
int last_received = 0;

struct sockaddr_in si_control;
struct sockaddr_in si_data;

int sc, sd;

snd_seq_t *seq;

int seq_port;

// alsa event encoder/decoder

snd_midi_event_t *encoder;
snd_midi_event_t *decoder;

struct timespec start, end;

void diep(char *s)
{
  perror(s);
  exit(1);
}

int period = 1;

// sysex buffer for segmented sysex messages

char sysexbuffer[4096];
int in_sysex = 0;

// parse the data received from the MIO

void parse(unsigned char *data)
{
     // we ignore all timestamps
 
     snd_seq_event_t ev;
     int count;

     int xx;

     rtp_header_s *rtp_header = (rtp_header_s *) data;
     
     data += sizeof(rtp_header_s);

     unsigned int bytes = 0;
     unsigned int timestamp = 0;
     unsigned short z = 0;

     // b=1 => long header

     if (*data & 0x80)
     {
        z = *data & 0x40 ? 1 : 0;
        bytes = ((*data++ & 0x0f )<< 8) + *data++;
     }
     else
     {
        z = *data & 0x40 ? 1 : 0;
        bytes = *data++ & 0x0f;
     }  

     if (z)
     {
        timestamp = 0;

        // read initial delta time
        while ((*data & 0x80) && bytes)
        {
           timestamp = (timestamp << 7) | *data++;
           bytes--;
        }

        if (bytes)
        {
           timestamp |= *data++;
           bytes--;
        }
     }

     // read the rest 

     while (bytes)
     {
        count = 0;

        if (*data == 0xf0)
        {
           sysexbuffer[in_sysex++] = *data++;
           bytes--;

           // process the rest
           
           while (!(*data & 0x80))
           {
              sysexbuffer[in_sysex++] = *data++;
              bytes--;               
           }

           if (*data == 0xf7)
           {
              // sysex message is complete

              sysexbuffer[in_sysex++] = *data++;
              bytes--;

              snd_seq_ev_clear(&ev);
              count = snd_midi_event_encode(encoder, sysexbuffer, in_sysex, &ev);
              
              in_sysex = 0;
           }
           else
           {
              bytes--; data++;
           }
        }
        else if ((*data == 0xf7) && (in_sysex))
        {
           // remove leading 0xf7

           bytes--; data++;

           // process the rest
           
           while (!(*data & 0x80))
           {
              sysexbuffer[in_sysex++] = *data++;
              bytes--;               
           }

           if ((*data == 0xf7) || (*data == 0xf5)) // regular f7 or dropped f7 (=f5)
           {
              // sysex message is complete

              sysexbuffer[in_sysex++] = 0xf7;
              bytes--; data++;

              snd_seq_ev_clear(&ev);
              count = snd_midi_event_encode(encoder, sysexbuffer, in_sysex, &ev);
              
              in_sysex = 0;
           }
           else if (*data == 0xf4)
           {
               // cancel sysex
               in_sysex = 0; data++; bytes--;
           }
           else  // 0xf0
           {
               // ignore
               data++; bytes--;
           }
        }
        else
        {
           snd_seq_ev_clear(&ev);
           count = snd_midi_event_encode(encoder, data, 4095, &ev);

           if (count > 0)
           {
              bytes -= count;
              data += count;
           }
        }

        if (count)
        {
           snd_seq_ev_set_source(&ev, seq_port);
           snd_seq_ev_set_subs(&ev);
           snd_seq_ev_set_direct(&ev);
 
           snd_seq_event_output(seq, &ev);
           snd_seq_drain_output(seq);
        }

        // read timestamp

        while ((*data & 0x80) && bytes)
        {
           timestamp = (timestamp << 7) | *data++;
           bytes--;
        }

        if (bytes)
        {
           timestamp |= *data++;
           bytes--;
        }
     }
}

// the control thread

void *sync_fun(void *arg)
{
    uint64_t dummy, t0;

    unsigned char buf[BUFLEN];

    apple_midi_sync *sync = (apple_midi_sync *)&buf;

    int fd = timerfd_create(CLOCK_MONOTONIC, 0);

    struct itimerspec ts;

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;

    ts.it_value.tv_sec = now.tv_sec;
    ts.it_value.tv_nsec = now.tv_nsec;

    while (1)
    {
       // send sync 0

       clock_gettime(CLOCK_MONOTONIC, &now);
    
       t0 = htobe32((uint64_t)now.tv_sec * 10000 + (uint64_t)now.tv_nsec / 100000);

       sync->command = APPLE_MIDI_SYNC;
       sync->ssrc    = APPLE_MIDI_SSRC;
       sync->count   = 0;
       sync->pad[0]  = 0;
       sync->pad[1]  = 0;  
       sync->pad[2]  = 0;   
       sync->ts0_h   = 0;
       sync->ts0_l   = t0;
       sync->ts1_h   = 0;
       sync->ts1_l   = 0;
       sync->ts2_h   = 0;
       sync->ts2_l   = 0;

       if (sendto(sd, buf, sizeof(apple_midi_sync), 0, (const struct sockaddr *)&si_data, sizeof(si_data))==-1)
          syslog( LOG_NOTICE, "sending SYNC0 failed\n");

       ts.it_value.tv_sec += period;

       timerfd_settime(fd, TFD_TIMER_ABSTIME, &ts, NULL);

       period += 3;

       if (period >= 10) period = 10;

       read(fd, &dummy, sizeof(dummy));
    }
    return NULL;
}

// rtp thread

void *rtp_fun(void *arg)
{
    uint64_t dummy, t1, t2;

    unsigned char buf[BUFLEN];

    apple_midi_sync *sync = (apple_midi_sync *)&buf;
    apple_midi_bye *bye = (apple_midi_bye *)&buf;

    struct timespec now;

    while (1)
    {

       recvfrom(sd, buf, BUFLEN, 0, NULL, NULL);

       if (buf[0] != 0xff) 
       {
          parse(buf);
       }
       else
       {
          switch (sync->command)
          {
            case APPLE_MIDI_SYNC:

              switch (sync->count)
              {
                 case 0:

                   // send sync 1

                   clock_gettime(CLOCK_MONOTONIC, &now);
    
                   t1 = htobe32((uint64_t)now.tv_sec * 10000 + (uint64_t)now.tv_nsec / 100000);

                   sync->command = APPLE_MIDI_SYNC;
                   sync->ssrc    = APPLE_MIDI_SSRC;
                   sync->count   = 1;
                   sync->pad[0]  = 0;
                   sync->pad[1]  = 0;  
                   sync->pad[2]  = 0;   
                   sync->ts1_h   = 0;
                   sync->ts1_l   = t1;
                   sync->ts2_h   = 0;
                   sync->ts2_l   = 0;

                   if (sendto(sd, buf, sizeof(apple_midi_sync), 0, (const struct sockaddr *)&si_data, sizeof(si_data))==-1)
                       syslog( LOG_NOTICE, "sending SYNC1 failed\n");

                   break;
         
                 case 1:

                   // send sync 2

                   clock_gettime(CLOCK_MONOTONIC, &now);
    
                   t2 = htobe32((uint64_t)now.tv_sec * 10000 + (uint64_t)now.tv_nsec / 100000);

                   sync->command = APPLE_MIDI_SYNC;
                   sync->ssrc    = APPLE_MIDI_SSRC;
                   sync->count   = 2;
                   sync->pad[0]  = 0;
                   sync->pad[1]  = 0;  
                   sync->pad[2]  = 0;   
                   sync->ts2_h   = 0;
                   sync->ts2_l   = t2;

                   if (sendto(sd, buf, sizeof(apple_midi_sync), 0, (const struct sockaddr *)&si_data, sizeof(si_data))==-1)
                      syslog( LOG_NOTICE, "sending SYNC2 failed\n");
                   break;

                 case 2:
                   break;
              }
              break;

            case  APPLE_MIDI_BYE:

              syslog( LOG_NOTICE, "Mio closed connection, exiting\n");
              exit(0);
              break;

            default:
              break;
          }
       }
     }

   return NULL;
}

int main(int argc, char **argv)
{
  char buf[BUFLEN];
  char ret_buf[BUFLEN];
  int i;

  if (argc != 5)
  {
     printf("usage: mio <alsa client name> <mio host address> <mio port> <local port>\n");
     exit(1);
  }

  char *alsa_client_name = argv[1];
  char *mio_host_address = argv[2];
  int   mio_port         = atoi(argv[3]);
  int   own_port         = atoi(argv[4]);

  start_daemon (alsa_client_name, LOG_LOCAL0);

  apple_midi_invite *invite = (apple_midi_invite *)&buf;
  apple_midi_sync *sync = (apple_midi_sync *)&buf;
  apple_midi_bye *bye = (apple_midi_bye *)&buf;
  apple_midi_feedback *feedback = (apple_midi_feedback *)&buf;
  apple_midi_rate *rate = (apple_midi_rate *)&buf;
    
  apple_midi_invite *invite_reply = (apple_midi_invite *)&ret_buf;
  apple_midi_sync *sync_reply = (apple_midi_sync *)&ret_buf;
  apple_midi_bye *bye_ret = (apple_midi_bye *)&ret_buf;
  apple_midi_feedback *feedback_reply = (apple_midi_feedback *)&ret_buf;
  apple_midi_rate *rate_reply = (apple_midi_rate *)&ret_buf;
  
  struct timespec now;
  uint32_t timestamp;

  int optVal = 1;
  socklen_t optLen = sizeof(optVal);

  if ((sc=socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1) {
      syslog( LOG_NOTICE, "creating socket for control port failed, exiting\n");
      exit(1);
  }

  setsockopt(sc, SOL_SOCKET, SO_REUSEADDR, (void*) &optVal, optLen);

  if ((sd=socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1) {
      syslog( LOG_NOTICE, "creating socket for data port failed, exiting\n");
      exit(1);
  }

  setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (void*) &optVal, optLen);

  // we must bind the sockets to two consecutive ports
  // otherwise the MIO will reject the invitaion on the data port

  struct sockaddr_in si_me;

  memset((char *) &si_me, 0, sizeof(si_me));
  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(own_port);
  si_me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sc, (const struct sockaddr *)&si_me, sizeof(si_me))==-1) {
      syslog( LOG_NOTICE, "binding local port %d failed, exiting\n", own_port);
    exit(1);
  }

  memset((char *) &si_me, 0, sizeof(si_me));
  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(own_port+1);
  si_me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sd, (const struct sockaddr *)&si_me, sizeof(si_me))==-1) {
      syslog( LOG_NOTICE, "binding local port %d failed, exiting\n", own_port+1);
    exit(1);
  }

  // now create the addresses of the remote partner

  memset((char *) &si_control, 0, sizeof(si_control));
  si_control.sin_family = AF_INET;
  si_control.sin_port = htons(mio_port);
  if (inet_aton(mio_host_address, &si_control.sin_addr)==0) {
    syslog( LOG_NOTICE, "failed to create mio host address/control port, exiting\n");
    exit(1);
  }

  memset((char *) &si_data, 0, sizeof(si_data));
  si_data.sin_family = AF_INET;
  si_data.sin_port = htons(mio_port+1);
  if (inet_aton(mio_host_address, &si_data.sin_addr)==0) {
    syslog( LOG_NOTICE, "failed to create mio host address/data port, exiting\n");
    exit(1);
  }

/*  invitation start */

  int retu = -1;
  int countu = 0;

  while (retu <= 0)
  {
     struct pollfd fds[1];

     // send invitation to control port

     fds[0].fd = sc;
     fds[0].events = POLLIN;
     fds[0].revents = 0;

     invite->command = APPLE_MIDI_INVITE;
     invite->version = APPLE_MIDI_VERSION;
     invite->token   = APPLE_MIDI_TOKEN;
     invite->ssrc    = APPLE_MIDI_SSRC;
     buf[16] ='P';   buf[17] = 'O';  buf[18] = 0x00; buf[19] = 0x00;

     if (sendto(sc, buf, 19, 0, (const struct sockaddr *)&si_control, sizeof(si_control))==-1)
        syslog( LOG_NOTICE, "error during sending of invitation on control port\n");

     retu = poll(fds, 1, 1000);

     if (retu < 0)
     {
        sleep(5);
     }

     if (retu > 0)
     {
         if (recvfrom(sc, ret_buf, BUFLEN, 0, NULL, NULL) == -1)
            syslog( LOG_NOTICE, "error during reception of invitation reply on control port\n");
     }

     if (retu == 0)
     {
        sleep(4);
     }

  }

  // send invitation to data port

  invite->command = APPLE_MIDI_INVITE;
  invite->version = APPLE_MIDI_VERSION;
  invite->token   = APPLE_MIDI_TOKEN;
  invite->ssrc    = APPLE_MIDI_SSRC;
  buf[16] ='P';   buf[17] = 'O';  buf[18] = 0x00; buf[19] = 0x00;

  if (sendto(sd, buf, 19, 0, (const struct sockaddr *)&si_data, sizeof(si_data))==-1)
     syslog( LOG_NOTICE, "error during sending of invitation on data port\n");

   sleep(1);

   if (recvfrom(sd, ret_buf, BUFLEN, 0, NULL, NULL) == -1)
     syslog( LOG_NOTICE, "error during reception of invitation reply on data port\n");

  int err;
  err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
  if (err < 0)
      exit(1);
  snd_seq_set_client_name(seq, alsa_client_name);

  seq_port = snd_seq_create_simple_port(seq, &ret_buf[16],
                        SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE|
                        SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ|
                        SND_SEQ_PORT_CAP_DUPLEX,
                        SND_SEQ_PORT_TYPE_MIDI_GENERIC);

  snd_midi_event_new(4095, &encoder);
  snd_midi_event_no_status(encoder, seq_port);

  snd_midi_event_new(4095, &decoder);
  snd_midi_event_no_status(decoder, seq_port);

  // start sync thread

  pthread_t sync_thread;

  pthread_create(&sync_thread, NULL, sync_fun, NULL);

  // start rtp thread

  pthread_t rtp_thread;

  pthread_create(&rtp_thread, NULL, rtp_fun, NULL);
  snd_seq_event_t *ev;

  const int RTP_HEADER_SIZE = 12;
  const int RTP_MIDI_SHORT_HEADER_SIZE = 1;
  const int RTP_MIDI_LONG_HEADER_SIZE = 2;
  const int RTP_MIDILIST_MAX_SIZE = 4095;

  char outbuf[RTP_HEADER_SIZE+RTP_MIDI_LONG_HEADER_SIZE+RTP_MIDILIST_MAX_SIZE];
  
  /* initialize rtp midi header */

  rtp_header_s *hdr = (rtp_header_s *)&outbuf[0];
  
  uint16_t seq_num = 0;

  hdr->version = 2;
  hdr->p = 0;
  hdr->x = 0;
  hdr->cc = 0;
  hdr->m = 0;    // MIO firmware bug: see README
  hdr->payload_type = 97;
  hdr->seq_num = 0;
  hdr->ts = 0;
  hdr->ssrc = APPLE_MIDI_SSRC;

  char *rtp_mh = (char *)&outbuf[RTP_HEADER_SIZE];
  
  long count;

  while (snd_seq_event_input(seq, &ev) >= 0) {

      snd_midi_event_reset_decode(decoder);

      switch (ev->type)
      {
          case SND_SEQ_EVENT_SYSEX:
          {  
              clock_gettime(CLOCK_MONOTONIC, &start);

              hdr->seq_num = htobe16(seq_num++);
              hdr->ts = htobe32((uint64_t)start.tv_sec * 10000 + (uint64_t)start.tv_nsec / 100000);

              if (ev->data.ext.len <= 15)
              {
                 // short header

                 rtp_mh[0] = ev->data.ext.len;
                 memcpy(&rtp_mh[1], ev->data.ext.ptr, ev->data.ext.len);
                 sendto(sd, outbuf, RTP_HEADER_SIZE+1+ev->data.ext.len, 0, (const struct sockaddr *)&si_data, sizeof(si_data));

             }
             else
             {
                 // long header

                 rtp_mh[0] = 0x80 | (ev->data.ext.len >> 8);
                 rtp_mh[1] = ev->data.ext.len & 0xff;

                 memcpy(&rtp_mh[2], ev->data.ext.ptr, ev->data.ext.len);
                 sendto(sd, outbuf, RTP_HEADER_SIZE+2+ev->data.ext.len, 0, (const struct sockaddr *)&si_data, sizeof(si_data));
             }
           break;
          }
          case SND_SEQ_EVENT_NONREGPARAM:
               SND_SEQ_EVENT_REGPARAM:
          {
              clock_gettime(CLOCK_MONOTONIC, &start);

              hdr->seq_num = htobe16(seq_num++);
              hdr->ts = htobe32((uint64_t)start.tv_sec * 10000 + (uint64_t)start.tv_nsec / 100000);

              rtp_mh[0] = 15;
              count = snd_midi_event_decode(decoder, &rtp_mh[1], 12, ev);

              rtp_mh[15] = rtp_mh[12];
              rtp_mh[14] = rtp_mh[11];
              rtp_mh[13] = rtp_mh[10];
              rtp_mh[12] = 22;
              rtp_mh[11] = rtp_mh[9];
              rtp_mh[10] = rtp_mh[8];
              rtp_mh[9]  = rtp_mh[7];
              rtp_mh[8]  = 16;
              rtp_mh[7]  = rtp_mh[6];
              rtp_mh[6]  = rtp_mh[5];
              rtp_mh[5]  = rtp_mh[4];
              rtp_mh[4]  = 10;

              sendto(sd, outbuf, RTP_HEADER_SIZE+1+count, 0, (const struct sockaddr *)&si_data, sizeof(si_data));
           break;
          }
          default:
          {
              count = snd_midi_event_decode(decoder, &rtp_mh[1], 12, ev);

              if (count > 0)
              {
                 clock_gettime(CLOCK_MONOTONIC, &start);

                 hdr->seq_num = htobe16(seq_num++);
                 hdr->ts = htobe32((uint64_t)start.tv_sec * 10000 + (uint64_t)start.tv_nsec / 100000);

                 rtp_mh[0] = count;

                 sendto(sd, outbuf, RTP_HEADER_SIZE+1+count, 0, (const struct sockaddr *)&si_data, sizeof(si_data));
              }
              break;
          }

     }

  }
 
  void *ret;

  pthread_join(rtp_thread, &ret);
  pthread_join(sync_thread, &ret);

  syslog( LOG_NOTICE, "daemon %s has ended\n", alsa_client_name);
  closelog();

  close(sc); close(sd);
  return 0;
}

