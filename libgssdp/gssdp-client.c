/* 
 * Copyright (C) 2006, 2007, 2008 OpenedHand Ltd.
 * Copyright (C) 2009 Nokia Corporation.
 * Copyright (C) 2013 Intel Corporation.
 *
 * Author: Jorn Baayen <jorn@openedhand.com>
 *         Zeeshan Ali (Khattak) <zeeshanak@gnome.org>
 *                               <zeeshan.ali@nokia.com>
 *         Jens Georg <jensg@openismus.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gssdp-client
 * @short_description: SSDP "bus" wrapper.
 *
 * #GSSDPClient wraps the SSDP "bus" as used by both #GSSDPResourceBrowser
 * and #GSSDPResourceGroup.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "gssdp-client.h"
#include "gssdp-client-private.h"
#include "gssdp-error.h"
#include "gssdp-socket-source.h"
#include "gssdp-protocol.h"
#include "gssdp-net.h"
#include "gssdp-socket-functions.h"
#ifdef HAVE_PKTINFO
#include "gssdp-pktinfo-message.h"
#endif

#include <sys/types.h>
#include <glib.h>
#ifdef G_OS_WIN32
#include <winsock2.h>
#else
#include <sys/utsname.h>
#include <arpa/inet.h>
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <libsoup/soup-headers.h>

/* Size of the buffer used for reading from the socket */
#define BUF_SIZE 65536

/* interface index for loopback device */
#define LOOPBACK_IFINDEX 1

static void
gssdp_client_initable_iface_init (gpointer g_iface,
                                  gpointer iface_data);

struct _GSSDPClientPrivate {
        char              *server_id;

        GHashTable        *user_agent_cache;
        guint              socket_ttl;
        guint              msearch_port;
        GSSDPNetworkDevice device;
        GList             *headers;

        GSSDPSocketSource *request_socket;
        GSSDPSocketSource *multicast_socket;
        GSSDPSocketSource *search_socket;

        gboolean           active;
        gboolean           initialized;
};
typedef struct _GSSDPClientPrivate GSSDPClientPrivate;

G_DEFINE_TYPE_EXTENDED (GSSDPClient,
                        gssdp_client,
                        G_TYPE_OBJECT,
                        0,
                        G_ADD_PRIVATE(GSSDPClient)
                        G_IMPLEMENT_INTERFACE
                                (G_TYPE_INITABLE,
                                 gssdp_client_initable_iface_init));

struct _GSSDPHeaderField {
        char *name;
        char *value;
};
typedef struct _GSSDPHeaderField GSSDPHeaderField;

enum {
        PROP_0,
        PROP_SERVER_ID,
        PROP_IFACE,
        PROP_NETWORK,
        PROP_HOST_IP,
        PROP_ACTIVE,
        PROP_SOCKET_TTL,
        PROP_MSEARCH_PORT,
};

enum {
        MESSAGE_RECEIVED,
        LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static char *
make_server_id                (void);
static gboolean
request_socket_source_cb      (GIOChannel   *source,
                               GIOCondition  condition,
                               gpointer      user_data);
static gboolean
multicast_socket_source_cb    (GIOChannel   *source,
                               GIOCondition  condition,
                               gpointer      user_data);
static gboolean
search_socket_source_cb       (GIOChannel   *source,
                               GIOCondition  condition,
                               gpointer      user_data);

static gboolean
init_network_info             (GSSDPClient  *client,
                               GError      **error);

static gboolean
gssdp_client_initable_init    (GInitable     *initable,
                               GCancellable  *cancellable,
                               GError       **error);

static void
gssdp_client_init (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        priv->active = TRUE;

        /* Generate default server ID */
        priv->server_id = make_server_id ();
}

static void
gssdp_client_initable_iface_init (gpointer               g_iface,
                                  G_GNUC_UNUSED gpointer iface_data)
{
        GInitableIface *iface = (GInitableIface *)g_iface;
        iface->init = gssdp_client_initable_init;
}

static gboolean
gssdp_client_initable_init (GInitable                   *initable,
                            G_GNUC_UNUSED GCancellable  *cancellable,
                            GError                     **error)
{
        GSSDPClient *client = GSSDP_CLIENT (initable);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);
        GError *internal_error = NULL;

        if (priv->initialized)
                return TRUE;

        if (!gssdp_net_init (error))
                return FALSE;

