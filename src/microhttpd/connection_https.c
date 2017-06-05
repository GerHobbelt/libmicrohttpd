/*
     This file is part of libmicrohttpd
     Copyright (C) 2007, 2008, 2010 Daniel Pittman and Christian Grothoff

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Lesser General Public
     License as published by the Free Software Foundation; either
     version 2.1 of the License, or (at your option) any later version.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Lesser General Public License for more details.

     You should have received a copy of the GNU Lesser General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

/**
 * @file connection_https.c
 * @brief  Methods for managing SSL/TLS connections. This file is only
 *         compiled if ENABLE_HTTPS is set.
 * @author Sagie Amir
 * @author Christian Grothoff
 */

#include "internal.h"
#include "connection.h"
#include "connection_https.h"
#include "memorypool.h"
#include "response.h"
#include "mhd_mono_clock.h"
#include <gnutls/gnutls.h>


/**
 * Callback for receiving data from the socket.
 *
 * @param connection the MHD_Connection structure
 * @param other where to write received data to
 * @param i maximum size of other (in bytes)
 * @return positive value for number of bytes actually received or
 *         negative value for error number MHD_ERR_xxx_
 */
static ssize_t
recv_tls_adapter (struct MHD_Connection *connection,
                  void *other,
                  size_t i)
{
  ssize_t res;

  if (i > SSIZE_MAX)
    i = SSIZE_MAX;

  res = gnutls_record_recv (connection->tls_session,
                            other,
                            i);
  if ( (GNUTLS_E_AGAIN == res) ||
       (GNUTLS_E_INTERRUPTED == res) )
    {
#ifdef EPOLL_SUPPORT
      if (GNUTLS_E_AGAIN == res)
        connection->epoll_state &= ~MHD_EPOLL_STATE_READ_READY;
#endif
      return MHD_ERR_AGAIN_;
    }
  if (res < 0)
    {
      /* Likely 'GNUTLS_E_INVALID_SESSION' (client communication
         disrupted); interpret as a hard error */
      connection->tls_read_ready = false;
      return MHD_ERR_CONNRESET_;
    }

#ifdef EPOLL_SUPPORT
  /* If data not available to fill whole buffer - socket is not read ready anymore. */
  if (i > (size_t)res)
    connection->epoll_state &= ~MHD_EPOLL_STATE_READ_READY;
#endif /* EPOLL_SUPPORT */

  /* Check whether TLS buffers still have some unread data. */
  connection->tls_read_ready = ( ((size_t)res == i) &&
                                 (0 != gnutls_record_check_pending (connection->tls_session)) );
  return res;
}


/**
 * Callback for writing data to the socket.
 *
 * @param connection the MHD connection structure
 * @param other data to write
 * @param i number of bytes to write
 * @return positive value for number of bytes actually sent or
 *         negative value for error number MHD_ERR_xxx_
 */
static ssize_t
send_tls_adapter (struct MHD_Connection *connection,
                  const void *other,
                  size_t i)
{
  ssize_t res;

  if (i > SSIZE_MAX)
    i = SSIZE_MAX;

  res = gnutls_record_send (connection->tls_session,
                            other,
                            i);
  if ( (GNUTLS_E_AGAIN == res) ||
       (GNUTLS_E_INTERRUPTED == res) )
    {
      MHD_socket_set_error_ (MHD_SCKT_EINTR_);
#ifdef EPOLL_SUPPORT
      if (GNUTLS_E_AGAIN == res)
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif
      return MHD_ERR_AGAIN_;
    }
  if (res < 0)
    {
      /* Likely 'GNUTLS_E_INVALID_SESSION' (client communication
         disrupted); interpret as a hard error */
      return MHD_ERR_CONNRESET_;
    }
#ifdef EPOLL_SUPPORT
  /* If NOT all available data was sent - socket is not write ready anymore. */
  if (i > (size_t)res)
    connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
  return res;
}


/**
 * Give gnuTLS chance to work on the TLS handshake.
 *
 * @param connection connection to handshake on
 * @return #MHD_YES on error or if the handshake is progressing
 *         #MHD_NO if the handshake has completed successfully
 *         and we should start to read/write data
 */
static int
run_tls_handshake (struct MHD_Connection *connection)
{
  int ret;

  if ((MHD_TLS_CONN_INIT == connection->tls_state) ||
      (MHD_TLS_CONN_HANDSHAKING == connection->tls_state))
    {
      ret = gnutls_handshake (connection->tls_session);
      if (ret == GNUTLS_E_SUCCESS)
	{
	  /* set connection TLS state to enable HTTP processing */
	  connection->tls_state = MHD_TLS_CONN_CONNECTED;
	  MHD_update_last_activity_ (connection);
	  return MHD_NO;
	}
      if ( (GNUTLS_E_AGAIN == ret) ||
	   (GNUTLS_E_INTERRUPTED == ret) )
	{
          connection->tls_state = MHD_TLS_CONN_HANDSHAKING;
	  /* handshake not done */
	  return MHD_YES;
	}
      /* handshake failed */
      connection->tls_state = MHD_TLS_CONN_TLS_FAILED;
#ifdef HAVE_MESSAGES
      MHD_DLOG (connection->daemon,
		_("Error: received handshake message out of context\n"));
#endif
      MHD_connection_close_ (connection,
                             MHD_REQUEST_TERMINATED_WITH_ERROR);
      return MHD_YES;
    }
  return MHD_NO;
}


/**
 * This function handles a particular SSL/TLS connection when
 * it has been determined that there is data to be read off a
 * socket. Message processing is done by message type which is
 * determined by peeking into the first message type byte of the
 * stream.
 *
 * Error message handling: all fatal level messages cause the
 * connection to be terminated.
 *
 * Application data is forwarded to the underlying daemon for
 * processing.
 *
 * @param connection the source connection
 * @return always #MHD_YES (we should continue to process the connection)
 */
static int
MHD_tls_connection_handle_read (struct MHD_Connection *connection)
{
  if (MHD_YES == run_tls_handshake (connection))
    return MHD_YES;
  return MHD_connection_handle_read (connection);
}


/**
 * This function was created to handle writes to sockets when it has
 * been determined that the socket can be written to. This function
 * will forward all write requests to the underlying daemon unless
 * the connection has been marked for closing.
 *
 * @return always #MHD_YES (we should continue to process the connection)
 */
static int
MHD_tls_connection_handle_write (struct MHD_Connection *connection)
{
  if (MHD_YES == run_tls_handshake (connection))
    return MHD_YES;
  return MHD_connection_handle_write (connection);
}


/**
 * Set connection callback function to be used through out
 * the processing of this secure connection.
 *
 * @param connection which callbacks should be modified
 */
void
MHD_set_https_callbacks (struct MHD_Connection *connection)
{
  connection->read_handler = &MHD_tls_connection_handle_read;
  connection->write_handler = &MHD_tls_connection_handle_write;
  connection->recv_cls = &recv_tls_adapter;
  connection->send_cls = &send_tls_adapter;
}


/**
 * Initiate shutdown of TLS layer of connection.
 *
 * @param connection to use
 * @return true if succeed, false otherwise.
 */
bool
MHD_tls_connection_shutdown (struct MHD_Connection *connection)
{
  if (! connection->tls_closed)
    {
      connection->tls_closed =
          (GNUTLS_E_SUCCESS == gnutls_bye(connection->tls_session, GNUTLS_SHUT_WR));
    }
  return connection->tls_closed ? MHD_YES : MHD_NO;;
}

/* end of connection_https.c */
