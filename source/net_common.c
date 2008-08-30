/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
// net.c
//

#include "com_local.h"
#include "files.h"
#include "protocol.h"
#include "q_msg.h"
#include "q_fifo.h"
#include "net_sock.h"
#include "net_stream.h"
#include "sys_public.h"
#include "sv_public.h"

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#define socklen_t int
#define NET_GET_ERROR()   do { net_error = WSAGetLastError(); } while( 0 )
#elif defined( __unix__ )
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>
#include <arpa/inet.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close
#define ioctlsocket ioctl
#define NET_GET_ERROR()   do { net_error = errno; } while( 0 )
#else
#error Unknown target OS
#endif

#if USE_CLIENT

#define	MAX_LOOPBACK	4

typedef struct {
	byte	data[MAX_PACKETLEN];
	size_t	datalen;
} loopmsg_t;

typedef struct {
	loopmsg_t	msgs[MAX_LOOPBACK];
	unsigned	get;
	unsigned	send;
} loopback_t;

static loopback_t	loopbacks[NS_COUNT];

#endif

cvar_t	*net_ip;
cvar_t	*net_port;
#if USE_CLIENT
static cvar_t	*net_clientport;
static cvar_t	*net_dropsim;
#endif
static cvar_t	*net_log_active;
static cvar_t	*net_log_name;
static cvar_t	*net_log_flush;
static cvar_t	*net_ignore_icmp;
static cvar_t	*net_tcp_ip;
static cvar_t	*net_tcp_port;
static cvar_t	*net_tcp_clientip;
static cvar_t	*net_tcp_clientport;
static cvar_t	*net_tcp_backlog;

static SOCKET		udp_sockets[NS_COUNT] = { INVALID_SOCKET, INVALID_SOCKET };
static const char   socketNames[NS_COUNT][8] = { "Client", "Server" };
static SOCKET       tcp_socket = INVALID_SOCKET;

static fileHandle_t	net_logFile;
static netflag_t	net_active;
static int          net_error;

static unsigned     net_statTime;
static size_t       net_rcvd;
static size_t       net_sent;
static size_t       net_rate_dn;
static size_t       net_rate_up;

//=============================================================================

/*
===================
NET_NetadrToSockadr
===================
*/
static void NET_NetadrToSockadr( const netadr_t *a, struct sockaddr_in *s ) {
	memset( s, 0, sizeof( *s ) );

	switch( a->type ) {
	case NA_BROADCAST:
		s->sin_family = AF_INET;
		s->sin_port = a->port;
		s->sin_addr.s_addr = INADDR_BROADCAST;
		break;
	case NA_IP:
		s->sin_family = AF_INET;
		s->sin_addr.s_addr = *( uint32_t * )&a->ip;
		s->sin_port = a->port;
		break;
	default:
		Com_Error( ERR_FATAL, "%s: bad address type", __func__ );
		break;
	}
}

/*
===================
NET_SockadrToNetadr
===================
*/
static void NET_SockadrToNetadr( const struct sockaddr_in *s, netadr_t *a ) {
	memset( a, 0, sizeof( *a ) );

	a->type = NA_IP;
	*( uint32_t * )&a->ip = s->sin_addr.s_addr;
	a->port = s->sin_port;
}

/*
=============
NET_StringToSockaddr

localhost
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
static qboolean NET_StringToSockaddr( const char *s, struct sockaddr_in *sadr ) {
	struct hostent	*h;
	char copy[MAX_QPATH], *p;
    int dots;

	memset( sadr, 0, sizeof( *sadr ) );

	sadr->sin_family = AF_INET;
	sadr->sin_port = 0;

	Q_strncpyz( copy, s, sizeof( copy ) );
	// strip off a trailing :port if present
    p = strchr( copy, ':' );
    if( p ) {
        *p = 0;
        sadr->sin_port = htons( ( u_short )atoi( p + 1 ) );
    }
    for( p = copy, dots = 0; *p; p++ ) {
        if( *p == '.' ) {
            dots++;
        } else if( !Q_isdigit( *p ) ) {
            break;
        }
    }
	if( *p == 0 && dots == 3 ) {
        uint32_t addr = inet_addr( copy );

        if( addr == INADDR_NONE ) {
            return qfalse;
        }
		sadr->sin_addr.s_addr = addr;
	} else {
		if( !( h = gethostbyname( copy ) ) )
			return qfalse;
		sadr->sin_addr.s_addr = *( uint32_t * )h->h_addr_list[0];
	}

	return qtrue;
}


/*
===================
NET_AdrToString
===================
*/
char *NET_AdrToString( const netadr_t *a ) {
	static	char	s[MAX_QPATH];

	switch( a->type ) {
	case NA_LOOPBACK:
		strcpy( s, "loopback" );
		return s;
	case NA_IP:
	case NA_BROADCAST:
		Com_sprintf( s, sizeof( s ), "%u.%u.%u.%u:%u",
            a->ip[0], a->ip[1], a->ip[2], a->ip[3], ntohs( a->port ) );
		return s;
	default:
		Com_Error( ERR_FATAL, "%s: bad address type", __func__ );
		break;
	}

	return NULL;
}