        /* Make sure all network info is available to us */
        if (!init_network_info (client, &internal_error))
                goto errors;

        /* Set up sockets (Will set errno if it failed) */
        priv->request_socket =
                gssdp_socket_source_new (GSSDP_SOCKET_SOURCE_TYPE_REQUEST,
                                         gssdp_client_get_host_ip (client),
                                         priv->socket_ttl,
                                         priv->device.iface_name,
                                         &internal_error);
        if (priv->request_socket != NULL) {
                gssdp_socket_source_set_callback
                        (priv->request_socket,
                        (GSourceFunc) request_socket_source_cb,
                        client);
        } else {
                goto errors;
        }

        priv->multicast_socket =
                gssdp_socket_source_new (GSSDP_SOCKET_SOURCE_TYPE_MULTICAST,
                                         gssdp_client_get_host_ip (client),
                                         priv->socket_ttl,
                                         priv->device.iface_name,
                                         &internal_error);
        if (priv->multicast_socket != NULL) {
                gssdp_socket_source_set_callback
                        (priv->multicast_socket,
                         (GSourceFunc) multicast_socket_source_cb,
                         client);
        } else {
                goto errors;
        }

        /* Setup send socket. For security reasons, it is not recommended to
         * send M-SEARCH with source port == SSDP_PORT */
        priv->search_socket = GSSDP_SOCKET_SOURCE (g_initable_new
                                        (GSSDP_TYPE_SOCKET_SOURCE,
                                         NULL,
                                         &internal_error,
                                         "type", GSSDP_SOCKET_SOURCE_TYPE_SEARCH,
                                         "host-ip", gssdp_client_get_host_ip (client),
                                         "ttl", priv->socket_ttl,
                                         "port", priv->msearch_port,
                                         "device-name", priv->device.iface_name,
                                         NULL));

        if (priv->search_socket != NULL) {
                gssdp_socket_source_set_callback
                                        (priv->search_socket,
                                         (GSourceFunc) search_socket_source_cb,
                                         client);
        }
 errors:
        if (!priv->request_socket ||
            !priv->multicast_socket ||
            !priv->search_socket) {
                g_propagate_error (error, internal_error);

                g_clear_object (&priv->request_socket);
                g_clear_object (&priv->multicast_socket);
                g_clear_object (&priv->search_socket);

                return FALSE;
        }

        gssdp_socket_source_attach (priv->request_socket);
        gssdp_socket_source_attach (priv->multicast_socket);
        gssdp_socket_source_attach (priv->search_socket);

        priv->initialized = TRUE;

        priv->user_agent_cache = g_hash_table_new_full (g_str_hash,
                                                        g_str_equal,
                                                        g_free,
                                                        g_free);

        return TRUE;
}

