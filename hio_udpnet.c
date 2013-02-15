/*
 * hio_net.c - TCP/IP networking access to PS2 HDD
 * $Id: hio_udpnet.c,v 1.1 2005/12/08 20:46:02 bobi Exp $
 *
 * Copyright 2004 Bobi B., w1zard0f07@yahoo.com
 *
 * This file is part of hdl_dump.
 *
 * hdl_dump is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * hdl_dump is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with hdl_dump; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#if defined (_BUILD_WIN32)
#  if defined (_MSC_VER) && defined (_WIN32)
#    include <winsock2.h> /* Microsoft Visual C/C++ compiler */
#  else
#    include <winsock.h> /* GNU C/C++ compiler */
#  endif
#  include <windows.h>
#elif defined (_BUILD_UNIX)
#  include <errno.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#endif
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "osal.h"
#include "hio_udpnet.h"
#include "byteseq.h"
#include "net_io.h"
#include "net_common.h"
#include "retcodes.h"


typedef struct hio_net_type
{
  hio_t hio;
  SOCKET sock;
  SOCKET udp;
  unsigned long error_code;
  size_t quick_packets;
  time_t delay_time;
} hio_net_t;

#define SETBIT(mask, bit) (mask)[(bit) / 32] |= 1 << ((bit) % 32)
#define GETBIT(mask, bit) ((mask)[(bit) / 32] & (1 << ((bit) % 32)))

#if defined (_BUILD_WIN32)
static int net_init = 0;
#endif

#if defined (_BUILD_WIN32)
/* it is not the same, since usleep works in microseconds,
 * whereas Sleep is in miliseconds */
#  define usleep Sleep
#endif


/**************************************************************/
/* execute a command and wait for a reply */
static int
query (SOCKET s,
       unsigned long command,
       unsigned long sector,
       unsigned long num_sectors,
       unsigned long *response,
       char output [HDD_SECTOR_SIZE * NET_NUM_SECTORS]) /* or NULL */
{
  unsigned char cmd [NET_IO_CMD_LEN];
  u_int32_t cmd_length = NET_IO_CMD_LEN;
  int result;
  set_u32 (cmd +  0, command);
  set_u32 (cmd +  4, sector);
  set_u32 (cmd +  8, num_sectors);
  set_u32 (cmd + 12, 0x003a2d29);

  result = send_exact (s, (const char*) cmd, cmd_length, 0);
  if (result == cmd_length)
    { /* command successfully sent */
      result = recv_exact (s, (char*) cmd, NET_IO_CMD_LEN, 0);
      if (result == NET_IO_CMD_LEN)
	{ /* response successfully received */
	  if (get_u32 (cmd + 0) != command ||
	      get_u32 (cmd + 4) != sector ||
	      get_u32 (cmd + 8) != num_sectors)
	    return (RET_PROTO_ERR);

	  *response = get_u32 (cmd + 12);
	  if (output != NULL &&
	      *response != (u_int32_t) -1)
	    { /* receive additional information */
	      size_t bytes_expected = HDD_SECTOR_SIZE * num_sectors;
	      result = recv_exact (s, output, bytes_expected, 0);
	      result = result == bytes_expected ? RET_OK : RET_ERR;
	    }
	  else
	    result = RET_OK;
	}
      else
	result = RET_ERR;
    }
  else
    result = RET_ERR;
  return (result);
}


/**************************************************************/
static int
send_for_write (hio_net_t *net,
		unsigned long command,
		unsigned long sector,
		unsigned long num_sectors,
		unsigned long *response,
		const char data [HDD_SECTOR_SIZE * NET_NUM_SECTORS])
{
  size_t quick_packets = net->quick_packets;
  time_t delay_time = net->delay_time;

  u_int32_t bitmask[(NET_NUM_SECTORS + 31) / 32] = { 0 };
  typedef struct udp_packet_t
  {
    unsigned char sector[HDD_SECTOR_SIZE * 2];
    unsigned long command, start;
  } udp_packet_t;
  udp_packet_t packet;

  /* ask to start writing operation */
  int result = query (net->sock, CMD_HIO_WRITE, sector, num_sectors, response, NULL);
  if (result == RET_OK &&
      *response == 0)
    {
      do
	{ /* flood with (unconfirmed) data */
	  size_t i, n = 0;
	  for (i = 0; i < num_sectors; i += 2)
	    if (!GETBIT (bitmask, i))
	      {
		memcpy (packet.sector, data + i * HDD_SECTOR_SIZE,
			HDD_SECTOR_SIZE * 2);
		set_u32 (&packet.command, command);
		set_u32 (&packet.start, sector + i);
		send (net->udp, (void*) &packet, sizeof (packet), 0);

		/* that is pretty heuristic */
		if ((n++ % quick_packets) == 0)
		  usleep (delay_time);
	      }

	  /* ask if all data is received */
	  result = query (net->sock, CMD_HIO_WRITE_STAT, sector, num_sectors,
			  response, NULL);
	  if (result == RET_OK)
	    {
	      if (*response == num_sectors)
		{
		  if (quick_packets <= net->quick_packets)
		    ++quick_packets;
		  return (RET_OK); /* write operation has completed */
		}
	      else if (*response == (unsigned long) -1)
		return (RET_SVR_ERR); /* write operation has failed */
	      else if (*response == 0)
		{ /* more data needed; bitmask has been sent */
		  unsigned long tmp[(NET_NUM_SECTORS + 31) / 32];
		  result = recv_exact (net->sock, (void*) tmp, sizeof (tmp), 0);
		  if (result == sizeof (tmp))
		    { /* refresh bitmask */
		      for (i = 0; i < (NET_NUM_SECTORS + 31) / 32; ++i)
			bitmask[i] = get_u32 (tmp + i);
		      --quick_packets;
		    }
		  else
		    return (RET_ERR);
		}
	      else
		return (RET_SVR_ERR); /* protocol error? */
	    }
	}
      while (1);
    }
  return (result);
}