/*
=============
NET_StringToAdr

localhost
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
qboolean NET_StringToAdr( const char *s, netadr_t *a, int port ) {
	struct sockaddr_in sadr;
	
	if( !NET_StringToSockaddr( s, &sadr ) ) {
		return qfalse;
    }
	
	NET_SockadrToNetadr( &sadr, a );

    if( !a->port ) {
        a->port = BigShort( port );
    }

	return qtrue;
}

//=============================================================================

static void NetLogFile_Close( void ) {
	if( !net_logFile ) {
		return;
	}

	Com_Printf( "Closing network log.\n" );

	FS_FCloseFile( net_logFile );
	net_logFile = 0;
}

static void NetLogFile_Open( void ) {
	int mode;

	mode = net_log_active->integer > 1 ? FS_MODE_APPEND : FS_MODE_WRITE;

	if( net_log_flush->integer ) {
		mode |= FS_FLUSH_SYNC;
	}

	FS_FOpenFile( net_log_name->string, &net_logFile, mode );

	if( !net_logFile ) {
		Com_WPrintf( "Couldn't open %s\n", net_log_name->string );
		Cvar_Set( "net_log_active", "0" );
		return;
	}

	Com_Printf( "Logging network packets to %s\n", net_log_name->string );
}

static void net_log_active_changed( cvar_t *self ) {
	if( !self->integer ) {
		NetLogFile_Close();
	} else {
		NetLogFile_Open();
	}	
}	

static void net_log_param_changed( cvar_t *self ) {
	if( net_log_active->integer ) {
		NetLogFile_Close();
		NetLogFile_Open();
	}	
}

/*
=============
NET_LogPacket
=============
*/
static void NET_LogPacket( const netadr_t *address, const char *prefix,
                           const byte *data, size_t length )
{
	int numRows;
	int i, j, c;

	if( !net_logFile ) {
		return;
	}

	FS_FPrintf( net_logFile, "%s : %s\n", prefix, NET_AdrToString( address ) );

	numRows = ( length + 15 ) / 16;
	for( i = 0; i < numRows; i++ ) {
		FS_FPrintf( net_logFile, "%04x : ", i * 16 );
		for( j = 0; j < 16; j++ ) {
			if( i * 16 + j < length ) {
				FS_FPrintf( net_logFile, "%02x ", data[i * 16 + j] );
			} else {
				FS_FPrintf( net_logFile, "   " );
			}
		}
		FS_FPrintf( net_logFile, ": " );
		for( j = 0; j < 16; j++ ) {
			if( i * 16 + j < length ) {
				c = data[i * 16 + j];
				FS_FPrintf( net_logFile, "%c", ( c < 32 || c > 127 ) ? '.' : c );
			} else {
				FS_FPrintf( net_logFile, " " );
			}
		}
		FS_FPrintf( net_logFile, "\n" );
	}

	FS_FPrintf( net_logFile, "\n" );
}

static void NET_UpdateStats( void ) {
    if( net_statTime > com_eventTime ) {
        net_statTime = com_eventTime;
    }
    if( com_eventTime - net_statTime < 1000 ) {
        return;
    }
    net_statTime = com_eventTime;

    net_rate_dn = net_rcvd;
    net_rate_up = net_sent;
    net_sent = 0;
    net_rcvd = 0;
}


#if USE_CLIENT
/*
=============
NET_GetLoopPacket
=============
*/
qboolean NET_GetLoopPacket( netsrc_t sock ) {
	loopback_t	*loop;
	loopmsg_t *loopmsg;

    NET_UpdateStats();

	loop = &loopbacks[sock];

	if( loop->send - loop->get > MAX_LOOPBACK - 1 ) {
		loop->get = loop->send - MAX_LOOPBACK + 1;
	}

	if( loop->get >= loop->send ) {
		return qfalse;
	}

	loopmsg = &loop->msgs[loop->get & (MAX_LOOPBACK-1)];
	loop->get++;

	memcpy( msg_read_buffer, loopmsg->data, loopmsg->datalen );

	if( net_log_active->integer ) {
		NET_LogPacket( &net_from, "LP recv", loopmsg->data, loopmsg->datalen );
	}
    if( sock == NS_CLIENT ) {
        net_rcvd += loopmsg->datalen;
    }

    SZ_Init( &msg_read, msg_read_buffer, sizeof( msg_read_buffer ) );
	msg_read.cursize = loopmsg->datalen;

	return qtrue;
}
#endif