static void
gssdp_client_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
        GSSDPClient *client = GSSDP_CLIENT (object);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        switch (property_id) {
        case PROP_SERVER_ID:
                g_value_set_string
                        (value,
                         gssdp_client_get_server_id (client));
                break;
        case PROP_IFACE:
                g_value_set_string (value,
                                    gssdp_client_get_interface (client));
                break;
        case PROP_NETWORK:
                g_value_set_string (value,
                                    gssdp_client_get_network (client));
                break;
        case PROP_HOST_IP:
                g_value_set_string (value,
                                    gssdp_client_get_host_ip (client));
                break;
        case PROP_ACTIVE:
                g_value_set_boolean (value, priv->active);
                break;
        case PROP_SOCKET_TTL:
                g_value_set_uint (value, priv->socket_ttl);
                break;
        case PROP_MSEARCH_PORT:
                g_value_set_uint (value, priv->msearch_port);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gssdp_client_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
        GSSDPClient *client = GSSDP_CLIENT (object);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        switch (property_id) {
        case PROP_SERVER_ID:
                gssdp_client_set_server_id (client,
                                            g_value_get_string (value));
                break;
        case PROP_IFACE:
                priv->device.iface_name = g_value_dup_string (value);
                break;
        case PROP_NETWORK:
                priv->device.network = g_value_dup_string (value);
                break;
        case PROP_HOST_IP:
                priv->device.host_ip = g_value_dup_string (value);
                break;
        case PROP_ACTIVE:
                priv->active = g_value_get_boolean (value);
                break;
        case PROP_SOCKET_TTL:
                priv->socket_ttl = g_value_get_uint (value);
                break;
        case PROP_MSEARCH_PORT:
                priv->msearch_port = g_value_get_uint (value);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gssdp_client_dispose (GObject *object)
{
        GSSDPClient *client = GSSDP_CLIENT (object);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        /* Destroy the SocketSources */
        g_clear_object (&priv->request_socket);
        g_clear_object (&priv->multicast_socket);
        g_clear_object (&priv->search_socket);
        g_clear_object (&priv->device.host_addr);

        G_OBJECT_CLASS (gssdp_client_parent_class)->dispose (object);
}

static void
gssdp_client_finalize (GObject *object)
{
        GSSDPClient *client = GSSDP_CLIENT (object);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        gssdp_net_shutdown ();

        g_clear_pointer (&priv->server_id, g_free);
        g_clear_pointer (&priv->device.iface_name, g_free);
        g_clear_pointer (&priv->device.host_ip, g_free);
        g_clear_pointer (&priv->device.network, g_free);

        g_clear_pointer (&priv->user_agent_cache, g_hash_table_unref);

        G_OBJECT_CLASS (gssdp_client_parent_class)->finalize (object);
}

static void
gssdp_client_class_init (GSSDPClientClass *klass)
{
        GObjectClass *object_class;

        object_class = G_OBJECT_CLASS (klass);

        object_class->set_property = gssdp_client_set_property;
        object_class->get_property = gssdp_client_get_property;
        object_class->dispose      = gssdp_client_dispose;
        object_class->finalize     = gssdp_client_finalize;

        /**
         * GSSDPClient:server-id:
         *
         * The SSDP server's identifier.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_SERVER_ID,
                 g_param_spec_string
                         ("server-id",
                          "Server ID",
                          "The SSDP server's identifier.",
                          NULL,
                          G_PARAM_READWRITE |
                          G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:interface:
         *
         * The name of the network interface this client is associated with.
         * Set to NULL to autodetect.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_IFACE,
                 g_param_spec_string
                         ("interface",
                          "Network interface",
                          "The name of the associated network interface.",
                          NULL,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_NAME |
                          G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:network:
         *
         * The network this client is currently connected to. You could set this
         * to anything you want to identify the network this client is
         * associated with. If you are using #GUPnPContextManager and associated
         * interface is a WiFi interface, this property is set to the ESSID of
         * the network. Otherwise, expect this to be the network IP address by
         * default.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_NETWORK,
                 g_param_spec_string
                         ("network",
                          "Network ID",
                          "The network this client is currently connected to.",
                          NULL,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT |
                          G_PARAM_STATIC_NAME |
                          G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:host-ip:
         *
         * The IP address of the assoicated network interface.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_HOST_IP,
                 g_param_spec_string ("host-ip",
                                      "Host IP",
                                      "The IP address of the associated"
                                      "network interface",
                                      NULL,
                                      G_PARAM_READWRITE |
                                      G_PARAM_CONSTRUCT |
                                      G_PARAM_STATIC_NAME |
                                      G_PARAM_STATIC_NICK |
                                      G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:active:
         *
         * Whether this client is active or not (passive). When active
         * (default), the client sends messages on the network, otherwise
         * not. In most cases, you don't want to touch this property.
         *
         **/
        g_object_class_install_property
                (object_class,
                 PROP_ACTIVE,
                 g_param_spec_boolean
                         ("active",
                          "Active",
                          "TRUE if the client is active.",
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_STATIC_NAME |
                          G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:socket-ttl:
         *
         * Time-to-live value to use for all sockets created by this client.
         * If not set (or set to 0) the value recommended by UPnP will be used.
         * This property can only be set during object construction.
         */
        g_object_class_install_property
                (object_class,
                 PROP_SOCKET_TTL,
                 g_param_spec_uint
                        ("socket-ttl",
                         "Socket TTL",
                         "Time To Live for client's sockets",
                         0, 255,
                         0,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK |
                         G_PARAM_STATIC_BLURB));

        /**
         * GSSDPClient:msearch-port:
         *
         * UDP port to use for sending multicast M-SEARCH requests on the
         * network. If not set (or set to 0) a random port will be used.
         * This property can be only set during object construction.
         */
        g_object_class_install_property
                (object_class,
                 PROP_MSEARCH_PORT,
                 g_param_spec_uint
                        ("msearch-port",
                         "M-SEARCH port",
                         "UDP port to use for M-SEARCH requests",
                         0, G_MAXUINT16,
                         0,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS));

        /**
         * GSSDPClient::message-received: (skip)
         * @client: The #GSSDPClient that received the message.
         * @from_ip: The IP address of the source.
         * @from_port: The UDP port used by the sender.
         * @type: The #_GSSDPMessageType.
         * @headers: (type SoupMessageHeaders): Parsed #SoupMessageHeaders from the message.
         *
         * Internal signal.
         *
         * Stability: Private
         **/
        signals[MESSAGE_RECEIVED] =
                g_signal_new ("message-received",
                              GSSDP_TYPE_CLIENT,
                              G_SIGNAL_RUN_LAST,
                              0,
                              NULL, NULL, NULL,
                              G_TYPE_NONE,
                              4,
                              G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE,
                              G_TYPE_UINT,
                              G_TYPE_INT,
                              G_TYPE_POINTER);
}

/**
 * gssdp_client_new:
 * @iface: (allow-none): The name of the network interface, or %NULL for auto-detection.
 * @error: Location to store error, or NULL
 *
 * Return value: A new #GSSDPClient object.
 **/
GSSDPClient *
gssdp_client_new (const char *iface, GError **error)
{
        return g_initable_new (GSSDP_TYPE_CLIENT,
                               NULL,
                               error,
                               "interface", iface,
                               NULL);
}

/**
 * gssdp_client_new_with_port:
 * @iface: (allow-none): The name of the network interface, or %NULL for
 * auto-detection.
 * @msearch_port: The network port to use for M-SEARCH requests or 0 for
 * random.
 * @error: (allow-none): Location to store error, or %NULL.
 *
 * Return value: A new #GSSDPClient object.
 **/
GSSDPClient *
gssdp_client_new_with_port (const char *iface,
                            guint16     msearch_port,
                            GError    **error)
{
        return g_initable_new (GSSDP_TYPE_CLIENT,
                               NULL,
                               error,
                               "interface", iface,
                               "msearch-port", msearch_port,
                               NULL);
}

/**
 * gssdp_client_set_server_id:
 * @client: A #GSSDPClient
 * @server_id: The server ID
 *
 * Sets the server ID of @client to @server_id.
 **/
void
gssdp_client_set_server_id (GSSDPClient *client,
                            const char  *server_id)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        priv = gssdp_client_get_instance_private (client);

        g_clear_pointer (&priv->server_id, g_free);

        if (server_id)
                priv->server_id = g_strdup (server_id);

        g_object_notify (G_OBJECT (client), "server-id");
}

/**
 * gssdp_client_get_server_id:
 * @client: A #GSSDPClient
 *
 * Return value: The server ID.
 **/
const char *
gssdp_client_get_server_id (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_val_if_fail (GSSDP_IS_CLIENT (client), NULL);
        priv = gssdp_client_get_instance_private (client);

        return priv->server_id;
}

/**
 * gssdp_client_get_interface:
 * @client: A #GSSDPClient
 *
 * Get the name of the network interface associated to @client.
 *
 * Return value: The network interface name. This string should not be freed.
 **/
const char *
gssdp_client_get_interface (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_val_if_fail (GSSDP_IS_CLIENT (client), NULL);
        priv = gssdp_client_get_instance_private (client);


        return priv->device.iface_name;
}

/**
 * gssdp_client_get_host_ip:
 * @client: A #GSSDPClient
 *
 * Get the IP address we advertise ourselves as using.
 *
 * Return value: The IP address. This string should not be freed.
 **/
const char *
gssdp_client_get_host_ip (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_val_if_fail (GSSDP_IS_CLIENT (client), NULL);
        priv = gssdp_client_get_instance_private (client);

        return priv->device.host_ip;
}

/**
 * gssdp_client_set_network:
 * @client: A #GSSDPClient
 * @network: The string identifying the network
 *
 * Sets the network identification of @client to @network.
 **/
void
gssdp_client_set_network (GSSDPClient *client,
                          const char  *network)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        priv = gssdp_client_get_instance_private (client);

        g_clear_pointer (&priv->device.network, g_free);

        if (network)
                priv->device.network = g_strdup (network);

        g_object_notify (G_OBJECT (client), "network");
}

/**
 * gssdp_client_add_cache_entry:
 * @client: A #GSSDPClient
 * @ip_address: The host to add to the cache
 * @user_agent: User agent ot the host to add
 **/
void
gssdp_client_add_cache_entry (GSSDPClient  *client,
                               const char   *ip_address,
                               const char   *user_agent)
{
        GSSDPClientPrivate *priv = NULL;
        char *hwaddr = NULL;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        g_return_if_fail (ip_address != NULL);
        g_return_if_fail (user_agent != NULL);

        priv = gssdp_client_get_instance_private (client);

        hwaddr = gssdp_net_arp_lookup (&priv->device, ip_address);

        if (hwaddr)
                g_hash_table_insert (priv->user_agent_cache,
                                     hwaddr,
                                     g_strdup (user_agent));
}

/**
 * gssdp_client_guess_user_agent:
 * @client: A #GSSDPClient
 * @ip_address: IP address to guess the user-agent for
 *
 * Returns: (transfer none): The user-agent cached for this IP, %NULL if none
 * is cached.
 **/
const char *
gssdp_client_guess_user_agent (GSSDPClient *client,
                               const char  *ip_address)
{
        GSSDPClientPrivate *priv = NULL;
        char *hwaddr = NULL;

        g_return_val_if_fail (GSSDP_IS_CLIENT (client), NULL);
        g_return_val_if_fail (ip_address != NULL, NULL);

        priv = gssdp_client_get_instance_private (client);

        hwaddr = gssdp_net_arp_lookup (&priv->device, ip_address);

        if (hwaddr) {
                const char *agent;

                agent =  g_hash_table_lookup (priv->user_agent_cache,
                                              hwaddr);
                g_free (hwaddr);

                return agent;
        }

        return NULL;
}

/**
 * gssdp_client_get_network:
 * @client: A #GSSDPClient
 *
 * Get the network this client is associated with.
 *
 * Return value: The network identification. This string should not be freed.
 **/
const char *
gssdp_client_get_network (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;
        g_return_val_if_fail (GSSDP_IS_CLIENT (client), NULL);

        priv = gssdp_client_get_instance_private (client);

        return priv->device.network;
}

/**
 * gssdp_client_get_active:
 * @client: A #GSSDPClient
 *
 * Return value: %TRUE if @client is active, %FALSE otherwise.
 **/
gboolean
gssdp_client_get_active (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;
        g_return_val_if_fail (GSSDP_IS_CLIENT (client), FALSE);

        priv = gssdp_client_get_instance_private (client);

        return priv->active;
}

static void
header_field_free (GSSDPHeaderField *header)
{
        g_free (header->name);
        g_free (header->value);
        g_slice_free (GSSDPHeaderField, header);
}

static gchar *
append_header_fields (GList *headers, const gchar *message)
{
        GString *str = NULL;
        GList *iter = NULL;

        str = g_string_new (message);

        for (iter = headers; iter; iter = iter->next) {
                GSSDPHeaderField *header = (GSSDPHeaderField *) iter->data;
                g_string_append_printf (str, "%s: %s\r\n",
                                        header->name,
                                        header->value ? header->value : "");
        }

        g_string_append (str, "\r\n");

        return g_string_free (str, FALSE);
}

/**
 * gssdp_client_append_header:
 * @client: A #GSSDPClient
 * @name: Header name
 * @value: Header value
 *
 * Adds a header field to the message sent by this @client. It is intended to
 * be used by clients requiring vendor specific header fields. (If there is an
 * existing header with name name , then this creates a second one).
 **/
void
gssdp_client_append_header (GSSDPClient *client,
                            const char  *name,
                            const char  *value)
{
        GSSDPHeaderField *header = NULL;
        GSSDPClientPrivate *priv = NULL;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        g_return_if_fail (name != NULL);

        priv = gssdp_client_get_instance_private (client);

        header = g_slice_new (GSSDPHeaderField);
        header->name = g_strdup (name);
        header->value = g_strdup (value);
        priv->headers = g_list_append (priv->headers, header);
}

/**
 * gssdp_client_remove_header:
 * @client: A #GSSDPClient
 * @name: Header name
 *
 * Removes @name from the list of headers . If there are multiple values for
 * @name, they are all removed.
 **/
void
gssdp_client_remove_header (GSSDPClient *client,
                            const char  *name)
{
        GSSDPClientPrivate *priv;
        GList *l;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        g_return_if_fail (name != NULL);

        priv = gssdp_client_get_instance_private (client);
        l = priv->headers;
        while (l != NULL)
        {
                GList *next = l->next;
                GSSDPHeaderField *header = l->data;

                if (!g_strcmp0 (header->name, name)) {
                        header_field_free (header);
                        priv->headers = g_list_delete_link (priv->headers, l);
                }
                l = next;
        }
}

/**
 * gssdp_client_clear_headers:
 * @client: A #GSSDPClient
 *
 * Removes all the headers for this @client.
 **/
void
gssdp_client_clear_headers (GSSDPClient *client)
{
        GSSDPClientPrivate *priv = NULL;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        priv = gssdp_client_get_instance_private (client);

        g_list_free_full (priv->headers,
                          (GDestroyNotify) header_field_free);
}

/**
 * _gssdp_client_send_message:
 * @client: A #GSSDPClient
 * @dest_ip: (allow-none): The destination IP address, or %NULL to broadcast
 * @dest_port: (allow-none): The destination port, or %NULL for default
 * @message: The message to send
 *
 * Sends @message to @dest_ip.
 **/
void
_gssdp_client_send_message (GSSDPClient      *client,
                            const char       *dest_ip,
                            gushort           dest_port,
                            const char       *message,
                            _GSSDPMessageType type)
{
        GSSDPClientPrivate *priv = NULL;
        gssize res;
        GError *error = NULL;
        GInetAddress *inet_address = NULL;
        GSocketAddress *address = NULL;
        GSocket *socket;
        char *extended_message;

        g_return_if_fail (GSSDP_IS_CLIENT (client));
        g_return_if_fail (message != NULL);

        priv = gssdp_client_get_instance_private (client);

        if (!priv->active)
                /* We don't send messages in passive mode */
                return;

        /* Broadcast if @dest_ip is NULL */
        if (dest_ip == NULL)
                dest_ip = SSDP_ADDR;

        /* Use default port if no port was explicitly specified */
        if (dest_port == 0)
                dest_port = SSDP_PORT;

        if (type == _GSSDP_DISCOVERY_REQUEST)
                socket = gssdp_socket_source_get_socket
                                        (priv->search_socket);
        else
                socket = gssdp_socket_source_get_socket
                                        (priv->request_socket);

        inet_address = g_inet_address_new_from_string (dest_ip);
        address = g_inet_socket_address_new (inet_address, dest_port);
        extended_message = append_header_fields (priv->headers, message);

        res = g_socket_send_to (socket,
                                address,
                                extended_message,
                                strlen (extended_message),
                                NULL,
                                &error);

        if (res == -1) {
                g_warning ("Error sending SSDP packet to %s: %s",
                           dest_ip,
                           error->message);
                g_error_free (error);
        }

        g_free (extended_message);
        g_object_unref (address);
        g_object_unref (inet_address);
}

/*
 * Generates the default server ID
 */
static char *
make_server_id (void)
{
#ifdef G_OS_WIN32
        OSVERSIONINFO versioninfo;
        versioninfo.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
        if (GetVersionEx (&versioninfo)) {
                return g_strdup_printf ("Microsoft Windows/%ld.%ld GSSDP/%s",
                                        versioninfo.dwMajorVersion,
                                        versioninfo.dwMinorVersion,
                                        VERSION);
        } else {
                return g_strdup_printf ("Microsoft Windows GSSDP/%s",
                                        VERSION);
        }
#else
        struct utsname sysinfo;

        uname (&sysinfo);
        
        return g_strdup_printf ("%s/%s GSSDP/%s",
                                sysinfo.sysname,
                                sysinfo.version,
                                VERSION);
#endif
}

static gboolean
parse_http_request (char                *buf,
                    int                  len,
                    SoupMessageHeaders **headers,
                    int                 *type)
{
        char *req_method = NULL;
        char *path = NULL;
        SoupHTTPVersion version;

        *headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);

        if (soup_headers_parse_request (buf,
                                        len,
                                        *headers,
                                        &req_method,
                                        &path,
                                        &version) == SOUP_STATUS_OK &&
            version == SOUP_HTTP_1_1 &&
            (path && g_ascii_strncasecmp (path, "*", 1) == 0)) {
                if (g_ascii_strncasecmp (req_method,
                                         SSDP_SEARCH_METHOD,
                                         strlen (SSDP_SEARCH_METHOD)) == 0)
                        *type = _GSSDP_DISCOVERY_REQUEST;
                else if (g_ascii_strncasecmp (req_method,
                                              GENA_NOTIFY_METHOD,
                                              strlen (GENA_NOTIFY_METHOD)) == 0)
                        *type = _GSSDP_ANNOUNCEMENT;
                else
                        g_warning ("Unhandled method '%s'", req_method);

                g_free (req_method);
                g_free (path);

                return TRUE;
        } else {
                soup_message_headers_free (*headers);
                *headers = NULL;

                g_free (path);
                g_free (req_method);

                return FALSE;
        }
}

static gboolean
parse_http_response (char                *buf,
                    int                  len,
                    SoupMessageHeaders **headers,
                    int                 *type)
{
        guint status_code;

        *headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);

        if (soup_headers_parse_response (buf,
                                         len,
                                         *headers,
                                         NULL,
                                         &status_code,
                                         NULL) &&
            status_code == 200) {
                *type = _GSSDP_DISCOVERY_RESPONSE;

                return TRUE;
        } else {
                soup_message_headers_free (*headers);
                *headers = NULL;

                return FALSE;
        }
}

