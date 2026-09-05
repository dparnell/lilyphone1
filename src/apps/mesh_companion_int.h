#ifndef __MESH_COMPANION_INT_H__
#define __MESH_COMPANION_INT_H__

/* The seam between the node (mesh_net.cpp) and the companion protocol
 * (mesh_companion.cpp).
 *
 * Everything here runs on the mesh task and touches MeshCore, which is why it
 * is not in the public header: the UI must not be able to reach any of it.
 */
#include <helpers/BaseChatMesh.h>

/* Hands the protocol the node it speaks for. Until this is called every command
 * is answered with an error, which is what a client sees while the radio is
 * still coming up. */
void companion_attach(BaseChatMesh *chat);

/* One pass of the link: a frame in, a frame out, or a step of a contacts
 * listing. Called from the mesh task loop. */
void companion_service(void);

/* An acknowledgement arrived. Returns the contact it was from when it answers
 * something the app sent, or NULL when it is not one of ours - in which case
 * the node's own send is the one being answered. */
ContactInfo *companion_process_ack(BaseChatMesh *chat, const uint8_t *data);

/* Somebody was heard from, or a route to them changed. */
void companion_on_advert(const ContactInfo &contact, bool is_new);
void companion_on_path_updated(const ContactInfo &contact);

/* Something was said. Held for the app to collect with SYNC_NEXT_MESSAGE, so
 * that a message is not lost while the app is away. */
void companion_queue_msg(const ContactInfo &from, mesh::Packet *pkt,
                         uint32_t sender_timestamp, uint8_t txt_type, const char *text);
void companion_queue_channel_msg(int channel_idx, mesh::Packet *pkt,
                                 uint32_t timestamp, const char *text);

#endif