/*
=============
NET_GetPacket

Fills msg_read_buffer with packet contents,
net_from variable receives source address.
=============
*/
neterr_t NET_GetPacket( netsrc_t sock ) {
	struct sockaddr_in from;
	socklen_t fromlen;
	int		ret;

    if( udp_sockets[sock] == INVALID_SOCKET ) {
        return NET_AGAIN;
    }

    NET_UpdateStats();

	fromlen = sizeof( from );
	ret = recvfrom( udp_sockets[sock], msg_read_buffer, MAX_PACKETLEN, 0,
        ( struct sockaddr * )&from, &fromlen );

    if( !ret ) {
        return NET_AGAIN;
    }

	NET_SockadrToNetadr( &from, &net_from );

	if( ret == -1 ) {
        NET_GET_ERROR();

#ifdef _WIN32
		switch( net_error ) {
		case WSAEWOULDBLOCK:
			// wouldblock is silent
			break;
		case WSAECONNRESET:
			Com_DPrintf( "%s: %s from %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( &net_from ) );
			if( !net_ignore_icmp->integer ) {
				return NET_ERROR;
			}
			break;
		case WSAEMSGSIZE:
			Com_WPrintf( "%s: oversize packet from %s\n", __func__,
				NET_AdrToString( &net_from ) );
			break;
		default:
			Com_EPrintf( "%s: %s from %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( &net_from ) );
			break;
		}
#else
		switch( net_error ) {
		case EWOULDBLOCK:
			// wouldblock is silent
			break;
		case ECONNREFUSED:
			Com_DPrintf( "%s: %s from %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( &net_from ) );
			if( !net_ignore_icmp->integer ) {
				return NET_ERROR;
			}
			break;
		default:
			Com_EPrintf( "%s: %s from %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( &net_from ) );
			break;
		}
#endif
		return NET_AGAIN;
	}

	if( net_log_active->integer ) {
		NET_LogPacket( &net_from, "UDP recv", msg_read_buffer, ret );
	}
	
	if( ret > MAX_PACKETLEN ) {
		Com_WPrintf( "%s: oversize packet from %s\n", __func__,
			NET_AdrToString( &net_from ) );
		return NET_AGAIN;
	}

    SZ_Init( &msg_read, msg_read_buffer, sizeof( msg_read_buffer ) );
	msg_read.cursize = ret;
    net_rcvd += ret;

	return NET_OK;
}

//=============================================================================

/*
=============
NET_SendPacket
=============
*/
neterr_t NET_SendPacket( netsrc_t sock, const netadr_t *to, size_t length, const void *data ) {
	struct sockaddr_in	addr;
    int    ret;

	if( length < 1 || length > MAX_PACKETLEN ) {
		Com_WPrintf( "%s: bad length: %"PRIz" bytes\n", __func__, length );
		return NET_ERROR;
	}

	switch( to->type ) {
#if USE_CLIENT
	case NA_LOOPBACK: {
			loopback_t *loop;
			loopmsg_t *msg;

			if( net_dropsim->integer > 0 &&
                ( rand() % 100 ) < net_dropsim->integer )
            {
				return NET_AGAIN;
			}
	
			loop = &loopbacks[sock ^ 1];

			msg = &loop->msgs[loop->send & ( MAX_LOOPBACK - 1 )];
			loop->send++;
			
			memcpy( msg->data, data, length );
			msg->datalen = length;

			if( net_log_active->integer ) {
				NET_LogPacket( to, "LB send", data, length );
			}
            if( sock == NS_CLIENT ) {
                net_sent += length;
            }
		}
		return NET_OK;
#endif
	case NA_IP:
	case NA_BROADCAST:
		break;
	default:
		Com_Error( ERR_FATAL, "%s: bad address type", __func__ );
		break;
	}

    if( udp_sockets[sock] == INVALID_SOCKET ) {
        return NET_AGAIN;
    }

	NET_NetadrToSockadr( to, &addr );

	ret = sendto( udp_sockets[sock], data, length, 0,
        ( struct sockaddr * )&addr, sizeof( addr ) );
	if( ret == -1 ) {
        NET_GET_ERROR();

#ifdef _WIN32
		switch( net_error ) {
		case WSAEWOULDBLOCK:
		case WSAEINTR:
			// wouldblock is silent
			break;
		case WSAECONNRESET:
		case WSAEHOSTUNREACH:
			Com_DPrintf( "%s: %s to %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( to ) );
			if( !net_ignore_icmp->integer ) {
				return NET_ERROR;
			}
			break;
		case WSAEADDRNOTAVAIL:
			// some PPP links do not allow broadcasts
			if( to->type == NA_BROADCAST ) {
				break;
			}
			// intentional fallthrough
		default:
			Com_EPrintf( "%s: %s to %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( to ) );
			break;
		}
#else
		switch( net_error ) {
		case EWOULDBLOCK:
			// wouldblock is silent
			break;
		case ECONNRESET:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ENETDOWN:
			Com_DPrintf( "%s: %s to %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( to ) );
            if( !net_ignore_icmp->integer ) {
                return NET_ERROR;
            }
			break;
		default:
			Com_EPrintf( "%s: %s to %s\n", __func__,
				NET_ErrorString(), NET_AdrToString( to ) );
			break;
		}
#endif
		return NET_AGAIN;
	}

    if( ret != length ) {
		Com_WPrintf( "%s: short send to %s\n", __func__,
            NET_AdrToString( to ) );
    }

	if( net_log_active->integer ) {
		NET_LogPacket( to, "UDP send", data, ret );
	}
    net_sent += ret;

	return NET_OK;
}


//=============================================================================

const char *NET_ErrorString( void ) {
#ifdef _WIN32
	switch( net_error ) {
	case S_OK:
        return "NO ERROR";
	default:
        return "UNKNOWN ERROR";
#include "wsaerr.h"
    }
#else
	return strerror( net_error );
#endif
}

static qboolean NET_StringToIface( const char *s, struct sockaddr_in *sadr ) {
    if( *s ) {
        return NET_StringToSockaddr( s, sadr );
    }

    // empty string binds to all interfaces 
    memset( sadr, 0, sizeof( *sadr ) );
    sadr->sin_family = AF_INET;
    sadr->sin_addr.s_addr = INADDR_ANY;
    return qtrue;
}

static SOCKET UDP_OpenSocket( const char *interface, int port ) {
	SOCKET				newsocket;
	struct sockaddr_in	address;
	u_long			    _true = 1;

	Com_DPrintf( "Opening UDP socket: %s:%i\n", interface, port );

    newsocket = socket( PF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( newsocket == INVALID_SOCKET ) {
        NET_GET_ERROR();
		return INVALID_SOCKET;
	}

	// make it non-blocking
	if( ioctlsocket( newsocket, FIONBIO, &_true ) == -1 ) {
		goto fail;
	}

	// make it broadcast capable
    _true = 1;
	if( setsockopt( newsocket, SOL_SOCKET, SO_BROADCAST,
        ( char * )&_true, sizeof( _true ) ) == -1 )
    {
		goto fail;
	}

    if( !NET_StringToIface( interface, &address ) ) {
        Com_Printf( "Bad interface address: %s\n", interface );
        goto fail;
    }

	if( port != PORT_ANY ) {
		address.sin_port = htons( ( u_short )port );
	}

	if( !bind( newsocket, ( struct sockaddr * )&address, sizeof( address ) ) ) {
	    return newsocket;
    }

fail:
    NET_GET_ERROR();
	closesocket( newsocket );
    Com_EPrintf( "%s: %s:%d: %s\n", __func__,
        interface, port, NET_ErrorString() );
	return INVALID_SOCKET;
}

static SOCKET TCP_OpenSocket( const char *interface, int port, netsrc_t who ) {
	SOCKET				newsocket;
	struct sockaddr_in	address;
	u_long			    _true;

	Com_DPrintf( "Opening TCP socket: %s:%i\n", interface, port );

    newsocket = socket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
	if( newsocket == INVALID_SOCKET ) {
        NET_GET_ERROR();
		return INVALID_SOCKET;
	}

	// make it non-blocking
    _true = 1;
	if( ioctlsocket( newsocket, FIONBIO, &_true ) == -1 ) {
		goto fail;
	}

    // give it a chance to reuse previous port
    if( who == NS_SERVER ) {
        _true = 1;
        if( setsockopt( newsocket, SOL_SOCKET, SO_REUSEADDR,
                ( char * )&_true, sizeof( _true ) ) == -1 )
        {
            goto fail;
        }
    }

    if( !NET_StringToIface( interface, &address ) ) {
        Com_Printf( "Bad interface address: %s\n", interface );
        goto fail;
    }
	if( port != PORT_ANY ) {
		address.sin_port = htons( ( u_short )port );
	}

	if( !bind( newsocket, ( struct sockaddr * )&address, sizeof( address ) ) ) {
	    return newsocket;
    }

fail:
    NET_GET_ERROR();
	closesocket( newsocket );
    Com_EPrintf( "%s: %s:%d: %s\n", __func__,
        interface, port, NET_ErrorString() );
	return INVALID_SOCKET;
}

static void NET_OpenServer( void ) {
	int i, port;

	for( i = 0, port = net_port->integer; i < PORT_MAX_SEARCH; i++, port++ ) {
		udp_sockets[NS_SERVER] = UDP_OpenSocket( net_ip->string, port );
		if( udp_sockets[NS_SERVER] != INVALID_SOCKET ) {
            if( i > 0 ) {
                // warn if bound to different port
                Com_WPrintf( "Server bound to UDP port %d.\n", port );
                Cvar_SetInteger( net_port, port, CVAR_SET_DIRECT );
            }
            // set this for compatibility with game mods
            Cvar_Set( "port", net_port->string );
            return;
        }

        // continue searching for a free port if this one was in use
#ifdef _WIN32
        if( net_error != WSAEADDRINUSE )
#else
        if( net_error != EADDRINUSE )
#endif
            break;
	}

#if USE_CLIENT
	if( !dedicated->integer ) {
        Com_WPrintf( "Couldn't open server UDP port.\n" );
        return;
    }
#endif

	Com_Error( ERR_FATAL, "Couldn't open dedicated server UDP port" );
}

#if USE_CLIENT
static void NET_OpenClient( void ) {
    struct sockaddr_in address;
    socklen_t length;

	udp_sockets[NS_CLIENT] = UDP_OpenSocket( net_ip->string,
        net_clientport->integer );
	if( udp_sockets[NS_CLIENT] != INVALID_SOCKET ) {
		return;
	}

	if( net_clientport->integer != PORT_ANY ) {
		udp_sockets[NS_CLIENT] = UDP_OpenSocket( net_ip->string, PORT_ANY );
		if( udp_sockets[NS_CLIENT] != INVALID_SOCKET ) {
            length = sizeof( address );
            getsockname( udp_sockets[NS_CLIENT],
                ( struct sockaddr * )&address, &length );
            Com_WPrintf( "Client bound to UDP port %d.\n",
                ntohs( address.sin_port ) );
		    Cvar_SetByVar( net_clientport, va( "%d", PORT_ANY ), CVAR_SET_DIRECT );
			return;
		}
	}

    Com_WPrintf( "Couldn't open client UDP port.\n" );
}
#endif

//=============================================================================

void NET_Close( netstream_t *s ) {
    if( !s->state ) {
        return;
    }

    closesocket( s->socket );
    s->socket = INVALID_SOCKET;
    s->state = NS_DISCONNECTED;
}

neterr_t NET_Listen( qboolean arg ) {
    if( !arg ) {
        if( tcp_socket != INVALID_SOCKET ) {
            closesocket( tcp_socket );
            tcp_socket = INVALID_SOCKET;
        }
        return NET_OK;
    }

    if( tcp_socket != INVALID_SOCKET ) {
        return NET_OK;
    }

    // zero TCP port means use default server port
	tcp_socket = TCP_OpenSocket( net_tcp_ip->string,
        net_tcp_port->integer, NS_SERVER );
    if( tcp_socket == INVALID_SOCKET ) {
        return NET_ERROR;
    }
    if( listen( tcp_socket, net_tcp_backlog->integer ) == -1 ) {
        NET_GET_ERROR();
        closesocket( tcp_socket );
        tcp_socket = INVALID_SOCKET;
        return NET_ERROR;
    }

    return NET_OK;
}

neterr_t NET_Accept( netadr_t *peer, netstream_t *s ) {
	struct sockaddr_in from;
    socklen_t fromlen;
	u_long _true = 1;
    SOCKET newsocket;
    struct timeval tv;
    fd_set fd;
    int ret;

    if( tcp_socket == INVALID_SOCKET ) {
        return NET_AGAIN;
    }

    FD_ZERO( &fd );
    FD_SET( tcp_socket, &fd );
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    ret = select( tcp_socket + 1, &fd, NULL, NULL, &tv );
    if( ret == -1 ) {
        NET_GET_ERROR();
        return NET_ERROR;
    }

    if( !ret ) {
        return NET_AGAIN;
    }

	fromlen = sizeof( from );
    newsocket = accept( tcp_socket, ( struct sockaddr * )&from, &fromlen );
    if( newsocket == -1 ) {
        NET_GET_ERROR();
        return NET_ERROR;
    }

	// make it non-blocking
	if( ioctlsocket( newsocket, FIONBIO, &_true ) == -1 ) {
        NET_GET_ERROR();
        closesocket( newsocket );
        return NET_ERROR;
	}

	NET_SockadrToNetadr( &from, peer );

    memset( s, 0, sizeof( *s ) );
    s->socket = newsocket;
    s->address = *peer;
    s->state = NS_CONNECTED;

    return NET_OK;
}

neterr_t NET_Connect( const netadr_t *peer, netstream_t *s ) {
    SOCKET socket;
    struct sockaddr_in address;
    int ret;

	socket = TCP_OpenSocket( net_tcp_clientip->string,
        net_tcp_clientport->integer, NS_CLIENT );
    if( socket == INVALID_SOCKET ) {
        return NET_ERROR;
    }
    NET_NetadrToSockadr( peer, &address );

    memset( s, 0, sizeof( *s ) );

    ret = connect( socket, ( struct sockaddr * )&address, sizeof( address ) );
    if( ret == -1 ) {
        NET_GET_ERROR();

#ifdef _WIN32
        if( net_error != WSAEWOULDBLOCK ) {
#else
        if( net_error != EINPROGRESS ) {
#endif
            closesocket( socket );
            return NET_ERROR;
        }
    }

    s->state = NS_CONNECTING;
    s->address = *peer;
    s->socket = socket;

    return NET_OK;
}

neterr_t NET_Run( netstream_t *s ) {
    struct timeval tv;
    fd_set rfd, wfd, efd;
    int ret, err;
    size_t length;
    byte *data;
    neterr_t result = NET_AGAIN;

    if( s->state < NS_CONNECTING || s->state > NS_CONNECTED ) {
        return result;
    }

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO( &rfd );
    FD_SET( s->socket, &rfd );
    FD_ZERO( &wfd );
    FD_SET( s->socket, &wfd );
    FD_ZERO( &efd );
    FD_SET( s->socket, &efd );
	ret = select( s->socket + 1, &rfd, &wfd, &efd, &tv );
    if( ret == -1 ) {
        goto error;
    }

    if( !ret ) {
        return result;
    }

    result = NET_AGAIN;
    if( s->state == NS_CONNECTING ) {
        socklen_t length;

        if( !FD_ISSET( s->socket, &wfd ) && !FD_ISSET( s->socket, &efd ) ) {
            return result;
        }

        length = sizeof( err );
        ret = getsockopt( s->socket, SOL_SOCKET, SO_ERROR,
			( char * )&err, &length );
        if( ret == -1 ) {
            goto error;
        }

        if( err ) {
            net_error = err;
            s->state = NS_BROKEN;
            return NET_ERROR;
        }

        s->state = NS_CONNECTED;
        result = NET_OK;
    }

    if( FD_ISSET( s->socket, &rfd ) ) {
        // read as much as we can
        data = FIFO_Reserve( &s->recv, &length );
        if( length ) {
            ret = recv( s->socket, data, length, 0 );
            if( !ret ) {
                goto closed;
            }
            if( ret == -1 ) {
                goto error;
            }

            FIFO_Commit( &s->recv, ret );

            if( net_log_active->integer ) {
                NET_LogPacket( &s->address, "TCP recv", data, ret );
            }
            net_rcvd += ret;

            result = NET_OK;
        }
    }

    if( FD_ISSET( s->socket, &wfd ) ) {
        // write as much as we can
        data = FIFO_Peek( &s->send, &length );
        if( length ) {
            ret = send( s->socket, data, length, 0 );
            if( !ret ) {
                goto closed;
            }
            if( ret == -1 ) {
                goto error;
            }

            FIFO_Decommit( &s->send, ret );

            if( net_log_active->integer ) {
                NET_LogPacket( &s->address, "TCP send", data, ret );
            }
            net_sent += ret;

            result = NET_OK;
        }
    }

    return result;

closed:
    s->state = NS_CLOSED;
    return NET_CLOSED;

error:
    NET_GET_ERROR();
    s->state = NS_BROKEN;
    return NET_ERROR;
}

/*
====================
NET_Sleep

sleeps msec or until server UDP socket is ready
====================
*/
void NET_Sleep( int msec ) {
    struct timeval timeout;
	fd_set	fdset;
	SOCKET s = udp_sockets[NS_SERVER];

	if( s == INVALID_SOCKET ) {
	    s = udp_sockets[NS_CLIENT];
	    if( s == INVALID_SOCKET ) {
            Sys_Sleep( msec );
		    return;
        }
	}

	timeout.tv_sec = msec / 1000;
	timeout.tv_usec = ( msec % 1000 ) * 1000;
	FD_ZERO( &fdset );
#ifdef __unix__
    if( sys_stdio->integer ) {
    	FD_SET( 0, &fdset ); // stdin
    }
#endif
	FD_SET( s, &fdset );
	select( s + 1, &fdset, NULL, NULL, &timeout );
}

//===================================================================

/*
====================
NET_DumpHostInfo
====================
*/
static void NET_DumpHostInfo( struct hostent *h ) {
	byte **list;
	int i;

	Com_Printf( "Hostname: %s\n", h->h_name );

	list = ( byte ** )h->h_aliases;
	for( i = 0; list[i]; i++ ) {
		Com_Printf( "Alias   : %s\n", list[i] );
	}

	list = ( byte ** )h->h_addr_list;
	for( i = 0; list[i]; i++ ) {
		Com_Printf( "IP      : %u.%u.%u.%u\n",
			list[i][0], list[i][1], list[i][2], list[i][3] );
	}
}

static void dump_socket( SOCKET s, const char *s1, const char *s2 ) {
    struct sockaddr_in sockaddr;
    socklen_t len;
    netadr_t adr;

    len = sizeof( sockaddr );
    if( getsockname( s, ( struct sockaddr * )&sockaddr, &len ) == -1 ) {
        NET_GET_ERROR();
        Com_EPrintf( "%s: getsockname: %s\n", __func__, NET_ErrorString() );
        return;
    }
    NET_SockadrToNetadr( &sockaddr, &adr );
    Com_Printf( "%s %s socket bound to %s\n", s1, s2, NET_AdrToString( &adr ) );
}

/*
====================
NET_ShowIP_f
====================
*/
static void NET_ShowIP_f( void ) {
	char buffer[256];
	struct hostent *h;
    netsrc_t sock;

	if( gethostname( buffer, sizeof( buffer ) ) == -1 ) {
        NET_GET_ERROR();
		Com_EPrintf( "%s: gethostname: %s\n", __func__, NET_ErrorString() );
		return;
	}

	if( !( h = gethostbyname( buffer ) ) ) {
        NET_GET_ERROR();
		Com_EPrintf( "%s: gethostbyname: %s\n", __func__, NET_ErrorString() );
		return;
	}

	NET_DumpHostInfo( h );

	for( sock = 0; sock < NS_COUNT; sock++ ) {
    	if( udp_sockets[sock] != INVALID_SOCKET ) {
            dump_socket( udp_sockets[sock], socketNames[sock], "UDP" );
        }
    }
    if( tcp_socket != INVALID_SOCKET ) {
        dump_socket( tcp_socket, socketNames[NS_SERVER], "TCP" );
    }
}

/*
====================
NET_Dns_f
====================
*/
static void NET_Dns_f( void ) {
	char buffer[MAX_QPATH];
	char *p;
	struct hostent *h;
	u_long address;

	if( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: %s <address>\n", Cmd_Argv( 0 ) );
		return;
	}

	Cmd_ArgvBuffer( 1, buffer, sizeof( buffer ) );

	if( ( p = strchr( buffer, ':' ) ) != NULL ) {
		*p = 0;
	}

	if( ( address = inet_addr( buffer ) ) != INADDR_NONE ) {
		h = gethostbyaddr( ( const char * )&address, sizeof( address ), AF_INET );
	} else {
		h = gethostbyname( buffer );
	}

	if( !h ) {
		Com_Printf( "Couldn't resolve %s\n", buffer );
		return;
	}

	NET_DumpHostInfo( h );
	
}

/*
====================
NET_Restart_f
====================
*/
static void NET_Restart_f( void ) {
	netflag_t flag = net_active;
    SOCKET sock = tcp_socket;

    if( sock != INVALID_SOCKET ) {
        NET_Listen( qfalse );
    }
	NET_Config( NET_NONE );
	NET_Config( flag );
    if( sock != INVALID_SOCKET ) {
        NET_Listen( qtrue );
    }

    SV_SetConsoleTitle();
}

/*
====================
NET_Config
====================
*/
void NET_Config( netflag_t flag ) {
	netsrc_t sock;

	if( flag == net_active ) {
		return;
	}

	if( flag == NET_NONE ) {
		// shut down any existing sockets
		for( sock = 0; sock < NS_COUNT; sock++ ) {
			if( udp_sockets[sock] != INVALID_SOCKET ) {
				closesocket( udp_sockets[sock] );
				udp_sockets[sock] = INVALID_SOCKET;
			}
		}
		net_active = NET_NONE;
		return;
	}

#if USE_CLIENT
	if( flag & NET_CLIENT ) {
		if( udp_sockets[NS_CLIENT] == INVALID_SOCKET ) {
			NET_OpenClient();
		}
	}
#endif

	if( flag & NET_SERVER ) {
		if( udp_sockets[NS_SERVER] == INVALID_SOCKET ) {
			NET_OpenServer();
		}
	}

	net_active |= flag;
}

qboolean NET_GetAddress( netsrc_t sock, netadr_t *adr ) {
    struct sockaddr_in address;
    socklen_t length;

    if( udp_sockets[sock] == INVALID_SOCKET ) {
        return qfalse;
    }

    length = sizeof( address );
    if( getsockname( udp_sockets[sock], ( struct sockaddr * )
        &address, &length ) == -1 )
    {
        return qfalse;
    }

    NET_SockadrToNetadr( &address, adr );

    return qtrue;
}

static size_t NET_UpRate_m( char *buffer, size_t size ) {
	return Com_sprintf( buffer, size, "%"PRIz, net_rate_up );
}

static size_t NET_DnRate_m( char *buffer, size_t size ) {
	return Com_sprintf( buffer, size, "%"PRIz, net_rate_dn );
}

static void net_udp_param_changed( cvar_t *self ) {
    // keep TCP socket vars in sync unless modified by user
    if( !( net_tcp_ip->flags & CVAR_MODIFIED ) ) {
        Cvar_SetByVar( net_tcp_ip, net_ip->string, CVAR_SET_DIRECT );
    }
    if( !( net_tcp_port->flags & CVAR_MODIFIED ) ) {
        Cvar_SetByVar( net_tcp_port, net_port->string, CVAR_SET_DIRECT );
    }

    NET_Restart_f();
}

static void net_tcp_param_changed( cvar_t *self ) {
    if( tcp_socket != INVALID_SOCKET ) {
        NET_Listen( qfalse );
        NET_Listen( qtrue );
    }
}

/*
====================
NET_Init
====================
*/
void NET_Init( void ) {
    int i;
#ifdef _WIN32
	WSADATA		ws;

	i = WSAStartup( MAKEWORD( 1, 1 ), &ws );
	if( i ) {
		Com_Error( ERR_FATAL, "Winsock initialization failed, returned %d", i );
	}

	Com_DPrintf( "Winsock Initialized\n" );
#endif

	net_ip = Cvar_Get( "net_ip", "", 0 );
    net_ip->changed = net_udp_param_changed;
	net_port = Cvar_Get( "net_port", PORT_SERVER_STRING, 0 );
    net_port->changed = net_udp_param_changed;
#if USE_CLIENT
	net_clientport = Cvar_Get( "net_clientport", PORT_ANY_STRING, 0 );
    net_clientport->changed = net_udp_param_changed;
	net_dropsim = Cvar_Get( "net_dropsim", "0", 0 );
#endif
	net_log_active = Cvar_Get( "net_log_active", "0", 0 );
	net_log_active->changed = net_log_active_changed;
	net_log_name = Cvar_Get( "net_log_name", "qnetwork.log", 0 );
	net_log_name->changed = net_log_param_changed;
	net_log_flush = Cvar_Get( "net_log_flush", "0", 0 );
	net_log_flush->changed = net_log_param_changed;
	net_ignore_icmp = Cvar_Get( "net_ignore_icmp", "0", 0 );

    if( ( i = Cvar_VariableInteger( "ip_hostport" ) ) ||
        ( i = Cvar_VariableInteger( "hostport" ) ) ||
        ( i = Cvar_VariableInteger( "port" ) ) )
    {
        Com_Printf( "NOTICE: net_port overriden to %d "
            "by deprecated cvar\n", i );
        Cvar_SetInteger( net_port, i, CVAR_SET_DIRECT );
    }

#if USE_CLIENT
    if( ( i = Cvar_VariableInteger( "ip_clientport" ) ) ||
        ( i = Cvar_VariableInteger( "clientport" ) ) )
    {
        Com_Printf( "NOTICE: net_clientport overriden to %d "
            "by deprecated cvar\n", i );
        Cvar_SetInteger( net_clientport, i, CVAR_SET_DIRECT );
    }
#endif

	net_tcp_ip = Cvar_Get( "net_tcp_ip", net_ip->string, 0 );
    net_tcp_ip->changed = net_tcp_param_changed;
	net_tcp_port = Cvar_Get( "net_tcp_port", net_port->string, 0 );
    net_tcp_port->changed = net_tcp_param_changed;
	net_tcp_clientip = Cvar_Get( "net_tcp_clientip", "", 0 );
	net_tcp_clientport = Cvar_Get( "net_tcp_clientport", PORT_ANY_STRING, 0 );
	net_tcp_backlog = Cvar_Get( "net_tcp_backlog", "4", 0 );

    if( net_log_active->integer ) {
		NetLogFile_Open();
    }

    net_statTime = com_eventTime;

	Cmd_AddCommand( "net_restart", NET_Restart_f );
	Cmd_AddCommand( "showip", NET_ShowIP_f );
	Cmd_AddCommand( "dns", NET_Dns_f );

    Cmd_AddMacro( "net_uprate", NET_UpRate_m );
    Cmd_AddMacro( "net_dnrate", NET_DnRate_m );
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown( void ) {
	if( net_logFile ) {
		FS_FCloseFile( net_logFile );
		net_logFile = 0;
	}

    NET_Listen( qfalse );
	NET_Config( NET_NONE );

#ifdef _WIN32
	WSACleanup();
#endif

	Cmd_RemoveCommand( "net_restart" );
	Cmd_RemoveCommand( "showip" );
	Cmd_RemoveCommand( "dns" );
}