/*
 * Called when data can be read from the socket
 */
static gboolean
socket_source_cb (GSSDPSocketSource *socket_source, GSSDPClient *client)
{
        int type, len;
        char buf[BUF_SIZE], *end;
        SoupMessageHeaders *headers = NULL;
        GSocket *socket;
        GSocketAddress *address = NULL;
        gssize bytes;
        GInetAddress *inetaddr;
        char *ip_string = NULL;
        guint16 port;
        GError *error = NULL;
        GInputVector vector;
        GSocketControlMessage **messages;
        gint num_messages;
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        vector.buffer = buf;
        vector.size = BUF_SIZE;

        /* Get Socket */
        socket = gssdp_socket_source_get_socket (socket_source);
        bytes = g_socket_receive_message (socket,
                                          &address,
                                          &vector,
                                          1,
                                          &messages,
                                          &num_messages,
                                          NULL,
                                          NULL,
                                          &error);

        if (bytes == -1) {
                g_warning ("Failed to receive from socket: %s", error->message);

                goto out;
        }

#if defined(HAVE_PKTINFO) && !defined(__APPLE__)
        {
                int i;
                for (i = 0; i < num_messages; i++) {
                        GSSDPPktinfoMessage *msg;
                        gint msg_ifindex;

                        if (!GSSDP_IS_PKTINFO_MESSAGE (messages[i]))
                                continue;

                        msg = GSSDP_PKTINFO_MESSAGE (messages[i]);
                        msg_ifindex = gssdp_pktinfo_message_get_ifindex (msg);
                        /* message needs to be on correct interface or on
                         * loopback (as kernel can be smart and route things
                         * there even if sent to another network) */
                        if (!((msg_ifindex == priv->device.index ||
                               msg_ifindex == LOOPBACK_IFINDEX) &&
                              (g_inet_address_equal (gssdp_pktinfo_message_get_local_addr (msg),
                                                     priv->device.host_addr))))
                                goto out;
                        else
                                break;
                }
        }
#else
        /* We need the following lines to make sure the right client received
         * the packet. We won't need to do this if there was any way to tell
         * Mr. Unix that we are only interested in receiving multicast packets
         * on this socket from a particular interface but AFAIK that is not
         * possible, at least not in a portable way.
         */
        {
                struct sockaddr_in addr;
                in_addr_t mask;
                in_addr_t our_addr;
                if (!g_socket_address_to_native (address,
                                                 &addr,
                                                 sizeof (struct sockaddr_in),
                                                 &error)) {
                        g_warning ("Could not convert address to native: %s",
                                   error->message);

                        goto out;
                }

                mask = priv->device.mask.sin_addr.s_addr;
                our_addr = inet_addr (gssdp_client_get_host_ip (client));

                if ((addr.sin_addr.s_addr & mask) != (our_addr & mask))
                        goto out;

        }
#endif

        if (bytes >= BUF_SIZE) {
                g_warning ("Received packet of %" G_GSSIZE_FORMAT " bytes, "
                           "but the maximum buffer size is %d. Packed dropped.",
                           bytes, BUF_SIZE);

                goto out;
        }

        /* Add trailing \0 */
        buf[bytes] = '\0';

        /* Find length */
        end = strstr (buf, "\r\n\r\n");
        if (!end) {
                g_debug ("Received packet lacks \"\\r\\n\\r\\n\" sequence. "
                         "Packed dropped.");

                goto out;
        }

        len = end - buf + 2;

        /* Parse message */
        type = -1;
        headers = NULL;

        if (!parse_http_request (buf,
                                 len,
                                 &headers,
                                 &type)) {
                if (!parse_http_response (buf,
                                          len,
                                          &headers,
                                          &type)) {
                        g_debug ("Unhandled packet '%s'", buf);
                }
        }

        /* Emit signal if parsing succeeded */
        inetaddr = g_inet_socket_address_get_address (
                                        G_INET_SOCKET_ADDRESS (address));
        ip_string = g_inet_address_to_string (inetaddr);
        port = g_inet_socket_address_get_port (
                                        G_INET_SOCKET_ADDRESS (address));

        if (type >= 0) {
                const char *agent;

                /* update client cache */
                agent = soup_message_headers_get_one (headers, "Server");
                if (!agent)
                        agent = soup_message_headers_get_one (headers, "User-Agent");

                if (agent)
                        gssdp_client_add_cache_entry (client,
                                                      ip_string,
                                                      agent);

                g_signal_emit (client,
                               signals[MESSAGE_RECEIVED],
                               0,
                               ip_string,
                               port,
                               type,
                               headers);
        }

out:
        if (error)
                g_error_free (error);

        g_free (ip_string);

        if (headers)
                soup_message_headers_free (headers);

        if (address)
                g_object_unref (address);

        if (messages) {
                int i;
                for (i = 0; i < num_messages; i++)
                        g_object_unref (messages[i]);

                g_free (messages);
        }

        return TRUE;
}