/**************************************************************/
/* execute a command with no reply */
static int
execute (SOCKET s,
	 unsigned long command,
	 unsigned long sector,
	 unsigned long num_sectors)
{
  unsigned char cmd [NET_IO_CMD_LEN + HDD_SECTOR_SIZE * NET_NUM_SECTORS];
  u_int32_t cmd_length = NET_IO_CMD_LEN;
  int result;
  set_u32 (cmd +  0, command);
  set_u32 (cmd +  4, sector);
  set_u32 (cmd +  8, num_sectors);
  set_u32 (cmd + 12, 0x7d3a2d29); /* }:-) */

  result = send_exact (s, (const char*) cmd, cmd_length, 0);
  return (result == cmd_length ? RET_OK : RET_ERR);
}


/**************************************************************/
static int
net_stat (hio_t *hio,
	  u_int32_t *size_in_kb)
{
  hio_net_t *net = (hio_net_t*) hio;
  unsigned long size_in_kb2;
  int result = query (net->sock, CMD_HIO_STAT, 0, 0, &size_in_kb2, NULL);
  if (result == OSAL_OK)
    *size_in_kb = size_in_kb2;
  else
    net->error_code = osal_get_last_error_code ();
  return (result);
}


/**************************************************************/
static int
net_read (hio_t *hio,
	  u_int32_t start_sector,
	  u_int32_t num_sectors,
	  void *output,
	  u_int32_t *bytes)
{
  hio_net_t *net = (hio_net_t*) hio;
  unsigned long response;
  int result;
  char *outp = (char*) output;

  *bytes = 0;
  do
    {
      u_int32_t at_once_s = num_sectors > NET_NUM_SECTORS ? NET_NUM_SECTORS : num_sectors;
      result = query (net->sock, CMD_HIO_READ, start_sector, at_once_s,
		      &response, outp);
      if (result == OSAL_OK)
	{
	  if (response == at_once_s)
	    {
	      start_sector += at_once_s;
	      num_sectors -= at_once_s;
	      *bytes += HDD_SECTOR_SIZE * at_once_s;
	      outp += HDD_SECTOR_SIZE * at_once_s;
	    }
	  else
	    /* server reported an error; give up */
	    result = RET_SVR_ERR;
	}
      else
	net->error_code = osal_get_last_error_code ();
    }
  while (result == OSAL_OK && num_sectors > 0);
  return (result);
}


/**************************************************************/
static int
net_write (hio_t *hio,
	   u_int32_t start_sector,
	   u_int32_t num_sectors,
	   const void *input,
	   u_int32_t *bytes)
{
  hio_net_t *net = (hio_net_t*) hio;
  unsigned long response;
  int result = RET_OK;
  char *inp = (char*) input;

  if (result == RET_OK)
    {
      *bytes = 0;
      do
	{
	  u_int32_t at_once_s = (num_sectors > NET_NUM_SECTORS ?
				 NET_NUM_SECTORS : num_sectors);
	  const void *data_to_send;
	  u_int32_t sectors_to_send;

	  data_to_send = inp;
	  sectors_to_send = at_once_s;

	  result = send_for_write (net, CMD_HIO_WRITE,
				   start_sector, sectors_to_send,
				   &response, data_to_send);
	  if (result == OSAL_OK)
	    {
	      if (response == at_once_s)
		{
		  start_sector += at_once_s;
		  num_sectors -= at_once_s;
		  *bytes += HDD_SECTOR_SIZE * at_once_s;
		  inp += HDD_SECTOR_SIZE * at_once_s;
		}
	      else
		/* server reported an error; give up */
		result = RET_SVR_ERR;
	    }
	  else
	    net->error_code = osal_get_last_error_code ();
	}
      while (result == OSAL_OK && num_sectors > 0);
    }
  return (result);
}


/**************************************************************/
static int
net_poweroff (hio_t *hio)
{
  hio_net_t *net = (hio_net_t*) hio;
  int result = execute (net->sock, CMD_HIO_POWEROFF, 0, 0);
  return (result);
}


