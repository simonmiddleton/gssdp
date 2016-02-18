/*
 * Copyright (C) 2014 Jens Georg <mail@jensge.org>
 *
 * Author: Jens Georg <mail@jensge.org>
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

#ifndef __GSSDP_PKTINFO6_MESSAGE_H__
#define __GSSDP_PKTINFO6_MESSAGE_H__

#include <gio/gio.h>

G_BEGIN_DECLS

G_GNUC_INTERNAL GType
gssdp_pktinfo6_message_get_type (void) G_GNUC_CONST;

#define GSSDP_TYPE_PKTINFO6_MESSAGE (gssdp_pktinfo6_message_get_type())
#define GSSDP_PKTINFO6_MESSAGE(obj) \
                            (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                             GSSDP_TYPE_PKTINFO6_MESSAGE, \
                             GSSDPPktinfo6Message))
#define GSSDP_PKTINFO6_MESAGE_CLASS(klass) \
                            (G_TYPE_CHECK_CLASS_CAST ((klass), \
                             GSSDP_TYPE_PKTINFO6_MESSAGE, \
                             GSSDPPktinfo6Class))
#define GSSDP_IS_PKTINFO6_MESSAGE(obj) \
                            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
                             GSSDP_TYPE_PKTINFO6_MESSAGE))
#define GSSDP_IS_PKTINFO6_MESSAGE_CLASS(klass) \
                            (G_TYPE_CHECK_CLASS_TYPE ((klass), \
                             GSSDP_TYPE_PKTINFO6_MESSAGE))
#define GSSDP_PKTINFO6_MESSAGE_GET_CLASS(obj) \
                            (G_TYPE_INSTANCE_GET_CLASS ((obj), \
                             GSSDP_TYPE_PKTINFO6_MESSAGE, \
                             GSSDPPktinfo6MessageClass))

typedef struct _GSSDPPktinfo6MessagePrivate GSSDPPktinfo6MessagePrivate;
typedef struct _GSSDPPktInfo6Message GSSDPPktinfo6Message;
typedef struct _GSSDPPktinfo6MessageClass GSSDPPktinfo6MessageClass;

struct _GSSDPPktInfo6Message {
        GSocketControlMessage parent;
        GSSDPPktinfo6MessagePrivate *priv;
};

struct _GSSDPPktinfo6MessageClass {
        GSocketControlMessageClass parent_class;
};

G_GNUC_INTERNAL GSocketControlMessage *
gssdp_pktinfo6_message_new (GInetAddress *addr, gint ifindex);

G_GNUC_INTERNAL gint
gssdp_pktinfo6_message_get_ifindex (GSSDPPktinfo6Message *message);

G_GNUC_INTERNAL GInetAddress *
gssdp_pktinfo6_message_get_local_addr (GSSDPPktinfo6Message *message);

#endif /* __GSSDP_PKTINFO6_MESSAGE_H__ */