static gboolean
request_socket_source_cb (G_GNUC_UNUSED GIOChannel  *source,
                          G_GNUC_UNUSED GIOCondition condition,
                          gpointer                   user_data)
{
        GSSDPClient *client = GSSDP_CLIENT (user_data);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        return socket_source_cb (priv->request_socket, client);
}

static gboolean
multicast_socket_source_cb (G_GNUC_UNUSED GIOChannel  *source,
                            G_GNUC_UNUSED GIOCondition condition,
                            gpointer                   user_data)
{
        GSSDPClient *client = GSSDP_CLIENT (user_data);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        return socket_source_cb (priv->multicast_socket, client);
}

static gboolean
search_socket_source_cb (G_GNUC_UNUSED GIOChannel  *source,
                         G_GNUC_UNUSED GIOCondition condition,
                         gpointer                   user_data)
{
        GSSDPClient *client = GSSDP_CLIENT (user_data);
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);

        return socket_source_cb (priv->search_socket, client);
}

static gboolean
init_network_info (GSSDPClient *client, GError **error)
{
        GSSDPClientPrivate *priv = gssdp_client_get_instance_private (client);
        gboolean ret = TRUE;

        /* Either interface name or host_ip wasn't given during construction.
         * If one is given, try to find the other, otherwise just pick an
         * interface.
         */
        if (priv->device.iface_name == NULL ||
            priv->device.host_ip == NULL)
                gssdp_net_get_host_ip (&(priv->device));
        else {
                /* Ugly. Ideally, get_host_ip needs to be run everytime, but
                 * it is currently to stupid so just query index here if we
                 * have a name and an interface already.
                 *
                 * query_ifindex will return -1 on platforms that don't
                 * support this.
                 */
                priv->device.index =
                        gssdp_net_query_ifindex (&priv->device);
        }

        if (priv->device.host_addr == NULL &&
            priv->device.host_ip != NULL) {
                priv->device.host_addr =
                                g_inet_address_new_from_string
                                    (priv->device.host_ip);
        }

        if (priv->device.iface_name == NULL) {
                g_set_error_literal (error,
                                     GSSDP_ERROR,
                                     GSSDP_ERROR_FAILED,
                                     "No default route?");

                ret = FALSE;
        } else if (priv->device.host_ip == NULL) {
                        g_set_error (error,
                                     GSSDP_ERROR,
                                     GSSDP_ERROR_NO_IP_ADDRESS,
                                     "Failed to find IP of interface %s",
                                     priv->device.iface_name);

                ret = FALSE;
        }

        return ret;
}

