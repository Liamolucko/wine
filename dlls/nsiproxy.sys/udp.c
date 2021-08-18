/*
 * nsiproxy.sys udp module
 *
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2021 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "config.h"
#include <stdarg.h>

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_SOCKETVAR_H
#include <sys/socketvar.h>
#endif

#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif

#ifdef HAVE_NETINET_IP_VAR_H
#include <netinet/ip_var.h>
#endif

#ifdef HAVE_NETINET_IN_PCB_H
#include <netinet/in_pcb.h>
#endif

#ifdef HAVE_NETINET_UDP_H
#include <netinet/udp.h>
#endif

#ifdef HAVE_NETINET_UDP_VAR_H
#include <netinet/udp_var.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ifdef.h"
#include "netiodef.h"
#include "ws2ipdef.h"
#include "udpmib.h"
#include "wine/heap.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/server.h"

#include "nsiproxy_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);

static NTSTATUS udp_endpoint_enumerate_all( void *key_data, DWORD key_size, void *rw_data, DWORD rw_size,
                                            void *dynamic_data, DWORD dynamic_size,
                                            void *static_data, DWORD static_size, DWORD_PTR *count )
{
    DWORD num = 0;
    NTSTATUS status = STATUS_SUCCESS;
    BOOL want_data = key_size || rw_size || dynamic_size || static_size;
    struct nsi_udp_endpoint_key key, *key_out = key_data;
    struct nsi_udp_endpoint_static stat, *stat_out = static_data;
    struct ipv6_addr_scope *addr_scopes = NULL;
    unsigned int addr_scopes_size = 0, pid_map_size = 0;
    struct pid_map *pid_map = NULL;

    TRACE( "%p %d %p %d %p %d %p %d %p\n", key_data, key_size, rw_data, rw_size,
           dynamic_data, dynamic_size, static_data, static_size, count );

#ifdef __linux__
    {
        FILE *fp;
        char buf[512], *ptr;
        int inode;

        if (!(fp = fopen( "/proc/net/udp", "r" ))) return ERROR_NOT_SUPPORTED;

        memset( &key, 0, sizeof(key) );
        memset( &stat, 0, sizeof(stat) );
        pid_map = get_pid_map( &pid_map_size );

        /* skip header line */
        ptr = fgets( buf, sizeof(buf), fp );
        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            if (sscanf( ptr, "%*u: %x:%hx %*s %*s %*s %*s %*s %*s %*s %d",
                        &key.local.Ipv4.sin_addr.WS_s_addr, &key.local.Ipv4.sin_port, &inode ) != 3)
                continue;

            key.local.Ipv4.sin_family = WS_AF_INET;
            key.local.Ipv4.sin_port = htons( key.local.Ipv4.sin_port );

            stat.pid = find_owning_pid( pid_map, pid_map_size, inode );
            stat.create_time = 0; /* FIXME */
            stat.flags = 0; /* FIXME */
            stat.mod_info = 0; /* FIXME */

            if (num < *count)
            {
                if (key_out) *key_out++ = key;
                if (stat_out) *stat_out++ = stat;
            }
            num++;
        }
        fclose( fp );

        if ((fp = fopen( "/proc/net/udp6", "r" )))
        {
            memset( &key, 0, sizeof(key) );
            memset( &stat, 0, sizeof(stat) );

            addr_scopes = get_ipv6_addr_scope_table( &addr_scopes_size );

            /* skip header line */
            ptr = fgets( buf, sizeof(buf), fp );
            while ((ptr = fgets( buf, sizeof(buf), fp )))
            {
                DWORD *local_addr = (DWORD *)&key.local.Ipv6.sin6_addr;

                if (sscanf( ptr, "%*u: %8x%8x%8x%8x:%hx %*s %*s %*s %*s %*s %*s %*s %d",
                            local_addr, local_addr + 1, local_addr + 2, local_addr + 3,
                            &key.local.Ipv6.sin6_port, &inode ) != 6)
                    continue;
                key.local.Ipv6.sin6_family = WS_AF_INET6;
                key.local.Ipv6.sin6_port = htons( key.local.Ipv6.sin6_port );
                key.local.Ipv6.sin6_scope_id = find_ipv6_addr_scope( &key.local.Ipv6.sin6_addr, addr_scopes,
                                                                     addr_scopes_size );

                stat.pid = find_owning_pid( pid_map, pid_map_size, inode );
                stat.create_time = 0; /* FIXME */
                stat.flags = 0; /* FIXME */
                stat.mod_info = 0; /* FIXME */

                if (num < *count)
                {
                    if (key_out) *key_out++ = key;
                    if (stat_out) *stat_out++ = stat;
                }
                num++;
            }
            fclose( fp );
        }
    }