/**************************************************************/
static int
net_flush (hio_t *hio)
{
  hio_net_t *net = (hio_net_t*) hio;
  unsigned long response;
  int result = query (net->sock, CMD_HIO_FLUSH, 0, 0, &response, NULL);
  return (result);
}


/**************************************************************/
static int
net_close (hio_t *hio)
{
  hio_net_t *net = (hio_net_t*) hio;
#if defined (_BUILD_WIN32)
  shutdown (net->sock, SD_RECEIVE | SD_SEND);
  closesocket (net->sock);
  closesocket (net->udp);
#elif defined (_BUILD_UNIX)
  close (net->sock);
  close (net->udp);
#endif
  osal_free (hio);

#if defined (_BUILD_WIN32)
  timeEndPeriod (1);
#endif

  return (RET_OK);
}


/**************************************************************/
static char*
net_last_error (hio_t *hio)
{
  hio_net_t *net = (hio_net_t*) hio;
  return (osal_get_error_msg (net->error_code));
}


/**************************************************************/
static void
net_dispose_error (hio_t *hio,
		   char* error)
{
  osal_dispose_error_msg (error);
}


/**************************************************************/
static hio_t*
net_alloc (const dict_t *config,
	   SOCKET tcp,
	   SOCKET udp)
{
  hio_net_t *net = (hio_net_t*) osal_alloc (sizeof (hio_net_t));
  if (net != NULL)
    {
      memset (net, 0, sizeof (hio_net_t));
      net->hio.stat = &net_stat;
      net->hio.read = &net_read;
      net->hio.write = &net_write;
      net->hio.flush = &net_flush;
      net->hio.close = &net_close;
      net->hio.poweroff = &net_poweroff;
      net->hio.last_error = &net_last_error;
      net->hio.dispose_error = &net_dispose_error;
      net->sock = tcp;
      net->udp = udp;

#if defined (_BUILD_WIN32)
      net->quick_packets = dict_get_numeric (config, CONFIG_UDP_QUICK_PACKETS, 5);
      net->delay_time = dict_get_numeric (config, CONFIG_UDP_DELAY_TIME, 2);
#else
      net->quick_packets = dict_get_numeric (config, CONFIG_UDP_QUICK_PACKETS, 7);
      net->delay_time = dict_get_numeric (config, CONFIG_UDP_DELAY_TIME, 1000);
#endif

#if defined (_BUILD_WIN32)
      timeBeginPeriod (1);
#endif
    }
  return ((hio_t*) net);
}


/**************************************************************/
int
hio_udpnet_probe (const dict_t *config,
		  const char *path,
		  hio_t **hio)
{
  int result = RET_NOT_COMPAT;
  char *endp;
  int a, b, c, d;

#if defined (_BUILD_WIN32)
  /* only Windows requires sockets initialization */
  if (!net_init)
    {
      WORD version = MAKEWORD (2, 2);
      WSADATA wsa_data;
      int result = WSAStartup (version, &wsa_data);
      if (result == 0)
	net_init = 1; /* success */
      else
	return (RET_ERR);
    }
#endif

  a = strtol (path, &endp, 10);
  if (a > 0 && a <= 255 && *endp == '.')
    { /* there is a chance */
      b = strtol (endp + 1, &endp, 10);
      if (b >= 0 && b <= 255 && *endp == '.')
	{
	  c = strtol (endp + 1, &endp, 10);
	  if (c >= 0 && c <= 255 && *endp == '.')
	    {
	      d = strtol (endp + 1, &endp, 10);
	      if (d >= 0 && d <= 255 && *endp == '\0')
		{
		  SOCKET s = socket (PF_INET, SOCK_STREAM, 0);
		  if (s != INVALID_SOCKET)
		    {
		      struct sockaddr_in sa;
		      memset (&sa, 0, sizeof (sa));
		      sa.sin_family = AF_INET;
		      sa.sin_addr.s_addr = htonl ((a << 24) | (b << 16) | (c << 8) | (d));
		      sa.sin_port = htons (NET_HIO_SERVER_PORT);
		      result = connect (s, (const struct sockaddr*) &sa,
					sizeof (sa)) == 0 ? RET_OK : RET_ERR;
		      if (result == RET_OK)
			{ /* socket connected */
			  SOCKET udp = socket (PF_INET, SOCK_DGRAM, 0);
			  connect (udp, (const struct sockaddr*) &sa,
				   sizeof (sa));
			  *hio = net_alloc (config, s, udp);
			  if (*hio != NULL)
			    ; /* success */
			  else
			    result = RET_NO_MEM;
			}

		      if (result != RET_OK)
			{ /* close socket on error */
#if defined (_BUILD_WIN32)
			  DWORD err = GetLastError ();
			  shutdown (s, SD_RECEIVE | SD_SEND);
			  closesocket (s);
			  SetLastError (err);
#elif defined (_BUILD_UNIX)
			  int err = errno;
			  close (s);
			  errno = err;
#endif
			}
		    }
		}
	    }
	}
    }
  return (result);
}