#elif defined(HAVE_SYS_SYSCTL_H) && defined(UDPCTL_PCBLIST) && defined(HAVE_STRUCT_XINPGEN)
    {
        int mib[] = { CTL_NET, PF_INET, IPPROTO_UDP, UDPCTL_PCBLIST };
        size_t len = 0;
        char *buf = NULL;
        struct xinpgen *xig, *orig_xig;

        if (sysctl( mib, ARRAY_SIZE(mib), NULL, &len, NULL, 0 ) < 0)
        {
            ERR( "Failure to read net.inet.udp.pcblist via sysctlbyname!\n" );
            status = STATUS_NOT_SUPPORTED;
            goto err;
        }

        buf = heap_alloc( len );
        if (!buf)
        {
            status = STATUS_NO_MEMORY;
            goto err;
        }

        if (sysctl( mib, ARRAY_SIZE(mib), buf, &len, NULL, 0 ) < 0)
        {
            ERR( "Failure to read net.inet.udp.pcblist via sysctlbyname!\n" );
            status = STATUS_NOT_SUPPORTED;
            goto err;
        }

        /* Might be nothing here; first entry is just a header it seems */
        if (len <= sizeof(struct xinpgen)) goto err;

        addr_scopes = get_ipv6_addr_scope_table( &addr_scopes_size );
        pid_map = get_pid_map( &pid_map_size );

        orig_xig = (struct xinpgen *)buf;
        xig = orig_xig;

        for (xig = (struct xinpgen *)((char *)xig + xig->xig_len);
             xig->xig_len > sizeof (struct xinpgen);
             xig = (struct xinpgen *)((char *)xig + xig->xig_len))
        {
#if __FreeBSD_version >= 1200026
            struct xinpcb *in = (struct xinpcb *)xig;
            struct xsocket *sock = &in->xi_socket;
#else
            struct inpcb *in = &((struct xinpcb *)xig)->xi_inp;
            struct xsocket *sock = &((struct xinpcb *)xig)->xi_socket;
#endif
            static const struct in6_addr zero;

            /* Ignore sockets for other protocols */
            if (sock->xso_protocol != IPPROTO_UDP) continue;

            /* Ignore PCBs that were freed while generating the data */
            if (in->inp_gencnt > orig_xig->xig_gen) continue;

            /* we're only interested in IPv4 and IPv6 addresses */
            if (!(in->inp_vflag & (INP_IPV4 | INP_IPV6))) continue;

            /* If all 0's, skip it */
            if (in->inp_vflag & INP_IPV4 && !in->inp_laddr.s_addr) continue;
            if (in->inp_vflag & INP_IPV6 && !memcmp( &in->in6p_laddr, &zero, sizeof(zero) ) && !in->inp_lport) continue;

            if (in->inp_vflag & INP_IPV4)
            {
                key.local.Ipv4.sin_family = WS_AF_INET;
                key.local.Ipv4.sin_addr.WS_s_addr = in->inp_laddr.s_addr;
                key.local.Ipv4.sin_port = in->inp_lport;
            }
            else
            {
                key.local.Ipv6.sin6_family = WS_AF_INET6;
                memcpy( &key.local.Ipv6.sin6_addr, &in->in6p_laddr, sizeof(in->in6p_laddr) );
                key.local.Ipv6.sin6_port = in->inp_lport;
                key.local.Ipv6.sin6_scope_id = find_ipv6_addr_scope( &key.local.Ipv6.sin6_addr, addr_scopes,
                                                                     addr_scopes_size );
            }

            stat.pid = find_owning_pid( pid_map, pid_map_size, (UINT_PTR)sock->so_pcb );
            stat.create_time = 0; /* FIXME */
            stat.flags = !(in->inp_flags & INP_ANONPORT);
            stat.mod_info = 0; /* FIXME */

            if (num < *count)
            {
                if (key_out) *key_out++ = key;
                if (stat_out) *stat_out++ = stat;
            }
            num++;
        }
    err:
        heap_free( buf );
    }
#else
    FIXME( "not implemented\n" );
    return STATUS_NOT_IMPLEMENTED;
#endif

    if (!want_data || num <= *count) *count = num;
    else status = STATUS_MORE_ENTRIES;

    heap_free( pid_map );
    heap_free( addr_scopes );
    return status;
}

static struct module_table udp_tables[] =
{
    {
        NSI_UDP_ENDPOINT_TABLE,
        {
            sizeof(struct nsi_udp_endpoint_key), 0,
            0, sizeof(struct nsi_udp_endpoint_static)
        },
        udp_endpoint_enumerate_all,
    },
    {
        ~0u
    }
};

const struct module udp_module =
{
    &NPI_MS_UDP_MODULEID,
    udp_tables
};
